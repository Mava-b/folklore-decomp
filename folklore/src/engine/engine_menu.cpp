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
#include "engine_dlc.h"
#include "engine_region.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <xinput.h>
#include <d3d12.h>
#include <dxgi1_4.h>

#include <algorithm>
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
    /* A controller read over HID (cellPad.c): a DualShock 3 or 4, DualSense
     * or Switch Pro. Buttons in CELL_PAD packing. */
    int cellPad_host_ds3_state(uint16_t* buttons, uint8_t* stickLeftX, uint8_t* stickLeftY, int* psButton);
    const char* cellPad_host_pad_name(void);
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
        /* The depth of field the game's shaders were built with this run. */
        EngineSettings::DepthOfField startDepthOfField = EngineSettings::DepthOfField::Smooth;
        EngineSettings::Bloom startBloom = EngineSettings::Bloom::Smooth;
        EngineSettings::AmbientOcclusion startAmbientOcclusion = EngineSettings::AmbientOcclusion::Subtle;
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
        bool forUpdate = false;     /* the 1.10 update folder, not the disc dump */
        std::string folder;
        std::string problem;
    };

    FolderPickState folderPick;

    void startGameFolderPick(const std::string& startFolder, bool forUpdate = false)
    {
        {
            std::lock_guard<std::mutex> guard(folderPick.lock);
            if (folderPick.running)
            {
                return;
            }
            folderPick.running = true;
            folderPick.done = false;
            folderPick.forUpdate = forUpdate;
        }
        /* A borderless fullscreen window would cover the dialog: step out of
         * fullscreen while it is open. */
        const bool wasFullscreen = rsx_null_backend_is_fullscreen() != 0;
        std::thread([startFolder, wasFullscreen, forUpdate]() {
            if (wasFullscreen)
            {
                rsx_null_backend_set_fullscreen(0);
            }
            const std::string picked = enginePickGameFolder(startFolder, forUpdate);
            std::string problem;
            if (!picked.empty())
            {
                if (forUpdate)
                {
                    engineCheckUpdateFolder(picked, &problem);
                }
                else
                {
                    engineCheckGameFolder(picked, &problem);
                }
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

    /* Downloadable content (engine_dlc.cpp): the user picks RPCS3's folder
     * (or a PS3 copy of the game's HDD folder); the packs it holds are
     * decrypted with the user's own licences and installed into hdd0 on a
     * thread of their own, with progress shown here. */
    struct DlcImportState
    {
        std::mutex lock;
        bool running = false;
        int filesDone = 0;
        int filesTotal = 0;
        std::string currentFile;
        std::string report;
        std::vector<int> installed;
        std::vector<int> pendingRemovals;
        double installedCheckedAt = -100.0;
        /* Removal: the pack whose Remove was pressed once (0: Remove all),
         * waiting for the confirming press; -1 none. */
        int confirmingRemoval = -1;
        std::string removalMessage;
        bool removalReportTaken = false;
    };

    DlcImportState dlcImport;

    void startDlcImport(const std::string& startFolder)
    {
        {
            std::lock_guard<std::mutex> guard(dlcImport.lock);
            if (dlcImport.running)
            {
                return;
            }
            dlcImport.running = true;
            dlcImport.filesDone = 0;
            dlcImport.filesTotal = 0;
            dlcImport.currentFile = "Choosing a folder...";
            dlcImport.report.clear();
        }
        const bool wasFullscreen = rsx_null_backend_is_fullscreen() != 0;
        std::thread([startFolder, wasFullscreen]() {
            if (wasFullscreen)
            {
                rsx_null_backend_set_fullscreen(0);
            }
            const std::string picked = enginePickFolder(
                startFolder, "Folklore: choose RPCS3's folder (the one with dev_hdd0 in it) to add the DLC from");
            if (wasFullscreen)
            {
                rsx_null_backend_set_fullscreen(1);
            }
            std::string report;
            if (!picked.empty())
            {
                DlcSource source;
                std::string problem;
                if (!engineDlcScan(picked, &source, &problem))
                {
                    report = problem;
                }
                else
                {
                    report = engineDlcInstall(source, [](int done, int total, const std::string& file) {
                        std::lock_guard<std::mutex> guard(dlcImport.lock);
                        dlcImport.filesDone = done;
                        dlcImport.filesTotal = total;
                        dlcImport.currentFile = file;
                    });
                }
            }
            std::lock_guard<std::mutex> guard(dlcImport.lock);
            dlcImport.report = report;
            dlcImport.running = false;
            dlcImport.installedCheckedAt = -100.0;     /* look again */
        }).detach();
    }

    /* The folder the picker opens in: RPCS3's dev_hdd0\game, when the 1.10
     * update folder is known (it is in RPCS3), else the game folder. */
    std::string dlcStartFolder()
    {
        const EngineSettings& settings = engineSettings();
        if (!settings.updateDir.empty())
        {
            const size_t separator = settings.updateDir.find_last_of("\\/");
            if (separator != std::string::npos)
            {
                return settings.updateDir.substr(0, separator);
            }
        }
        return settings.gameDir;
    }

    /* Ask for packs to be removed at the next start, after copying the
     * saves to a snapshot named for it (a save that uses a removed pack's
     * Folks or items may not load without it). */
    void requestDlcRemoval(const std::vector<int>& packs, const std::string& what)
    {
        std::string snapshotMessage;
        std::string snapshotName;
        const bool snapshotTaken = saveSnapshotTakeLabelled("before removing DLC", snapshotMessage, snapshotName);
        engineDlcScheduleRemoval(packs);
        std::string message = what + " will be removed when you next start the game.";
        if (snapshotTaken)
        {
            message += " Your saves were copied first, to the snapshot " + snapshotName +
                       " (Save data snapshots, below): if a save that uses this DLC's Folks or items will not load "
                       "without it, restore that snapshot or add the DLC again.";
        }
        else
        {
            message += " (No saves to copy yet.)";
        }
        std::lock_guard<std::mutex> guard(dlcImport.lock);
        dlcImport.removalMessage = message;
        dlcImport.confirmingRemoval = -1;
        dlcImport.installedCheckedAt = -100.0;
    }

    void drawDownloadableContent()
    {
        ImGui::SeparatorText("Downloadable content");
        bool running = false;
        int filesDone = 0;
        int filesTotal = 0;
        std::string currentFile;
        std::string report;
        std::vector<int> installed;
        std::vector<int> pendingRemovals;
        int confirmingRemoval = -1;
        std::string removalMessage;
        {
            std::lock_guard<std::mutex> guard(dlcImport.lock);
            if (!dlcImport.running && ImGui::GetTime() - dlcImport.installedCheckedAt > 2.0)
            {
                dlcImport.installed = engineDlcInstalledPacks();
                dlcImport.pendingRemovals = engineDlcPendingRemovals();
                dlcImport.installedCheckedAt = ImGui::GetTime();
            }
            if (!dlcImport.removalReportTaken)
            {
                /* What the last start removed, shown once. */
                const std::string removed = engineDlcTakeRemovalReport();
                if (!removed.empty())
                {
                    dlcImport.removalMessage = "Removed at this start:\n" + removed;
                }
                dlcImport.removalReportTaken = true;
            }
            running = dlcImport.running;
            filesDone = dlcImport.filesDone;
            filesTotal = dlcImport.filesTotal;
            currentFile = dlcImport.currentFile;
            report = dlcImport.report;
            installed = dlcImport.installed;
            pendingRemovals = dlcImport.pendingRemovals;
            confirmingRemoval = dlcImport.confirmingRemoval;
            removalMessage = dlcImport.removalMessage;
        }

        int pressedRemove = -2;     /* -2 nothing; 0 Remove all; else a pack */
        int pressedKeep = -1;
        if (installed.empty())
        {
            ImGui::TextDisabled("No packs installed.");
        }
        else
        {
            ImGui::BeginDisabled(running);
            for (const int pack : installed)
            {
                ImGui::PushID(pack);
                const bool pending =
                    std::find(pendingRemovals.begin(), pendingRemovals.end(), pack) != pendingRemovals.end();
                if (pending)
                {
                    if (ImGui::SmallButton("Keep"))
                    {
                        pressedKeep = pack;
                    }
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "%s  (removed at the next start)",
                                       engineDlcPackNameByNumber(pack).c_str());
                }
                else
                {
                    if (ImGui::SmallButton(confirmingRemoval == pack ? "Confirm remove" : "Remove"))
                    {
                        pressedRemove = pack;
                    }
                    ImGui::SameLine();
                    ImGui::Text("%s", engineDlcPackNameByNumber(pack).c_str());
                }
                ImGui::PopID();
            }
            if (installed.size() > 1 && pendingRemovals.size() < installed.size())
            {
                if (ImGui::Button(confirmingRemoval == 0 ? "Confirm: remove all DLC" : "Remove all DLC"))
                {
                    pressedRemove = 0;
                }
            }
            ImGui::EndDisabled();
            if (confirmingRemoval >= 0)
            {
                ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                   "Press again to confirm. Saves that use this DLC's Folks or items may not load "
                                   "without it; your saves are copied to a snapshot first.");
            }
        }
        ImGui::TextDisabled("Your own add-on packs: install each pack's .pkg and its .rap licence in RPCS3 (File >");
        ImGui::TextDisabled("Install Packages/Raps/Edats), then add them here from RPCS3's folder. The other");
        ImGui::TextDisabled("release's packs work too (North American packs in the European build, and back).");
        bool startImport = false;
        ImGui::BeginDisabled(running);
        if (ImGui::Button(running ? "Adding DLC..." : "Add DLC from RPCS3..."))
        {
            startImport = true;
        }
        ImGui::EndDisabled();
        if (running)
        {
            const float fraction = filesTotal > 0 ? (float)filesDone / (float)filesTotal : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%d / %d files", filesDone, filesTotal);
            ImGui::ProgressBar(fraction, ImVec2(420.0f, 0.0f), filesTotal > 0 ? overlay : "");
            ImGui::TextDisabled("%s", currentFile.c_str());
        }
        else if (!report.empty())
        {
            ImGui::TextWrapped("%s", report.c_str());
        }
        if (!removalMessage.empty())
        {
            ImGui::TextWrapped("%s", removalMessage.c_str());
        }

        if (pressedKeep > 0)
        {
            engineDlcCancelRemoval(pressedKeep);
            std::lock_guard<std::mutex> guard(dlcImport.lock);
            dlcImport.removalMessage = engineDlcPackNameByNumber(pressedKeep) + " will be kept.";
            dlcImport.installedCheckedAt = -100.0;
        }
        if (pressedRemove != -2)
        {
            if (pressedRemove != confirmingRemoval)
            {
                std::lock_guard<std::mutex> guard(dlcImport.lock);
                dlcImport.confirmingRemoval = pressedRemove;     /* first press: ask again */
            }
            else if (pressedRemove == 0)
            {
                requestDlcRemoval(installed, "All DLC");
            }
            else
            {
                requestDlcRemoval({pressedRemove}, engineDlcPackNameByNumber(pressedRemove));
            }
        }
        if (startImport)
        {
            startDlcImport(dlcStartFolder());
        }
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
                    if (folderPick.forUpdate)
                    {
                        settings.updateDir = folderPick.folder;
                    }
                    else
                    {
                        settings.gameDir = folderPick.folder;
                    }
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
        if (FOLKLORE_USES_UPDATE)
        {
            ImGui::TextWrapped("1.10 update folder: %s",
                               settings.updateDir.empty() ? "(passed in by run.ps1)" : settings.updateDir.c_str());
        }
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
        if (FOLKLORE_USES_UPDATE)
        {
            ImGui::SameLine();
            if (ImGui::Button(pickRunning ? "Choosing a folder...##update" : "Change update folder..."))
            {
                startGameFolderPick(settings.updateDir, true);
            }
        }
        ImGui::EndDisabled();
        if (!menu.gameFolderMessage.empty())
        {
            ImGui::TextWrapped("%s", menu.gameFolderMessage.c_str());
        }
        return changed;
    }

    /* A controller read over HID (a DualShock 3 in DsHidMini's SXS mode, a
     * DualShock 4, DualSense or Switch Pro) is not an XInput pad; give the
     * menu its buttons in XInput terms. */
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

    bool readHidController(ControllerState& state)
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
        if (readHidController(state))
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

    bool radioLanguage(GameLanguage value)
    {
        EngineSettings& settings = engineSettings();
        if (ImGui::RadioButton(engineSettingsLanguageName(value), settings.language == value))
        {
            settings.language = value;
            return true;
        }
        return false;
    }

    /* The European release's five languages. The game reads the language
     * once, as it starts, so a change shows on the next launch. */
    bool drawLanguage()
    {
        if (!engineSettingsLanguageSelectable())
        {
            return false;
        }
        EngineSettings& settings = engineSettings();
        bool changed = false;
        ImGui::SeparatorText("Language");
        static const GameLanguage kChoices[5] = {GameLanguage::English, GameLanguage::French, GameLanguage::Spanish,
                                                 GameLanguage::German, GameLanguage::Italian};
        for (int choiceIndex = 0; choiceIndex < 5; choiceIndex++)
        {
            if (choiceIndex > 0)
            {
                ImGui::SameLine();
            }
            changed |= radioLanguage(kChoices[choiceIndex]);
        }
        const GameLanguage activeLanguage = engineSettingsActiveLanguage();
        if (activeLanguage != settings.language)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "Now %s; %s applies when you next start the game.",
                               engineSettingsLanguageName(activeLanguage),
                               engineSettingsLanguageName(settings.language));
        }
        else
        {
            ImGui::TextDisabled("Menus, text, subtitles and the cutscenes' sound (the disc has each per language).");
        }
        return changed;
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
        }
        ImGui::TextUnformatted("Menus and HUD (when wider than 16:9)");
        {
            int layoutIndex = (int)settings.hudLayout;
            bool layoutChanged = ImGui::RadioButton("Map and gauges at the edges##hud", &layoutIndex, 2);
            ImGui::SameLine();
            layoutChanged |= ImGui::RadioButton("Centred##hud", &layoutIndex, 1);
            ImGui::SameLine();
            layoutChanged |= ImGui::RadioButton("Stretched to fit##hud", &layoutIndex, 0);
            if (layoutChanged)
            {
                settings.hudLayout = (EngineSettings::HudLayout)layoutIndex;
                changed = true;
            }
            if (settings.hudLayout == EngineSettings::HudLayout::Edges)
            {
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::SliderInt("How far out##hudedge", &settings.hudEdge, 0, 100, "%d%%"))
                {
                    changed = true;
                }
            }
        }
        ImGui::TextUnformatted("Anti-aliasing");
        changed |= radioAntiAliasing("Off", AntiAliasing::Off);
        ImGui::SameLine();
        changed |= radioAntiAliasing("FXAA", AntiAliasing::Fxaa);
        ImGui::SameLine();
        changed |= radioAntiAliasing("SMAA (sharper)", AntiAliasing::Smaa);
        ImGui::TextUnformatted("Anisotropic filtering (textures at an angle)");
        changed |= radioAnisotropy("Off##aniso", 1);
        static const int kAnisotropyLevels[4] = {2, 4, 8, 16};
        static const char* const kAnisotropyLabels[4] = {"2x", "4x", "8x", "16x"};
        for (int levelIndex = 0; levelIndex < 4; levelIndex++)
        {
            ImGui::SameLine();
            changed |= radioAnisotropy(kAnisotropyLabels[levelIndex], kAnisotropyLevels[levelIndex]);
        }
        ImGui::TextUnformatted("Texture detail (sharper surfaces in the distance)");
        {
            int detailIndex = (int)settings.textureDetail;
            bool detailChanged = ImGui::RadioButton("Original (the PS3's)##detail", &detailIndex, 0);
            ImGui::SameLine();
            detailChanged |= ImGui::RadioButton("Sharper##detail", &detailIndex, 1);
            ImGui::SameLine();
            detailChanged |= ImGui::RadioButton("Sharpest##detail", &detailIndex, 2);
            if (detailChanged)
            {
                settings.textureDetail = (EngineSettings::TextureDetail)detailIndex;
                changed = true;
            }
        }
        ImGui::TextUnformatted("Sharpening");
        changed |= radioSharpening("Off##sharpen", 0);
        ImGui::SameLine();
        changed |= radioSharpening("Low", 30);
        ImGui::SameLine();
        changed |= radioSharpening("Medium", 60);
        ImGui::SameLine();
        changed |= radioSharpening("High", 100);
        ImGui::TextUnformatted("Depth of field");
        {
            int depthOfFieldIndex = (int)settings.depthOfField;
            bool depthOfFieldChanged = ImGui::RadioButton("Original (the PS3's)##dof", &depthOfFieldIndex, 0);
            ImGui::SameLine();
            depthOfFieldChanged |= ImGui::RadioButton("Smooth##dof", &depthOfFieldIndex, 1);
            if (depthOfFieldChanged)
            {
                settings.depthOfField = (EngineSettings::DepthOfField)depthOfFieldIndex;
                changed = true;
            }
            if (settings.depthOfField != menu.startDepthOfField)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(applies when you next start the game)");
            }
        }
        ImGui::TextUnformatted("Bloom");
        {
            int bloomIndex = (int)settings.bloom;
            bool bloomChanged = ImGui::RadioButton("Original (the PS3's)##bloom", &bloomIndex, 0);
            ImGui::SameLine();
            bloomChanged |= ImGui::RadioButton("Smooth (no flicker)##bloom", &bloomIndex, 1);
            if (bloomChanged)
            {
                settings.bloom = (EngineSettings::Bloom)bloomIndex;
                changed = true;
            }
            if (settings.bloom != menu.startBloom)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(applies when you next start the game)");
            }
        }
        ImGui::TextUnformatted("Shadows (characters' shadow edges)");
        {
            int shadowIndex = (int)settings.shadows;
            bool shadowChanged = ImGui::RadioButton("Original (hard)##shadows", &shadowIndex, 0);
            ImGui::SameLine();
            shadowChanged |= ImGui::RadioButton("Soft##shadows", &shadowIndex, 1);
            ImGui::SameLine();
            shadowChanged |= ImGui::RadioButton("Softer##shadows", &shadowIndex, 2);
            if (shadowChanged)
            {
                settings.shadows = (EngineSettings::Shadows)shadowIndex;
                changed = true;
            }
        }
        ImGui::TextUnformatted("Ambient occlusion (soft shading where surfaces meet)");
        {
            int ambientIndex = (int)settings.ambientOcclusion;
            bool ambientChanged = ImGui::RadioButton("Off##ao", &ambientIndex, 0);
            ImGui::SameLine();
            ambientChanged |= ImGui::RadioButton("Subtle##ao", &ambientIndex, 1);
            ImGui::SameLine();
            ambientChanged |= ImGui::RadioButton("Strong##ao", &ambientIndex, 2);
            if (ambientChanged)
            {
                settings.ambientOcclusion = (EngineSettings::AmbientOcclusion)ambientIndex;
                changed = true;
            }
            if (settings.ambientOcclusion != menu.startAmbientOcclusion)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(applies when you next start the game)");
            }
        }
        changed |= ImGui::Checkbox("Build new shaders in the background (no hitch when an effect first appears)",
                                   &settings.backgroundShaders);
        changed |= ImGui::Checkbox("Fast repeated objects (faster in busy towns; turn off if anything looks wrong)",
                                   &settings.repeatDraws);
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
            changed |= radioCap("Unlimited (up to your display's refresh rate, e.g. 120 or 144 Hz)", FrameRateCap::Unlimited);
            ImGui::TextDisabled("Above 30 fps the game logic may run differently (timing).");

            ImGui::SeparatorText("Controls");
            if (ImGui::Checkbox("Motion controls (move the controller to capture Folks)", &settings.motionControls))
            {
                engineSettingsApplyInput();
                changed = true;
            }
            {
                const char* padName = cellPad_host_pad_name();
                if (!settings.motionControls)
                {
                    ImGui::TextDisabled("Off: captures complete by themselves, with any controller.");
                }
                else if (padName && cellPad_motion_live())
                {
                    ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "%s connected: motion controls active.", padName);
                }
                else if (padName)
                {
                    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f),
                                       "%s connected, but its motion sensors are not reporting; captures complete by "
                                       "themselves.", padName);
                }
                else
                {
                    ImGui::TextDisabled("No motion controller found (DualShock 3 or 4, DualSense, Switch Pro); captures "
                                        "complete by themselves.");
                }
            }

            ImGui::SeparatorText("Opening logos");
            {
                int logoSkipIndex = (int)settings.logoSkip;
                bool logoSkipChanged = ImGui::RadioButton("Play them##logos", &logoSkipIndex, 0);
                ImGui::SameLine();
                logoSkipChanged |= ImGui::RadioButton("Start skips them##logos", &logoSkipIndex, 1);
                ImGui::SameLine();
                logoSkipChanged |= ImGui::RadioButton("Never show them##logos", &logoSkipIndex, 2);
                if (logoSkipChanged)
                {
                    settings.logoSkip = (EngineSettings::LogoSkip)logoSkipIndex;
                    changed = true;
                }
            }

            ImGui::SeparatorText("Physics");
            changed |= ImGui::Checkbox("Step physics by the frame time (fixes cloth and hair above 30 fps)",
                                       &settings.physicsFrameStep);

            ImGui::SeparatorText("Pacing");
            changed |= ImGui::Checkbox("Precise frame limiter (wake every 1 ms instead of 10 ms)", &settings.fineLimiterSleep);

            changed |= drawLanguage();
            changed |= drawGraphics();
            changed |= drawGameFiles();
            drawDownloadableContent();

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
        /* Start (or Enter, the key mapped to it) during the opening logos
         * skips the still logos that are left (EngineSettings::LogoSkip). */
        if (!menu.menuOpen && ((pressed & XINPUT_GAMEPAD_START) || (focused && keyDown(VK_RETURN))))
        {
            engineNoteLogoStart();
        }
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
            menu.startDepthOfField = engineSettings().depthOfField;
            menu.startBloom = engineSettings().bloom;
            menu.startAmbientOcclusion = engineSettings().ambientOcclusion;
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
