/*
 * engine_menu.cpp -- the port's in-game engine menu and performance overlay.
 *
 * Open and close with L3 + R3 together (or F1 on the keyboard). D-pad /
 * left stick (or the arrow keys) move, Cross / A (or Enter) changes the
 * highlighted setting, Circle / B, Start (or Escape) closes. While the menu
 * is open the game sees an idle controller.
 *
 * Drawn with Dear ImGui (src/third_party/imgui, MIT) through the live-draw
 * renderer's overlay hook (rsx_live_draw_set_overlay, patches/0031): the
 * hook runs on the present thread with the frame already in the backbuffer,
 * so nothing here touches the game's own rendering. Settings live in
 * engine_settings.cpp.
 *
 * Adding a setting: see engine_settings.h, then a widget in drawMenu().
 */
#include "engine_settings.h"
#include "engine_saves.h"
#include "engine_portable.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "imgui.h"
#include "backends/imgui_impl_dx12.h"

#pragma comment(lib, "xinput.lib")

extern "C"
{
    typedef void (*rsx_live_overlay_fn)(void* device, void* queue, void* list,
                                        unsigned long long rtv, unsigned format,
                                        uint32_t width, uint32_t height);
    void rsx_live_draw_set_overlay(rsx_live_overlay_fn fn);
    uint32_t rsx_live_draw_get_render_scale(void);
    float rsx_live_draw_get_aspect(void);
    int rsx_null_backend_is_fullscreen(void);
    void rsx_null_backend_set_fullscreen(int enabled);
    void rsx_live_draw_get_shader_stats(uint32_t* pending, uint32_t* built, uint32_t* skippedDraws);
    extern volatile int g_cellpad_block_guest_input;
    /* DualShock 3 over HID (cellPad.c): buttons in CELL_PAD packing. */
    int cellPad_host_ds3_state(uint16_t* buttons, uint8_t* stickLeftX, uint8_t* stickLeftY, int* psButton);
    int cellPad_motion_live(void);
    /* [fps] breakdown counters (cellGcmSys.c), nanoseconds since start. */
    extern volatile uint64_t g_gcm_perf_main_usleep_ns;
    extern volatile uint64_t g_gcm_perf_present_idle_ns;
    extern volatile uint64_t g_gcm_perf_gpu_wait_ns;
    extern volatile uint64_t g_gcm_perf_waitflip_ns;
    extern volatile uint64_t g_gcm_perf_ring_wait_ns;
    extern volatile uint64_t g_gcm_perf_drain_ns;
    extern volatile uint64_t g_gcm_perf_present_ns;
}

namespace
{
    /* The renderer keeps up to two frames in flight (frame pipelining), so
     * ImGui's per-frame buffers need one more than that. */
    constexpr int kFramesInFlight = 3;

    struct ControllerState
    {
        bool connected = false;
        WORD buttons = 0;
        SHORT leftStickX = 0;
        SHORT leftStickY = 0;
        bool psButton = false;
    };

    struct MenuState
    {
        bool initialized = false;
        bool failed = false;
        bool menuOpen = false;
        bool focusRequested = false;
        ID3D12DescriptorHeap* srvHeap = nullptr;
        LARGE_INTEGER frequency = {};
        LARGE_INTEGER lastFrame = {};
        WORD previousButtons = 0;
        bool previousToggleKey = false;
        bool previousCaptureKey = false;
        bool captureActive = false;
        LARGE_INTEGER captureStart = {};
        bool previousEscape = false;
        /* Performance overlay, refreshed twice a second. */
        double frameSecondsAverage = 0.0;
        double windowSeconds = 0.0;
        int windowFrames = 0;
        double shownFps = 0.0;
        double shownFrameMs = 0.0;
        double shownMainSleepMs = 0.0;
        double shownRenderIdleMs = 0.0;
        double shownGpuWaitMs = 0.0;
        double shownRendererWaitMs = 0.0;
        double shownDrainMs = 0.0;
        double shownPresentMs = 0.0;
        uint64_t lastSleepNs = 0;
        uint64_t lastIdleNs = 0;
        uint64_t lastGpuNs = 0;
        uint64_t lastWaitFlipNs = 0;
        uint64_t lastRingWaitNs = 0;
        uint64_t lastDrainNs = 0;
        uint64_t lastPresentNs = 0;
        /* Save data snapshots (engine_saves.h), refreshed when the menu
         * opens and after each action. */
        std::vector<SaveSnapshot> snapshots;
        int gameSaveCount = 0;
        std::string saveMessage;
        std::string pendingRestore;
        std::string gameFolderMessage;
    };

    MenuState menu;

    /* The game-folder picker runs on its own thread: it is a modal Windows
     * dialog, and on the present thread it would stop the game drawing. */
    struct FolderPickState
    {
        std::mutex lock;
        bool running = false;
        bool done = false;
        std::string folder;
        std::string problem;
    };

    FolderPickState folderPick;

    void startGameFolderPick(const std::string& startFolder)
    {
        {
            std::lock_guard<std::mutex> guard(folderPick.lock);
            if (folderPick.running)
            {
                return;
            }
            folderPick.running = true;
            folderPick.done = false;
        }
        /* A borderless fullscreen window would cover the dialog: step out of
         * fullscreen while it is open. */
        const bool wasFullscreen = rsx_null_backend_is_fullscreen() != 0;
        std::thread([startFolder, wasFullscreen]() {
            if (wasFullscreen)
            {
                rsx_null_backend_set_fullscreen(0);
            }
            const std::string picked = enginePickGameFolder(startFolder);
            std::string problem;
            if (!picked.empty())
            {
                engineCheckGameFolder(picked, &problem);
            }
            if (wasFullscreen)
            {
                rsx_null_backend_set_fullscreen(1);
            }
            std::lock_guard<std::mutex> guard(folderPick.lock);
            folderPick.folder = picked;
            folderPick.problem = problem;
            folderPick.running = false;
            folderPick.done = true;
        }).detach();
    }

    /* Where the game is read from and where its installed data goes. A new
     * game folder is used from the next start (the running game keeps the
     * files it opened). */
    bool drawGameFiles()
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;
        bool pickRunning = false;
        {
            std::lock_guard<std::mutex> guard(folderPick.lock);
            pickRunning = folderPick.running;
            if (folderPick.done)
            {
                folderPick.done = false;
                if (!folderPick.folder.empty() && folderPick.problem.empty())
                {
                    settings.gameDir = folderPick.folder;
                    menu.gameFolderMessage = "Saved: the game uses this folder the next time you start it.";
                    changed = true;
                }
                else if (!folderPick.problem.empty())
                {
                    menu.gameFolderMessage = folderPick.problem;
                }
            }
        }
        ImGui::SeparatorText("Game files");
        ImGui::TextWrapped("Game folder: %s",
                           settings.gameDir.empty() ? "(passed in by run.ps1)" : settings.gameDir.c_str());
        {
            const char* installedData = std::getenv("PS3_HDD1_ROOT");
            char workingDirectory[MAX_PATH * 2] = {};
            GetCurrentDirectoryA((DWORD)sizeof(workingDirectory), workingDirectory);
            ImGui::TextWrapped("Installed game data: %s", installedData ? installedData : "(not set)");
            ImGui::TextWrapped("Saves: %s\\gamedata", workingDirectory);
        }
        ImGui::BeginDisabled(pickRunning);
        if (ImGui::Button(pickRunning ? "Choosing a folder..." : "Change game folder..."))
        {
            startGameFolderPick(settings.gameDir);
        }
        ImGui::EndDisabled();
        if (!menu.gameFolderMessage.empty())
        {
            ImGui::TextWrapped("%s", menu.gameFolderMessage.c_str());
        }
        return changed;
    }

    /* A DualShock 3 read over HID (DsHidMini SXS mode) is not an XInput pad;
     * give the menu its buttons in XInput terms. */
    SHORT ds3AxisToXInput(uint8_t value, bool invert)
    {
        int centred = (int)value - 128;
        if (invert)
        {
            centred = -centred;
        }
        int scaled = centred * 256;
        if (scaled > 32767)
        {
            scaled = 32767;
        }
        if (scaled < -32768)
        {
            scaled = -32768;
        }
        return (SHORT)scaled;
    }

    bool readDualShock3(ControllerState& state)
    {
        uint16_t cellButtons = 0;
        uint8_t stickX = 128;
        uint8_t stickY = 128;
        int psButton = 0;
        if (!cellPad_host_ds3_state(&cellButtons, &stickX, &stickY, &psButton))
        {
            return false;
        }
        static const struct
        {
            uint16_t cellBit;
            WORD xinputBit;
        } kButtonMap[] = {
            { 0x0001, XINPUT_GAMEPAD_BACK },          { 0x0002, XINPUT_GAMEPAD_LEFT_THUMB },
            { 0x0004, XINPUT_GAMEPAD_RIGHT_THUMB },   { 0x0008, XINPUT_GAMEPAD_START },
            { 0x0010, XINPUT_GAMEPAD_DPAD_UP },       { 0x0020, XINPUT_GAMEPAD_DPAD_RIGHT },
            { 0x0040, XINPUT_GAMEPAD_DPAD_DOWN },     { 0x0080, XINPUT_GAMEPAD_DPAD_LEFT },
            { 0x0400, XINPUT_GAMEPAD_LEFT_SHOULDER }, { 0x0800, XINPUT_GAMEPAD_RIGHT_SHOULDER },
            { 0x1000, XINPUT_GAMEPAD_Y },             { 0x2000, XINPUT_GAMEPAD_B },
            { 0x4000, XINPUT_GAMEPAD_A },             { 0x8000, XINPUT_GAMEPAD_X },
        };
        WORD buttons = 0;
        for (const auto& mapping : kButtonMap)
        {
            if (cellButtons & mapping.cellBit)
            {
                buttons |= mapping.xinputBit;
            }
        }
        state.connected = true;
        state.buttons = buttons;
        state.leftStickX = ds3AxisToXInput(stickX, false);
        state.leftStickY = ds3AxisToXInput(stickY, true);
        state.psButton = psButton != 0;
        return true;
    }

    ControllerState readController()
    {
        ControllerState state;
        if (readDualShock3(state))
        {
            return state;
        }
        for (DWORD port = 0; port < XUSER_MAX_COUNT; port++)
        {
            XINPUT_STATE raw;
            ZeroMemory(&raw, sizeof(raw));
            if (XInputGetState(port, &raw) == ERROR_SUCCESS)
            {
                state.connected = true;
                state.buttons = raw.Gamepad.wButtons;
                state.leftStickX = raw.Gamepad.sThumbLX;
                state.leftStickY = raw.Gamepad.sThumbLY;
                break;
            }
        }
        return state;
    }

    bool ourWindowHasFocus()
    {
        const HWND foreground = GetForegroundWindow();
        if (!foreground)
        {
            return false;
        }
        DWORD processId = 0;
        GetWindowThreadProcessId(foreground, &processId);
        return processId == GetCurrentProcessId();
    }

    bool keyDown(int virtualKey)
    {
        return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
    }

    bool initialize(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT format, uint32_t height)
    {
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heapDesc.NumDescriptors = 1;
        heapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (FAILED(device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&menu.srvHeap))))
        {
            std::fprintf(stderr, "[engine] menu: descriptor heap creation failed\n");
            return false;
        }

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad | ImGuiConfigFlags_NavEnableKeyboard;
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;

        ImGui::StyleColorsDark();
        ImGuiStyle& style = ImGui::GetStyle();
        style.WindowRounding = 6.0f;
        style.FrameRounding = 4.0f;
        style.WindowPadding = ImVec2(16.0f, 14.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        style.Colors[ImGuiCol_WindowBg].w = 0.92f;

        const float scale = height >= 720 ? (float)height / 720.0f : 1.0f;
        ImFontConfig fontConfig;
        fontConfig.SizePixels = 15.0f * scale;
        io.Fonts->AddFontDefault(&fontConfig);
        style.ScaleAllSizes(scale);

        ImGui_ImplDX12_InitInfo initInfo;
        initInfo.Device = device;
        initInfo.CommandQueue = queue;
        initInfo.NumFramesInFlight = kFramesInFlight;
        initInfo.RTVFormat = format;
        initInfo.DSVFormat = DXGI_FORMAT_UNKNOWN;
        initInfo.SrvDescriptorHeap = menu.srvHeap;
        initInfo.LegacySingleSrvCpuDescriptor = menu.srvHeap->GetCPUDescriptorHandleForHeapStart();
        initInfo.LegacySingleSrvGpuDescriptor = menu.srvHeap->GetGPUDescriptorHandleForHeapStart();
        if (!ImGui_ImplDX12_Init(&initInfo))
        {
            std::fprintf(stderr, "[engine] menu: ImGui DX12 backend init failed\n");
            return false;
        }
        QueryPerformanceFrequency(&menu.frequency);
        QueryPerformanceCounter(&menu.lastFrame);
        std::fprintf(stderr, "[engine] menu ready (L3+R3 or F1)\n");
        return true;
    }

    void feedGamepad(ImGuiIO& io, const ControllerState& controller)
    {
        const WORD buttons = controller.buttons;
        const SHORT stickThreshold = 16000;
        io.AddKeyEvent(ImGuiKey_GamepadDpadUp, (buttons & XINPUT_GAMEPAD_DPAD_UP) || controller.leftStickY > stickThreshold);
        io.AddKeyEvent(ImGuiKey_GamepadDpadDown, (buttons & XINPUT_GAMEPAD_DPAD_DOWN) || controller.leftStickY < -stickThreshold);
        io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, (buttons & XINPUT_GAMEPAD_DPAD_LEFT) || controller.leftStickX < -stickThreshold);
        io.AddKeyEvent(ImGuiKey_GamepadDpadRight, (buttons & XINPUT_GAMEPAD_DPAD_RIGHT) || controller.leftStickX > stickThreshold);
        io.AddKeyEvent(ImGuiKey_GamepadFaceDown, (buttons & XINPUT_GAMEPAD_A) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceRight, (buttons & XINPUT_GAMEPAD_B) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceLeft, (buttons & XINPUT_GAMEPAD_X) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadFaceUp, (buttons & XINPUT_GAMEPAD_Y) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadL1, (buttons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0);
        io.AddKeyEvent(ImGuiKey_GamepadR1, (buttons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0);
    }

    void feedKeyboard(ImGuiIO& io, bool focused)
    {
        io.AddKeyEvent(ImGuiKey_UpArrow, focused && keyDown(VK_UP));
        io.AddKeyEvent(ImGuiKey_DownArrow, focused && keyDown(VK_DOWN));
        io.AddKeyEvent(ImGuiKey_LeftArrow, focused && keyDown(VK_LEFT));
        io.AddKeyEvent(ImGuiKey_RightArrow, focused && keyDown(VK_RIGHT));
        io.AddKeyEvent(ImGuiKey_Enter, focused && keyDown(VK_RETURN));
        io.AddKeyEvent(ImGuiKey_Space, focused && keyDown(VK_SPACE));
    }

    void updatePerformance(double frameSeconds)
    {
        menu.windowSeconds += frameSeconds;
        menu.windowFrames++;
        if (menu.windowSeconds < 0.5)
        {
            return;
        }
        const double frames = (double)menu.windowFrames;
        const uint64_t sleepNs = g_gcm_perf_main_usleep_ns;
        const uint64_t idleNs = g_gcm_perf_present_idle_ns;
        const uint64_t gpuNs = g_gcm_perf_gpu_wait_ns;
        menu.shownFps = frames / menu.windowSeconds;
        menu.shownFrameMs = menu.windowSeconds * 1000.0 / frames;
        menu.shownMainSleepMs = (double)(sleepNs - menu.lastSleepNs) / 1e6 / frames;
        menu.shownRenderIdleMs = (double)(idleNs - menu.lastIdleNs) / 1e6 / frames;
        menu.shownGpuWaitMs = (double)(gpuNs - menu.lastGpuNs) / 1e6 / frames;
        /* The game held back by the renderer: in cellGcmSetWaitFlip, or
         * stopped short of commands the renderer has not read yet. */
        const uint64_t waitFlipNs = g_gcm_perf_waitflip_ns;
        const uint64_t ringWaitNs = g_gcm_perf_ring_wait_ns;
        const uint64_t drainNs = g_gcm_perf_drain_ns;
        const uint64_t presentNs = g_gcm_perf_present_ns;
        menu.shownRendererWaitMs = (double)((waitFlipNs - menu.lastWaitFlipNs) + (ringWaitNs - menu.lastRingWaitNs)) / 1e6 / frames;
        menu.shownDrainMs = (double)(drainNs - menu.lastDrainNs) / 1e6 / frames;
        menu.shownPresentMs = (double)(presentNs - menu.lastPresentNs) / 1e6 / frames;
        menu.lastSleepNs = sleepNs;
        menu.lastIdleNs = idleNs;
        menu.lastGpuNs = gpuNs;
        menu.lastWaitFlipNs = waitFlipNs;
        menu.lastRingWaitNs = ringWaitNs;
        menu.lastDrainNs = drainNs;
        menu.lastPresentNs = presentNs;
        menu.windowSeconds = 0.0;
        menu.windowFrames = 0;
    }

    void drawPerformanceOverlay()
    {
        ImGui::SetNextWindowPos(ImVec2(10.0f, 10.0f), ImGuiCond_Always);
        ImGui::SetNextWindowBgAlpha(0.55f);
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                                       ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs;
        if (ImGui::Begin("##performance", nullptr, flags))
        {
            ImGui::Text("%.1f fps  %.1f ms", menu.shownFps, menu.shownFrameMs);
            /* Game work = frame time minus the limiter's sleep and the time
             * the game spent held back by the renderer; when the renderer is
             * the bottleneck the game waits, and that wait is not game work. */
            const double gameWork = menu.shownFrameMs - menu.shownMainSleepMs - menu.shownRendererWaitMs;
            ImGui::Text("game %.1f ms  limiter sleep %.1f ms", gameWork > 0.0 ? gameWork : 0.0, menu.shownMainSleepMs);
            ImGui::Text("game waiting for renderer %.1f ms", menu.shownRendererWaitMs);
            const double renderBusy = menu.shownFrameMs - menu.shownRenderIdleMs;
            ImGui::Text("render %.1f ms (commands %.1f, draw + present %.1f)  gpu wait %.1f ms",
                        renderBusy > 0.0 ? renderBusy : 0.0, menu.shownDrainMs, menu.shownPresentMs, menu.shownGpuWaitMs);
            uint32_t shadersPending = 0;
            uint32_t shadersBuilt = 0;
            uint32_t skippedDraws = 0;
            rsx_live_draw_get_shader_stats(&shadersPending, &shadersBuilt, &skippedDraws);
            if (shadersPending > 0)
            {
                ImGui::Text("building %u new shader%s", shadersPending, shadersPending == 1 ? "" : "s");
            }
        }
        ImGui::End();
    }

    bool radioCap(const char* label, FrameRateCap value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(label, settings.frameRateCap == value))
        {
            settings.frameRateCap = value;
            return true;
        }
        return false;
    }

    bool radioScale(const char* label, int value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(label, settings.renderScale == value))
        {
            settings.renderScale = value;
            return true;
        }
        return false;
    }

    bool radioAntiAliasing(const char* label, AntiAliasing value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(label, settings.antiAliasing == value))
        {
            settings.antiAliasing = value;
            return true;
        }
        return false;
    }

    bool radioAspect(const char* label, AspectRatio value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(label, settings.aspectRatio == value))
        {
            settings.aspectRatio = value;
            return true;
        }
        return false;
    }

    bool radioMotion(const char* label, MotionControls value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(label, settings.motionControls == value))
        {
            settings.motionControls = value;
            engineSettingsApplyInput();
            return true;
        }
        return false;
    }

    bool radioAnisotropy(const char* label, int value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(label, settings.anisotropy == value))
        {
            settings.anisotropy = value;
            return true;
        }
        return false;
    }

    bool radioSharpening(const char* label, int value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(label, settings.sharpening == value))
        {
            settings.sharpening = value;
            return true;
        }
        return false;
    }

    /* Render resolution, anti-aliasing and shader building. The resolution
     * is fixed when the renderer starts, so a new one shows on the next
     * launch; the others change at once. */
    bool drawGraphics()
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;
        ImGui::SeparatorText("Graphics");
        changed |= ImGui::Checkbox("Fullscreen (Alt+Enter)", &settings.fullscreen);
        ImGui::TextUnformatted("Render resolution");
        changed |= radioScale("1x  1280x720 (the PS3's)", 1);
        ImGui::SameLine();
        changed |= radioScale("2x  2560x1440", 2);
        ImGui::SameLine();
        changed |= radioScale("3x  3840x2160", 3);
        const int activeScale = (int)rsx_live_draw_get_render_scale();
        if (activeScale != settings.renderScale)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                               "Now %dx; %dx applies when you next start the game.", activeScale,
                               settings.renderScale);
        }
        else
        {
            ImGui::TextDisabled("Above 1x on a smaller window the image is also supersampled (smoother edges).");
        }
        ImGui::TextUnformatted("Aspect ratio (field of view)");
        changed |= radioAspect("16:9 (original)", AspectRatio::Original);
        ImGui::SameLine();
        changed |= radioAspect("Match display", AspectRatio::MatchDisplay);
        ImGui::SameLine();
        changed |= radioAspect("21:9", AspectRatio::Ultrawide);
        ImGui::SameLine();
        changed |= radioAspect("32:9", AspectRatio::SuperUltrawide);
        {
            const float activeAspect = rsx_live_draw_get_aspect();
            const float wantedAspect = engineSettingsAspectValue(settings.aspectRatio);
            if (std::fabs(activeAspect - wantedAspect) > 0.01f)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Now %.2f:1; %.2f:1 applies when you next start the game.", activeAspect, wantedAspect);
            }
            else if (activeAspect > 16.0f / 9.0f + 0.01f)
            {
                ImGui::TextDisabled("Wider than 16:9: menus and the HUD are stretched to fit.");
            }
        }
        ImGui::TextUnformatted("Anti-aliasing");
        changed |= radioAntiAliasing("Off", AntiAliasing::Off);
        ImGui::SameLine();
        changed |= radioAntiAliasing("FXAA", AntiAliasing::Fxaa);
        ImGui::TextUnformatted("Anisotropic filtering (textures at an angle)");
        changed |= radioAnisotropy("Off##aniso", 1);
        static const int kAnisotropyLevels[4] = {2, 4, 8, 16};
        static const char* const kAnisotropyLabels[4] = {"2x", "4x", "8x", "16x"};
        for (int levelIndex = 0; levelIndex < 4; levelIndex++)
        {
            ImGui::SameLine();
            changed |= radioAnisotropy(kAnisotropyLabels[levelIndex], kAnisotropyLevels[levelIndex]);
        }
        ImGui::TextUnformatted("Sharpening");
        changed |= radioSharpening("Off##sharpen", 0);
        ImGui::SameLine();
        changed |= radioSharpening("Low", 30);
        ImGui::SameLine();
        changed |= radioSharpening("Medium", 60);
        ImGui::SameLine();
        changed |= radioSharpening("High", 100);
        changed |= ImGui::Checkbox("Build new shaders in the background (no hitch when an effect first appears)",
                                   &settings.backgroundShaders);
        if (changed)
        {
            engineSettingsApplyGraphics();
        }
        return changed;
    }

    void refreshSaveSnapshots()
    {
        menu.snapshots = saveSnapshotList();
        menu.gameSaveCount = saveDataCount();
    }

    /* Snapshots of the game's own save data: take one at a save point, come
     * back to it later through the game's Load menu. Restoring asks for a
     * second press, and keeps the saves it replaces as a new snapshot. */
    void drawSaveSnapshots()
    {
        ImGui::SeparatorText("Save data snapshots");
        ImGui::TextDisabled("Copies of the game's own saves (a full save state is not possible in this port).");
        ImGui::Text("The game has %d save%s.", menu.gameSaveCount, menu.gameSaveCount == 1 ? "" : "s");
        if (ImGui::Button("Snapshot the game's saves now"))
        {
            saveSnapshotTake(menu.saveMessage);
            menu.pendingRestore.clear();
            refreshSaveSnapshots();
        }
        const size_t shown = menu.snapshots.size() < 8 ? menu.snapshots.size() : 8;
        for (size_t index = 0; index < shown; index++)
        {
            const SaveSnapshot& snapshot = menu.snapshots[index];
            ImGui::PushID((int)index);
            const bool confirming = menu.pendingRestore == snapshot.name;
            if (ImGui::Button(confirming ? "Confirm restore" : "Restore"))
            {
                if (confirming)
                {
                    saveSnapshotRestore(snapshot.name, menu.saveMessage);
                    menu.pendingRestore.clear();
                    refreshSaveSnapshots();
                    ImGui::PopID();
                    break;
                }
                menu.pendingRestore = snapshot.name;
                menu.saveMessage = "Press Confirm restore to replace the game's saves with " + snapshot.name + ".";
            }
            ImGui::SameLine();
            ImGui::Text("%s  (%d save%s)", snapshot.name.c_str(), snapshot.saveCount, snapshot.saveCount == 1 ? "" : "s");
            ImGui::PopID();
        }
        if (menu.snapshots.size() > shown)
        {
            ImGui::TextDisabled("...and %d older in save_snapshots", (int)(menu.snapshots.size() - shown));
        }
        if (!menu.saveMessage.empty())
        {
            ImGui::TextWrapped("%s", menu.saveMessage.c_str());
        }
    }

    void drawMenu(float displayWidth, float displayHeight)
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;

        ImGui::SetNextWindowPos(ImVec2(displayWidth * 0.5f, displayHeight * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        /* Taller than the screen scrolls instead of running off it. */
        ImGui::SetNextWindowSizeConstraints(ImVec2(0.0f, 0.0f),
                                            ImVec2(displayWidth * 0.95f, displayHeight * 0.92f));
        if (menu.focusRequested)
        {
            ImGui::SetNextWindowFocus();
            menu.focusRequested = false;
        }
        const ImGuiWindowFlags flags = ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoCollapse |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("Folklore - engine menu", nullptr, flags))
        {
            ImGui::SeparatorText("Frame rate");
            changed |= radioCap("30 fps (original)", FrameRateCap::Original);
            changed |= radioCap("60 fps (experimental)", FrameRateCap::Sixty);
            changed |= radioCap("Unlimited (experimental, paced by the 60 Hz flip)", FrameRateCap::Unlimited);
            ImGui::TextDisabled("Above 30 fps the game logic may run differently (timing).");

            ImGui::SeparatorText("Controls");
            ImGui::TextUnformatted("Motion controls (capturing Folks)");
            changed |= radioMotion("Automatic", MotionControls::Automatic);
            ImGui::SameLine();
            changed |= radioMotion("SIXAXIS (original)", MotionControls::Sixaxis);
            ImGui::SameLine();
            changed |= radioMotion("Auto-capture", MotionControls::AutoCapture);
            {
                uint16_t unusedButtons = 0;
                const bool haveDualShock3 = cellPad_host_ds3_state(&unusedButtons, nullptr, nullptr, nullptr) != 0;
                if (haveDualShock3 && cellPad_motion_live())
                {
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "DualShock 3 connected: motion controls active.");
                }
                else if (haveDualShock3)
                {
                    ImGui::TextDisabled("DualShock 3 connected; captures complete by themselves.");
                }
                else if (settings.motionControls == MotionControls::Sixaxis)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                       "No DualShock 3 found (DsHidMini in SXS mode) -- captures need one.");
                }
                else
                {
                    ImGui::TextDisabled("No DualShock 3 found (DsHidMini in SXS mode); captures complete by themselves.");
                }
            }

            ImGui::SeparatorText("Physics");
            changed |= ImGui::Checkbox("Step physics by the frame time (fixes cloth and hair above 30 fps)",
                                       &settings.physicsFrameStep);

            ImGui::SeparatorText("Pacing");
            changed |= ImGui::Checkbox("Precise frame limiter (wake every 1 ms instead of 10 ms)", &settings.fineLimiterSleep);

            changed |= drawGraphics();
            changed |= drawGameFiles();

            ImGui::SeparatorText("Display");
            changed |= ImGui::Checkbox("Performance overlay", &settings.showPerformanceOverlay);

            ImGui::SeparatorText("Now");
            ImGui::Text("%.1f fps, %.1f ms per frame", menu.shownFps, menu.shownFrameMs);

            drawSaveSnapshots();

            ImGui::Spacing();
            if (ImGui::Button("Close"))
            {
                menu.menuOpen = false;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("L3+R3 / F1 toggles, Circle / Esc closes. Saved to %s", engineSettingsPath());
        }
        ImGui::End();

        if (changed)
        {
            engineSettingsSave();
        }
    }

    void overlayHook(void* device, void* queue, void* list, unsigned long long rtv,
                     unsigned format, uint32_t width, uint32_t height)
    {
        (void)rtv;
        if (menu.failed)
        {
            return;
        }

        /* Toggle input: L3 + R3 pressed together, F1, or Escape/Circle to
         * close. Edge-triggered so holding the chord does not flicker. */
        const ControllerState controller = readController();
        const bool focused = ourWindowHasFocus();
        const WORD chord = XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB;
        const bool chordNow = (controller.buttons & chord) == chord;
        const bool chordBefore = (menu.previousButtons & chord) == chord;
        const bool toggleKey = focused && keyDown(VK_F1);
        const bool escapeKey = focused && keyDown(VK_ESCAPE);
        const WORD pressed = (WORD)(controller.buttons & ~menu.previousButtons);
        bool toggle = (chordNow && !chordBefore) || (toggleKey && !menu.previousToggleKey);
        if (menu.menuOpen && !toggle)
        {
            if ((escapeKey && !menu.previousEscape) || (pressed & (XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_START)))
            {
                toggle = true;
            }
        }
        /* Render capture without leaving the game: F2, or the PS button on a
         * DualShock 3, records the next four seconds of draws the way
         * capture.bat does (run.ps1 arms it). */
        const bool captureKey = (focused && keyDown(VK_F2)) || controller.psButton;
        if (captureKey && !menu.previousCaptureKey && !menu.captureActive)
        {
            const char* gatePath = std::getenv("RSX_DRAW_CSV_GATE");
            if (gatePath && *gatePath)
            {
                FILE* gateFile = std::fopen(gatePath, "w");
                if (gateFile)
                {
                    std::fclose(gateFile);
                    menu.captureActive = true;
                    QueryPerformanceCounter(&menu.captureStart);
                    std::fprintf(stderr, "[engine] render capture started (4 s)\n");
                }
            }
            else
            {
                std::fprintf(stderr, "[engine] render capture is not armed (start the game with run.ps1)\n");
            }
        }
        if (menu.captureActive && menu.frequency.QuadPart)
        {
            LARGE_INTEGER captureNow;
            QueryPerformanceCounter(&captureNow);
            const double captureSeconds =
                (double)(captureNow.QuadPart - menu.captureStart.QuadPart) / (double)menu.frequency.QuadPart;
            if (captureSeconds >= 4.0)
            {
                const char* gatePath = std::getenv("RSX_DRAW_CSV_GATE");
                if (gatePath && *gatePath)
                {
                    DeleteFileA(gatePath);
                }
                menu.captureActive = false;
                std::fprintf(stderr, "[engine] render capture finished\n");
            }
        }
        menu.previousCaptureKey = captureKey;
        menu.previousButtons = controller.buttons;
        menu.previousToggleKey = toggleKey;
        menu.previousEscape = escapeKey;
        if (toggle)
        {
            menu.menuOpen = !menu.menuOpen;
            menu.focusRequested = menu.menuOpen;
            if (menu.menuOpen)
            {
                refreshSaveSnapshots();
                menu.saveMessage.clear();
                menu.pendingRestore.clear();
            }
        }
        g_cellpad_block_guest_input = menu.menuOpen ? 1 : 0;

        /* Alt+Enter toggles fullscreen in the window itself; keep the
         * setting (and the ini) in step so the next launch starts the same. */
        {
            EngineSettings& settings = engineSettings();
            const bool fullscreenNow = rsx_null_backend_is_fullscreen() != 0;
            if (fullscreenNow != settings.fullscreen)
            {
                settings.fullscreen = fullscreenNow;
                engineSettingsSave();
            }
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        double frameSeconds = 0.0;
        if (menu.frequency.QuadPart)
        {
            frameSeconds = (double)(now.QuadPart - menu.lastFrame.QuadPart) / (double)menu.frequency.QuadPart;
        }
        menu.lastFrame = now;
        updatePerformance(frameSeconds);

        const bool showOverlay = engineSettings().showPerformanceOverlay;
        if (!menu.menuOpen && !showOverlay)
        {
            return;     /* nothing to draw: no ImGui work at all */
        }
        if (!menu.initialized)
        {
            menu.initialized = true;
            if (!initialize((ID3D12Device*)device, (ID3D12CommandQueue*)queue, (DXGI_FORMAT)format, height))
            {
                menu.failed = true;
                menu.menuOpen = false;
                g_cellpad_block_guest_input = 0;
                return;
            }
            QueryPerformanceCounter(&menu.lastFrame);
        }

        ImGuiIO& io = ImGui::GetIO();
        io.DisplaySize = ImVec2((float)width, (float)height);
        io.DeltaTime = frameSeconds > 0.0 && frameSeconds < 1.0 ? (float)frameSeconds : 1.0f / 60.0f;
        if (menu.menuOpen)
        {
            feedGamepad(io, controller);
            feedKeyboard(io, focused);
        }
        else
        {
            feedGamepad(io, ControllerState());
            feedKeyboard(io, false);
        }

        ImGui_ImplDX12_NewFrame();
        ImGui::NewFrame();
        if (showOverlay)
        {
            drawPerformanceOverlay();
        }
        if (menu.menuOpen)
        {
            drawMenu((float)width, (float)height);
        }
        ImGui::Render();

        ID3D12GraphicsCommandList* commandList = (ID3D12GraphicsCommandList*)list;
        commandList->SetDescriptorHeaps(1, &menu.srvHeap);
        ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), commandList);
    }

    struct Registrar
    {
        Registrar()
        {
            rsx_live_draw_set_overlay(&overlayHook);
        }
    };
    Registrar registrar;
}

#endif /* _WIN32 */
