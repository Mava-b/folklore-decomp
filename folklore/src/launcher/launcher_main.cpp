/*
 * launcher_main.cpp -- FolkloreLauncher.exe: everything folklore.exe needs
 * set before it starts, in one window.
 *
 *   Game      the disc dump, the 1.10 update (1.10 builds), where saves and
 *             installed data go, firmware fonts;
 *   DLC       the packs installed, Add DLC from RPCS3..., Add licences;
 *   Graphics, Display, Gameplay
 *             the settings the in-game engine menu (F1) has, including the
 *             ones that only apply when the game starts;
 *   Play      saves folklore_settings.ini and starts folklore.exe beside it
 *             (with the texture dump on, when ticked for this run);
 *   Ko-fi     the project's donation page, in the browser.
 *
 * It edits the same folklore_settings.ini through the same code as the game
 * (engine_settings.cpp), checks folders with the game's own checks
 * (engine_portable.cpp) and installs DLC with the game's importer
 * (engine_dlc.cpp), so what it says is what the game will find.
 *
 * The picture on the left is launcher_art.jpg (or .png) beside the exe, or
 * in a folder above it; without one the panel is a plain gradient.
 *
 * Win32 + Direct3D 11 + Dear ImGui (src/third_party/imgui, MIT).
 */
/* windows.h would otherwise define min and max as macros, breaking std::min. */
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "engine_dlc.h"
#include "engine_portable.h"
#include "engine_region.h"
#include "engine_save_slots.h"
#include "engine_settings.h"

#include "imgui.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "stb_image.h"

#include <windows.h>
#include <d3d11.h>
#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND window, UINT message, WPARAM wideParam,
                                                             LPARAM longParam);

namespace
{
    /* ---- Look --------------------------------------------------------- */

    const ImVec4 kBackground(0.067f, 0.039f, 0.094f, 1.0f);     /* the cover's night purple */
    const ImVec4 kPanel(0.110f, 0.067f, 0.149f, 1.0f);
    const ImVec4 kAccent(0.765f, 0.380f, 0.596f, 1.0f);         /* the sky's rose */
    const ImVec4 kAccentHover(0.851f, 0.471f, 0.682f, 1.0f);
    const ImVec4 kText(0.925f, 0.882f, 0.957f, 1.0f);
    const ImVec4 kTextDim(0.643f, 0.569f, 0.706f, 1.0f);
    const ImVec4 kGood(0.561f, 0.871f, 0.592f, 1.0f);
    const ImVec4 kWarning(1.0f, 0.765f, 0.400f, 1.0f);
    const ImVec4 kBad(1.0f, 0.502f, 0.451f, 1.0f);

    /* ---- Direct3D ------------------------------------------------------- */

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* deviceContext = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    UINT pendingWidth = 0;
    UINT pendingHeight = 0;

    void createRenderTarget()
    {
        ID3D11Texture2D* backBuffer = nullptr;
        swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
        if (backBuffer)
        {
            device->CreateRenderTargetView(backBuffer, nullptr, &renderTarget);
            backBuffer->Release();
        }
    }

    void releaseRenderTarget()
    {
        if (renderTarget)
        {
            renderTarget->Release();
            renderTarget = nullptr;
        }
    }

    bool createDevice(HWND window)
    {
        DXGI_SWAP_CHAIN_DESC description = {};
        description.BufferCount = 2;
        description.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.BufferDesc.RefreshRate.Numerator = 60;
        description.BufferDesc.RefreshRate.Denominator = 1;
        description.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
        description.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        description.OutputWindow = window;
        description.SampleDesc.Count = 1;
        description.Windowed = TRUE;
        description.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
        D3D_FEATURE_LEVEL chosenLevel;
        HRESULT result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2,
                                                       D3D11_SDK_VERSION, &description, &swapChain, &device,
                                                       &chosenLevel, &deviceContext);
        if (result == DXGI_ERROR_UNSUPPORTED)
        {
            result = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                                   D3D11_SDK_VERSION, &description, &swapChain, &device,
                                                   &chosenLevel, &deviceContext);
        }
        if (FAILED(result))
        {
            return false;
        }
        createRenderTarget();
        return true;
    }

    void destroyDevice()
    {
        releaseRenderTarget();
        if (swapChain)
        {
            swapChain->Release();
        }
        if (deviceContext)
        {
            deviceContext->Release();
        }
        if (device)
        {
            device->Release();
        }
    }

    /* ---- The cover art ---------------------------------------------------- */

    ID3D11ShaderResourceView* artView = nullptr;
    int artWidth = 0;
    int artHeight = 0;

    bool fileExists(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::string parentFolder(const std::string& path)
    {
        const size_t separator = path.find_last_of("\\/");
        return separator == std::string::npos ? std::string() : path.substr(0, separator);
    }

    /* launcher_art.jpg / .png beside the exe, or up to three folders above
     * it (a development build sits in folklore\build\...). */
    std::string findArt()
    {
        std::string folder = engineExecutableDirectory();
        for (int depth = 0; depth < 4 && !folder.empty(); depth++)
        {
            for (const char* name : {"launcher_art.jpg", "launcher_art.png"})
            {
                const std::string candidate = folder + "\\" + name;
                if (fileExists(candidate))
                {
                    return candidate;
                }
            }
            folder = parentFolder(folder);
        }
        return std::string();
    }

    /* A JPEG or PNG as a texture ImGui can draw; nullptr when it cannot be
     * read. */
    ID3D11ShaderResourceView* loadImageTexture(const std::string& path, int* imageWidth, int* imageHeight)
    {
        int channels = 0;
        unsigned char* pixels = stbi_load(path.c_str(), imageWidth, imageHeight, &channels, 4);
        if (!pixels)
        {
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC description = {};
        description.Width = (UINT)*imageWidth;
        description.Height = (UINT)*imageHeight;
        description.MipLevels = 1;
        description.ArraySize = 1;
        description.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        description.SampleDesc.Count = 1;
        description.Usage = D3D11_USAGE_DEFAULT;
        description.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        D3D11_SUBRESOURCE_DATA initial = {};
        initial.pSysMem = pixels;
        initial.SysMemPitch = (UINT)*imageWidth * 4u;
        ID3D11Texture2D* texture = nullptr;
        ID3D11ShaderResourceView* view = nullptr;
        if (SUCCEEDED(device->CreateTexture2D(&description, &initial, &texture)))
        {
            device->CreateShaderResourceView(texture, nullptr, &view);
            texture->Release();
        }
        stbi_image_free(pixels);
        return view;
    }

    void loadArt()
    {
        const std::string path = findArt();
        if (path.empty())
        {
            return;
        }
        artView = loadImageTexture(path, &artWidth, &artHeight);
    }

    /* ---- Fonts ------------------------------------------------------------ */

    ImFont* bodyFont = nullptr;
    ImFont* headingFont = nullptr;
    ImFont* titleFont = nullptr;

    void loadFonts(float scale)
    {
        ImGuiIO& io = ImGui::GetIO();
        io.Fonts->Clear();
        char windowsFolder[MAX_PATH] = {};
        GetWindowsDirectoryA(windowsFolder, MAX_PATH);
        const std::string fonts = std::string(windowsFolder) + "\\Fonts\\";
        const std::string body = fonts + "segoeui.ttf";
        std::string heading = fonts + "pala.ttf";     /* Palatino Linotype: close to the logo's serif */
        if (!fileExists(heading))
        {
            heading = fonts + "georgia.ttf";
        }
        bodyFont = fileExists(body) ? io.Fonts->AddFontFromFileTTF(body.c_str(), 18.0f * scale) : nullptr;
        if (!bodyFont)
        {
            bodyFont = io.Fonts->AddFontDefault();
        }
        headingFont = fileExists(heading) ? io.Fonts->AddFontFromFileTTF(heading.c_str(), 26.0f * scale) : nullptr;
        titleFont = fileExists(heading) ? io.Fonts->AddFontFromFileTTF(heading.c_str(), 64.0f * scale) : nullptr;
        if (!headingFont)
        {
            headingFont = bodyFont;
        }
        if (!titleFont)
        {
            titleFont = headingFont;
        }
    }

    void applyStyle(float scale)
    {
        ImGuiStyle& style = ImGui::GetStyle();
        style = ImGuiStyle();
        style.WindowRounding = 0.0f;
        style.FrameRounding = 6.0f;
        style.GrabRounding = 6.0f;
        style.TabRounding = 6.0f;
        style.ChildRounding = 8.0f;
        style.PopupRounding = 6.0f;
        style.FramePadding = ImVec2(10.0f, 6.0f);
        style.ItemSpacing = ImVec2(10.0f, 8.0f);
        style.WindowPadding = ImVec2(28.0f, 22.0f);
        style.ScrollbarSize = 12.0f;
        style.WindowBorderSize = 0.0f;
        style.ChildBorderSize = 0.0f;
        ImVec4* colours = style.Colors;
        colours[ImGuiCol_Text] = kText;
        colours[ImGuiCol_TextDisabled] = kTextDim;
        colours[ImGuiCol_WindowBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colours[ImGuiCol_ChildBg] = ImVec4(kPanel.x, kPanel.y, kPanel.z, 0.82f);
        colours[ImGuiCol_PopupBg] = ImVec4(0.13f, 0.08f, 0.17f, 0.98f);
        colours[ImGuiCol_Border] = ImVec4(0.35f, 0.22f, 0.42f, 0.6f);
        colours[ImGuiCol_FrameBg] = ImVec4(0.20f, 0.12f, 0.25f, 1.0f);
        colours[ImGuiCol_FrameBgHovered] = ImVec4(0.27f, 0.16f, 0.33f, 1.0f);
        colours[ImGuiCol_FrameBgActive] = ImVec4(0.33f, 0.19f, 0.39f, 1.0f);
        colours[ImGuiCol_Button] = ImVec4(0.27f, 0.15f, 0.33f, 1.0f);
        colours[ImGuiCol_ButtonHovered] = ImVec4(0.38f, 0.21f, 0.44f, 1.0f);
        colours[ImGuiCol_ButtonActive] = ImVec4(0.46f, 0.25f, 0.52f, 1.0f);
        colours[ImGuiCol_Header] = ImVec4(0.30f, 0.17f, 0.36f, 1.0f);
        colours[ImGuiCol_HeaderHovered] = ImVec4(0.40f, 0.22f, 0.46f, 1.0f);
        colours[ImGuiCol_HeaderActive] = ImVec4(0.48f, 0.26f, 0.54f, 1.0f);
        colours[ImGuiCol_Tab] = ImVec4(0.17f, 0.10f, 0.21f, 1.0f);
        colours[ImGuiCol_TabHovered] = kAccentHover;
        colours[ImGuiCol_TabSelected] = kAccent;
        colours[ImGuiCol_TabSelectedOverline] = kAccentHover;
        colours[ImGuiCol_CheckMark] = kAccentHover;
        colours[ImGuiCol_SliderGrab] = kAccent;
        colours[ImGuiCol_SliderGrabActive] = kAccentHover;
        colours[ImGuiCol_Separator] = ImVec4(0.35f, 0.22f, 0.42f, 0.8f);
        colours[ImGuiCol_PlotHistogram] = kAccent;
        colours[ImGuiCol_ScrollbarBg] = ImVec4(0.0f, 0.0f, 0.0f, 0.0f);
        colours[ImGuiCol_ScrollbarGrab] = ImVec4(0.35f, 0.22f, 0.42f, 1.0f);
        style.ScaleAllSizes(scale);
    }

    /* ---- State ------------------------------------------------------------ */

    struct FolderCheck
    {
        std::string checkedFolder;
        bool good = false;
        std::string problem;
    };

    FolderCheck gameCheck;
    FolderCheck updateCheck;
    std::string launchProblem;
    bool settingsDirty = false;
    /* Graphics page: run the game with the texture dump on (RSX_TEXTURE_DUMP,
     * as run.ps1 -DumpTextures). For this run only: dumping makes new areas
     * hitch while it writes, so it is not kept in the settings. */
    bool dumpTexturesThisRun = false;
    /* folklore.exe once Play has started it, until its window is up. */
    HANDLE gameProcess = nullptr;
    const char* const kDonateUrl = "https://ko-fi.com/mavab";

    /* Where the texture dump goes: textures\dump in the data folder, the
     * one run.ps1 -DumpTextures uses in a project folder. */
    std::string textureDumpFolder()
    {
        return engineDataDirectory() + "\\textures\\dump";
    }

    /* The DLC importer and the licence import run on a thread of their own,
     * with progress shown here (as in the engine menu). */
    struct DlcWork
    {
        std::mutex lock;
        bool running = false;
        int filesDone = 0;
        int filesTotal = 0;
        std::string currentFile;
        std::string report;
        bool refresh = true;
        std::vector<int> installed;
        std::vector<int> pendingRemovals;
    };

    DlcWork dlcWork;

    /* The Saves page: the slots on disk (re-read after any change) and the
     * import/export running on a worker thread (the folder picker blocks). */
    struct SaveWork
    {
        std::mutex lock;
        bool running = false;
        bool refresh = true;
        std::string report;
        int selectAfterImport = -1;
    };

    SaveWork saveWork;
    std::vector<SaveSlot> saveSlots;
    int deleteConfirmSlot = -1;

    /* Each save's picture (its ICON0.PNG: the area it was saved in), loaded
     * once per file and time. */
    struct SlotPicture
    {
        std::string path;
        long long savedAtSeconds = 0;
        ID3D11ShaderResourceView* view = nullptr;
        int width = 0;
        int height = 0;
    };

    std::vector<SlotPicture> slotPictures;

    void releaseSlotPictures()
    {
        for (SlotPicture& picture : slotPictures)
        {
            if (picture.view)
            {
                picture.view->Release();
            }
        }
        slotPictures.clear();
    }

    const SlotPicture* slotPicture(const SaveSlot& slot)
    {
        if (slot.iconPath.empty())
        {
            return nullptr;
        }
        for (const SlotPicture& picture : slotPictures)
        {
            if (picture.path == slot.iconPath && picture.savedAtSeconds == slot.savedAtSeconds)
            {
                return picture.view ? &picture : nullptr;
            }
        }
        SlotPicture picture;
        picture.path = slot.iconPath;
        picture.savedAtSeconds = slot.savedAtSeconds;
        picture.view = loadImageTexture(slot.iconPath, &picture.width, &picture.height);
        slotPictures.push_back(picture);
        return slotPictures.back().view ? &slotPictures.back() : nullptr;
    }

    void startSaveImport()
    {
        {
            std::lock_guard<std::mutex> guard(saveWork.lock);
            if (saveWork.running)
            {
                return;
            }
            saveWork.running = true;
            saveWork.report = "Choosing a folder...";
        }
        std::thread([]() {
            const std::string picked = enginePickFolder(
                std::string(), "Folklore: choose a save, a folder of saves, or RPCS3's folder to import from");
            std::string report;
            int firstSlot = -1;
            if (!picked.empty())
            {
                report = engineSaveSlotImport(picked, &firstSlot);
            }
            std::lock_guard<std::mutex> guard(saveWork.lock);
            saveWork.report = report;
            saveWork.selectAfterImport = firstSlot;
            saveWork.running = false;
            saveWork.refresh = true;
        }).detach();
    }

    void startSaveExport(int number)
    {
        {
            std::lock_guard<std::mutex> guard(saveWork.lock);
            if (saveWork.running)
            {
                return;
            }
            saveWork.running = true;
            saveWork.report = "Choosing a folder...";
        }
        std::thread([number]() {
            const std::string picked = enginePickFolder(
                std::string(), "Folklore: choose where to export save slot " + std::to_string(number) + " to");
            std::string report;
            if (!picked.empty())
            {
                report = engineSaveSlotExport(number, picked);
            }
            std::lock_guard<std::mutex> guard(saveWork.lock);
            saveWork.report = report;
            saveWork.running = false;
            saveWork.refresh = true;
        }).detach();
    }

    void checkFolders()
    {
        const EngineSettings& settings = engineSettings();
        const std::string gameFolder = engineNormaliseGameFolder(settings.gameDir);
        if (gameFolder != gameCheck.checkedFolder || gameCheck.checkedFolder.empty())
        {
            gameCheck.checkedFolder = gameFolder;
            gameCheck.problem.clear();
            gameCheck.good = !gameFolder.empty() && engineCheckGameFolder(gameFolder, &gameCheck.problem);
            if (gameFolder.empty())
            {
                gameCheck.problem = "Not chosen yet.";
            }
        }
        if (FOLKLORE_USES_UPDATE)
        {
            const std::string updateFolder = engineNormaliseUpdateFolder(settings.updateDir);
            if (updateFolder != updateCheck.checkedFolder || updateCheck.checkedFolder.empty())
            {
                updateCheck.checkedFolder = updateFolder;
                updateCheck.problem.clear();
                updateCheck.good = !updateFolder.empty() && engineCheckUpdateFolder(updateFolder, &updateCheck.problem);
                if (updateFolder.empty())
                {
                    updateCheck.problem = "Not chosen yet.";
                }
            }
        }
    }

    /* Where hdd0 is for the DLC code: as folklore.exe sets it. */
    void setDataEnvironment()
    {
        const std::string dataDirectory = engineDataDirectory();
        CreateDirectoryA(dataDirectory.c_str(), nullptr);
        char existing[MAX_PATH * 2];
        if (GetEnvironmentVariableA("PS3_HDD0_ROOT", existing, (DWORD)sizeof(existing)) > 0)
        {
            return;     /* set by whoever started the launcher, as for the game */
        }
        const std::string hdd0 = dataDirectory + "\\hdd0";
        CreateDirectoryA(hdd0.c_str(), nullptr);
        SetEnvironmentVariableA("PS3_HDD0_ROOT", hdd0.c_str());
    }

    void startDlcImport()
    {
        {
            std::lock_guard<std::mutex> guard(dlcWork.lock);
            if (dlcWork.running)
            {
                return;
            }
            dlcWork.running = true;
            dlcWork.filesDone = 0;
            dlcWork.filesTotal = 0;
            dlcWork.currentFile = "Choosing a folder...";
            dlcWork.report.clear();
        }
        const std::string startFolder = parentFolder(engineSettings().updateDir);
        std::thread([startFolder]() {
            const std::string picked = enginePickFolder(
                startFolder, "Folklore: choose RPCS3's folder (the one with dev_hdd0 in it) to add the DLC from");
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
                        std::lock_guard<std::mutex> guard(dlcWork.lock);
                        dlcWork.filesDone = done;
                        dlcWork.filesTotal = total;
                        dlcWork.currentFile = file;
                    });
                }
            }
            std::lock_guard<std::mutex> guard(dlcWork.lock);
            dlcWork.report = report;
            dlcWork.running = false;
            dlcWork.refresh = true;
        }).detach();
    }

    void startLicenceImport()
    {
        {
            std::lock_guard<std::mutex> guard(dlcWork.lock);
            if (dlcWork.running)
            {
                return;
            }
            dlcWork.running = true;
            dlcWork.filesDone = 0;
            dlcWork.filesTotal = 0;
            dlcWork.currentFile = "Choosing licences...";
            dlcWork.report.clear();
        }
        std::thread([]() {
            const std::vector<std::string> picked = enginePickFiles(
                std::string(), "Folklore: choose your DLC licences (.rap files)", "Licences (*.rap)", "*.rap");
            std::string report;
            if (!picked.empty())
            {
                {
                    std::lock_guard<std::mutex> guard(dlcWork.lock);
                    dlcWork.currentFile = "Unlocking the installed packs...";
                }
                report = engineDlcAddLicences(picked);
            }
            std::lock_guard<std::mutex> guard(dlcWork.lock);
            dlcWork.report = report;
            dlcWork.running = false;
            dlcWork.refresh = true;
        }).detach();
    }

    /* ---- Widgets ------------------------------------------------------------ */

    void sectionHeading(const char* text)
    {
        ImGui::Spacing();
        ImGui::PushFont(headingFont);
        ImGui::TextColored(kAccentHover, "%s", text);
        ImGui::PopFont();
        ImGui::Separator();
        ImGui::Spacing();
    }

    void note(const char* text)
    {
        ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
        ImGui::TextWrapped("%s", text);
        ImGui::PopStyleColor();
    }

    /* A combo over a list of labels; true when the choice changed. */
    bool choice(const char* label, int* index, const std::vector<const char*>& options, float width = 320.0f)
    {
        ImGui::SetNextItemWidth(width * ImGui::GetIO().FontGlobalScale);
        bool changed = false;
        const int current = std::clamp(*index, 0, (int)options.size() - 1);
        if (ImGui::BeginCombo(label, options[(size_t)current]))
        {
            for (int optionIndex = 0; optionIndex < (int)options.size(); optionIndex++)
            {
                const bool selected = optionIndex == current;
                if (ImGui::Selectable(options[(size_t)optionIndex], selected))
                {
                    *index = optionIndex;
                    changed = true;
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        return changed;
    }

    template <typename EnumType>
    bool enumChoice(const char* label, EnumType* value, const std::vector<const char*>& options)
    {
        int index = (int)*value;
        if (choice(label, &index, options))
        {
            *value = (EnumType)index;
            return true;
        }
        return false;
    }

    /* A folder row: the path, Browse..., and what the check found. */
    bool folderRow(const char* label, std::string* folder, const FolderCheck* check, const char* pickerTitle,
                   bool forUpdate, bool isGameFolder)
    {
        bool changed = false;
        ImGui::PushID(label);
        ImGui::TextUnformatted(label);
        ImGui::SetNextItemWidth(-130.0f);
        char buffer[MAX_PATH * 2] = {};
        std::snprintf(buffer, sizeof(buffer), "%s", folder->c_str());
        if (ImGui::InputTextWithHint("##path", "(not chosen)", buffer, sizeof(buffer)))
        {
            *folder = buffer;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Browse...", ImVec2(-1.0f, 0.0f)))
        {
            std::string picked;
            if (isGameFolder)
            {
                picked = enginePickGameFolder(*folder, forUpdate);
            }
            else
            {
                picked = enginePickFolder(*folder, pickerTitle);
            }
            if (!picked.empty())
            {
                *folder = picked;
                changed = true;
            }
        }
        if (check)
        {
            if (check->good)
            {
                ImGui::TextColored(kGood, "Found.");
            }
            else
            {
                ImGui::PushStyleColor(ImGuiCol_Text, folder->empty() ? kWarning : kBad);
                ImGui::TextWrapped("%s", check->problem.c_str());
                ImGui::PopStyleColor();
            }
        }
        ImGui::PopID();
        ImGui::Spacing();
        return changed;
    }

    /* ---- Pages ----------------------------------------------------------- */

    bool drawGamePage()
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;
        sectionHeading("Your game");
        if (FOLKLORE_USES_UPDATE)
        {
            note("This build runs the " FOLKLORE_RELEASE_NAME " release (" FOLKLORE_TITLE_ID ") with its 1.10 update. "
                 "It needs your disc dump and the installed 1.10 update (in RPCS3: dev_hdd0\\game\\" FOLKLORE_TITLE_ID
                 ").");
        }
        else
        {
            note("This build runs the " FOLKLORE_RELEASE_NAME " release (" FOLKLORE_TITLE_ID ") v1.00. It needs your "
                 "disc dump.");
        }
        ImGui::Spacing();
        changed |= folderRow("Disc dump (the folder that contains PS3_GAME)", &settings.gameDir, &gameCheck, "", false,
                             true);
        if (FOLKLORE_USES_UPDATE)
        {
            changed |= folderRow("1.10 update (the folder with PARAM.SFO and USRDIR)", &settings.updateDir, &updateCheck,
                                 "", true, true);
        }

        sectionHeading("Where things go");
        changed |= folderRow("Saves and installed game data (empty: beside the game)", &settings.dataDir, nullptr,
                             "Folklore: choose where saves and installed game data go", false, false);
        note("Saves, the game's installed data (several GB), DLC, the shader cache and logs go here.");
        ImGui::Spacing();
        changed |= folderRow("Firmware fonts: dev_flash (optional)", &settings.devFlashDir, nullptr,
                             "Folklore: choose a firmware dev_flash folder", false, false);
        return changed;
    }

    void drawDlcPage()
    {
        bool running = false;
        int filesDone = 0;
        int filesTotal = 0;
        std::string currentFile;
        std::string report;
        std::vector<int> installed;
        std::vector<int> pendingRemovals;
        {
            std::lock_guard<std::mutex> guard(dlcWork.lock);
            if (dlcWork.refresh && !dlcWork.running)
            {
                dlcWork.installed = engineDlcInstalledPacks();
                dlcWork.pendingRemovals = engineDlcPendingRemovals();
                dlcWork.refresh = false;
            }
            running = dlcWork.running;
            filesDone = dlcWork.filesDone;
            filesTotal = dlcWork.filesTotal;
            currentFile = dlcWork.currentFile;
            report = dlcWork.report;
            installed = dlcWork.installed;
            pendingRemovals = dlcWork.pendingRemovals;
        }

        sectionHeading("Downloadable content");
        const std::string repairStatus = engineDlcRepairStatus();
        if (!repairStatus.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, kBad);
            ImGui::TextWrapped("%s", repairStatus.c_str());
            ImGui::PopStyleColor();
            ImGui::Spacing();
        }
        if (installed.empty())
        {
            note("No DLC installed.");
        }
        else
        {
            ImGui::Text("Installed (%d):", (int)installed.size());
            for (int pack : installed)
            {
                const bool leaving = std::find(pendingRemovals.begin(), pendingRemovals.end(), pack) !=
                                     pendingRemovals.end();
                ImGui::BulletText("%s%s", engineDlcPackNameByNumber(pack).c_str(),
                                  leaving ? "  (removed at the next start)" : "");
            }
            note("Remove packs from the in-game menu (F1 > Downloadable content); it copies your saves first.");
        }
        ImGui::Spacing();
        ImGui::BeginDisabled(running);
        if (ImGui::Button("Add DLC from RPCS3..."))
        {
            startDlcImport();
        }
        ImGui::SameLine();
        if (ImGui::Button("Add licences (.rap)..."))
        {
            startLicenceImport();
        }
        ImGui::EndDisabled();
        note("Add DLC from RPCS3: install each pack's .pkg and .rap in RPCS3 (File > Install Packages/Raps/Edats), then "
             "pick RPCS3's folder (the one with dev_hdd0 in it). Copied the game's folder in by hand? Add licences "
             "takes the packs' .rap files (RPCS3 keeps them in dev_hdd0\\home\\00000001\\exdata).");
        if (running)
        {
            const float fraction = filesTotal > 0 ? (float)filesDone / (float)filesTotal : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof(overlay), "%d / %d files", filesDone, filesTotal);
            ImGui::ProgressBar(fraction, ImVec2(-1.0f, 0.0f), filesTotal > 0 ? overlay : "");
            note(currentFile.c_str());
        }
        else if (!report.empty())
        {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", report.c_str());
        }
    }

    /* One slot as a card: the area picture, where and when it was saved,
     * and Use. True when the player chose it (Use, or a click on the card). */
    bool drawSaveSlotCard(const SaveSlot& slot, bool inUse)
    {
        const float scale = ImGui::GetIO().FontGlobalScale;
        const float pictureWidth = 208.0f * scale;
        const float pictureHeight = 114.0f * scale;
        const float padding = 10.0f * scale;
        const float useWidth = 120.0f * scale;
        bool chosen = false;
        ImGui::PushID(slot.number);
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              inUse ? ImVec4(0.29f, 0.14f, 0.26f, 0.96f) : ImVec4(0.15f, 0.09f, 0.19f, 0.92f));
        ImGui::PushStyleColor(ImGuiCol_Border, inUse ? kAccent : ImVec4(0.27f, 0.18f, 0.32f, 1.0f));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 6.0f * scale);
        ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, inUse ? 2.0f : 1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(padding, padding));
        ImGui::BeginChild("##slot", ImVec2(0.0f, pictureHeight + padding * 2.0f),
                          ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
        {
            const SlotPicture* picture = slotPicture(slot);
            if (picture)
            {
                ImGui::Image((ImTextureID)(intptr_t)picture->view, ImVec2(pictureWidth, pictureHeight));
            }
            else
            {
                /* No picture: an empty slot, or a save without ICON0.PNG. */
                const ImVec2 corner = ImGui::GetCursorScreenPos();
                ImDrawList* drawList = ImGui::GetWindowDrawList();
                drawList->AddRectFilled(corner, ImVec2(corner.x + pictureWidth, corner.y + pictureHeight),
                                        ImGui::GetColorU32(ImVec4(0.09f, 0.05f, 0.12f, 1.0f)), 4.0f * scale);
                const char* label = slot.exists ? "No picture" : "New game";
                const ImVec2 labelSize = ImGui::CalcTextSize(label);
                drawList->AddText(ImVec2(corner.x + (pictureWidth - labelSize.x) * 0.5f,
                                         corner.y + (pictureHeight - labelSize.y) * 0.5f),
                                  ImGui::GetColorU32(kTextDim), label);
                ImGui::Dummy(ImVec2(pictureWidth, pictureHeight));
            }
            ImGui::SameLine(0.0f, padding * 1.6f);
            const float textWidth = ImGui::GetContentRegionAvail().x - useWidth - padding;
            ImGui::BeginGroup();
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + textWidth);
            ImGui::TextColored(kAccentHover, "Slot %d", slot.number);
            ImGui::SameLine();
            if (slot.exists)
            {
                ImGui::TextColored(kText, "%s", slot.subtitle.empty() ? "Saved game" : slot.subtitle.c_str());
            }
            else
            {
                ImGui::TextColored(kTextDim, "Empty");
            }
            if (slot.exists)
            {
                /* DETAIL: each character's chapter, then the play time. */
                std::string chapters;
                std::string playTime;
                for (const std::string& line : slot.detailLines)
                {
                    if (line.rfind("Play time", 0) == 0)
                    {
                        playTime = line;
                    }
                    else
                    {
                        chapters += (chapters.empty() ? "" : "   \xC2\xB7   ") + line;
                    }
                }
                if (!chapters.empty())
                {
                    ImGui::TextColored(kText, "%s", chapters.c_str());
                }
                std::string when = playTime;
                if (!slot.savedAt.empty())
                {
                    when += (when.empty() ? "Saved " : "   \xC2\xB7   Saved ") + slot.savedAt;
                }
                if (!when.empty())
                {
                    ImGui::TextColored(kTextDim, "%s", when.c_str());
                }
                if (!slot.otherRelease.empty())
                {
                    ImGui::TextColored(kWarning, "Made with the %s release: it may not load in this one.",
                                       slot.otherRelease.c_str());
                }
            }
            else
            {
                ImGui::TextColored(kTextDim, "Play starts a new game here, and the game saves into this slot.");
            }
            ImGui::PopTextWrapPos();
            ImGui::EndGroup();
            ImGui::SameLine(ImGui::GetWindowWidth() - useWidth - padding);
            if (inUse)
            {
                ImGui::TextColored(kGood, "In use");
            }
            else if (ImGui::Button("Use", ImVec2(useWidth, 0.0f)))
            {
                chosen = true;
            }
            if (!inUse && ImGui::IsWindowHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
                !ImGui::IsAnyItemHovered())
            {
                chosen = true;
            }
        }
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        ImGui::PopStyleColor(2);
        ImGui::PopID();
        ImGui::Spacing();
        return chosen;
    }

    bool drawSavesPage()
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;
        bool running = false;
        std::string report;
        {
            std::lock_guard<std::mutex> guard(saveWork.lock);
            if (saveWork.refresh && !saveWork.running)
            {
                releaseSlotPictures();
                saveSlots = engineSaveSlotList();
                saveWork.refresh = false;
                if (saveWork.selectAfterImport >= 0)
                {
                    settings.saveSlot = saveWork.selectAfterImport;
                    saveWork.selectAfterImport = -1;
                    changed = true;
                }
            }
            running = saveWork.running;
            report = saveWork.report;
        }

        sectionHeading("Save slots");
        note("The game loads and saves only the slot chosen here: choose one, then press Play. An empty slot starts "
             "a new game. Changing slot takes effect the next time the game starts.");
        bool selectedHasSave = false;
        for (const SaveSlot& slot : saveSlots)
        {
            selectedHasSave = selectedHasSave || slot.number == settings.saveSlot;
        }
        ImGui::BeginDisabled(running);
        if (ImGui::Button("New slot"))
        {
            settings.saveSlot = engineSaveSlotNextFree();
            deleteConfirmSlot = -1;
            changed = true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Import..."))
        {
            deleteConfirmSlot = -1;
            startSaveImport();
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(!selectedHasSave);
        const std::string exportLabel = "Export slot " + std::to_string(settings.saveSlot) + "...";
        if (ImGui::Button(exportLabel.c_str()))
        {
            deleteConfirmSlot = -1;
            startSaveExport(settings.saveSlot);
        }
        ImGui::SameLine();
        if (deleteConfirmSlot == settings.saveSlot)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.60f, 0.22f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kBad);
            const std::string confirmLabel = "Confirm: delete slot " + std::to_string(settings.saveSlot);
            if (ImGui::Button(confirmLabel.c_str()))
            {
                const std::string result = engineSaveSlotDelete(settings.saveSlot);
                std::lock_guard<std::mutex> guard(saveWork.lock);
                saveWork.report = result;
                saveWork.refresh = true;
                deleteConfirmSlot = -1;
            }
            ImGui::PopStyleColor(2);
            ImGui::SameLine();
            if (ImGui::Button("Cancel"))
            {
                deleteConfirmSlot = -1;
                std::lock_guard<std::mutex> guard(saveWork.lock);
                saveWork.report.clear();
            }
        }
        else
        {
            const std::string deleteLabel = "Delete slot " + std::to_string(settings.saveSlot) + "...";
            if (ImGui::Button(deleteLabel.c_str()))
            {
                deleteConfirmSlot = settings.saveSlot;
                std::lock_guard<std::mutex> guard(saveWork.lock);
                saveWork.report = "Delete slot " + std::to_string(settings.saveSlot) +
                                  "? It is moved to the deleted_saves folder, not erased. Press Confirm to go ahead.";
            }
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        if (!report.empty())
        {
            ImGui::Spacing();
            ImGui::TextWrapped("%s", report.c_str());
        }
        ImGui::Spacing();
        if (!selectedHasSave)
        {
            /* The chosen slot is empty (New slot, or its save was deleted):
             * first, so it is seen. */
            drawSaveSlotCard(engineSaveSlotRead(settings.saveSlot), true);
        }
        for (const SaveSlot& slot : saveSlots)
        {
            if (drawSaveSlotCard(slot, slot.number == settings.saveSlot))
            {
                settings.saveSlot = slot.number;
                deleteConfirmSlot = -1;
                changed = true;
            }
        }

        note("Import takes one save folder, a folder of them (an export, or RPCS3's dev_hdd0\\home\\00000001\\"
             "savedata) or RPCS3's own folder; each save gets the next free slot. Export writes the slot as a PS3 "
             "save folder that RPCS3 can use as it is.");
        return changed;
    }

    bool drawGraphicsPage()
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;
        sectionHeading("Picture");
        int scaleIndex = std::clamp(settings.renderScale, 1, 3) - 1;
        if (choice("Render resolution", &scaleIndex, {"1x (1280x720, the PS3's)", "2x (2560x1440)", "3x (3840x2160)"}))
        {
            settings.renderScale = scaleIndex + 1;
            changed = true;
        }
        changed |= enumChoice("Aspect ratio", &settings.aspectRatio,
                              {"16:9 (the PS3's)", "Match the display", "21:9 ultrawide", "32:9 super ultrawide"});
        changed |= enumChoice("Anti-aliasing", &settings.antiAliasing, {"Off", "FXAA (softer)", "SMAA (sharp edges)"});
        {
            const std::vector<int> levels = {1, 2, 4, 8, 16};
            int levelIndex = 0;
            for (int index = 0; index < (int)levels.size(); index++)
            {
                if (levels[(size_t)index] == settings.anisotropy)
                {
                    levelIndex = index;
                }
            }
            if (choice("Anisotropic filtering", &levelIndex, {"Off", "2x", "4x", "8x", "16x"}))
            {
                settings.anisotropy = levels[(size_t)levelIndex];
                changed = true;
            }
        }
        changed |= enumChoice("Texture detail", &settings.textureDetail, {"Original (the PS3's)", "Sharper", "Sharpest"});
        ImGui::SetNextItemWidth(320.0f * ImGui::GetIO().FontGlobalScale);
        changed |= ImGui::SliderInt("Sharpening", &settings.sharpening, 0, 100, "%d%%");
        changed |= ImGui::Checkbox("High-resolution textures (texture pack in textures\\replace)", &settings.texturePack);

        sectionHeading("Texture dump");
        ImGui::Checkbox("Dump textures this run (for making a texture pack)", &dumpTexturesThisRun);
        note("Every texture the game shows is saved once, as a DDS file named by the key a replacement is found by. "
             "The game hitches briefly when a new area loads while it writes. Only for the next Play; it is not kept.");
        if (dumpTexturesThisRun)
        {
            ImGui::TextColored(kTextDim, "Into: %s", textureDumpFolder().c_str());
        }

        sectionHeading("Effects");
        changed |= enumChoice("Depth of field", &settings.depthOfField, {"Original (the PS3's)", "Smooth"});
        changed |= enumChoice("Bloom", &settings.bloom, {"Original (the PS3's)", "Smooth (no flicker)"});
        changed |= enumChoice("Id souls", &settings.idSouls,
                              {"Original (static)", "Smooth (soft shimmer)", "Clear (no pattern)"});
        changed |= enumChoice("Shadows", &settings.shadows, {"Original (hard)", "Soft", "Softer"});
        changed |= enumChoice("Ambient occlusion", &settings.ambientOcclusion, {"Off", "Subtle", "Strong"});

        sectionHeading("Performance");
        changed |= ImGui::Checkbox("Build new shaders in the background (no hitch when an effect first appears)",
                                   &settings.backgroundShaders);
        changed |= ImGui::Checkbox("Fast repeated objects (faster in busy towns)", &settings.repeatDraws);
        return changed;
    }

    bool drawDisplayPage()
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;
        sectionHeading("Window and frame rate");
        changed |= ImGui::Checkbox("Fullscreen (Alt+Enter in game toggles it)", &settings.fullscreen);
        changed |= enumChoice("Frame rate", &settings.frameRateCap,
                              {"30 fps (the game's own)", "60 fps", "Unlimited (up to the display's refresh)"});
        if (settings.frameRateCap != FrameRateCap::Original)
        {
            note("Above 30 fps is experimental: the game was built for 30.");
        }
        changed |= ImGui::Checkbox("Precise frame limiter (wake every 1 ms instead of 10 ms)",
                                   &settings.fineLimiterSleep);

        sectionHeading("Menus and HUD (wider than 16:9)");
        {
            /* The setting's order is Stretched, Centred, Edges; shown best first. */
            const EngineSettings::HudLayout layouts[3] = {EngineSettings::HudLayout::Edges,
                                                          EngineSettings::HudLayout::Centred,
                                                          EngineSettings::HudLayout::Stretched};
            int layoutIndex = 0;
            for (int index = 0; index < 3; index++)
            {
                if (layouts[index] == settings.hudLayout)
                {
                    layoutIndex = index;
                }
            }
            if (choice("Layout", &layoutIndex, {"Map and gauges at the edges", "Centred", "Stretched to fit"}))
            {
                settings.hudLayout = layouts[layoutIndex];
                changed = true;
            }
            if (settings.hudLayout == EngineSettings::HudLayout::Edges)
            {
                ImGui::SetNextItemWidth(320.0f * ImGui::GetIO().FontGlobalScale);
                changed |= ImGui::SliderInt("How far out", &settings.hudEdge, 0, 100, "%d%%");
            }
        }

        sectionHeading("On screen");
        changed |= ImGui::Checkbox("Performance overlay", &settings.showPerformanceOverlay);
        changed |= ImGui::Checkbox("\"Game saved\" notice", &settings.showSaveNotice);
        return changed;
    }

    bool drawGameplayPage()
    {
        EngineSettings& settings = engineSettings();
        bool changed = false;
        sectionHeading("Controls");
        changed |= ImGui::Checkbox("Motion controls (move the controller to capture Folks)", &settings.motionControls);
        note(settings.motionControls
                 ? "Works with a DualShock 3 or 4, DualSense or Switch Pro controller; one without motion sensors "
                   "captures by itself."
                 : "Off: every capture completes by itself, boss captures too, with any controller.");

        sectionHeading("Starting the game");
        changed |= enumChoice("Opening logos", &settings.logoSkip,
                              {"Play them (as on the PS3)", "Start skips them", "Never show them"});
        if (engineSettingsLanguageSelectable())
        {
            const std::vector<GameLanguage> languages = {GameLanguage::English, GameLanguage::French,
                                                         GameLanguage::Spanish, GameLanguage::German,
                                                         GameLanguage::Italian};
            int languageIndex = 0;
            std::vector<const char*> names;
            for (int index = 0; index < (int)languages.size(); index++)
            {
                names.push_back(engineSettingsLanguageName(languages[(size_t)index]));
                if (languages[(size_t)index] == settings.language)
                {
                    languageIndex = index;
                }
            }
            if (choice("Language", &languageIndex, names))
            {
                settings.language = languages[(size_t)languageIndex];
                changed = true;
            }
        }

        sectionHeading("Physics");
        changed |= ImGui::Checkbox("Step physics by the frame time (fixes cloth and hair above 30 fps)",
                                   &settings.physicsFrameStep);
        return changed;
    }

    /* ---- Play ----------------------------------------------------------- */

    bool readyToPlay(std::string* reason)
    {
        if (!gameCheck.good)
        {
            *reason = "Choose your disc dump on the Game page first.";
            return false;
        }
        if (FOLKLORE_USES_UPDATE && !updateCheck.good)
        {
            *reason = "Choose the installed 1.10 update on the Game page first.";
            return false;
        }
        const std::string game = engineExecutableDirectory() + "\\folklore.exe";
        if (!fileExists(game))
        {
            *reason = "folklore.exe is not beside the launcher (" + game + ").";
            return false;
        }
        return true;
    }

    bool startGame()
    {
        engineSettingsSave();
        settingsDirty = false;
        const std::string folder = engineExecutableDirectory();
        std::string commandLine = "\"" + folder + "\\folklore.exe\"";
        /* The game (and the run it starts) inherits this. */
        if (dumpTexturesThisRun)
        {
            const std::string dumpFolder = textureDumpFolder();
            CreateDirectoryA((engineDataDirectory() + "\\textures").c_str(), nullptr);
            CreateDirectoryA(dumpFolder.c_str(), nullptr);
            SetEnvironmentVariableA("RSX_TEXTURE_DUMP", dumpFolder.c_str());
        }
        else
        {
            SetEnvironmentVariableA("RSX_TEXTURE_DUMP", nullptr);
        }
        STARTUPINFOA startup = {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process = {};
        std::vector<char> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back(0);
        /* The launcher is the window in front; the game (started by
         * folklore.exe in turn) may bring its own window forward. */
        AllowSetForegroundWindow(ASFW_ANY);
        if (!CreateProcessA(nullptr, commandBuffer.data(), nullptr, nullptr, FALSE, 0, nullptr, folder.c_str(),
                            &startup, &process))
        {
            launchProblem = "Could not start folklore.exe (error " + std::to_string(GetLastError()) + ").";
            return false;
        }
        CloseHandle(process.hThread);
        gameProcess = process.hProcess;
        return true;
    }

    /* After Play: stay open until the game's window is up and give it the
     * foreground. Windows only lets the window in front pass the foreground
     * on; a launcher that closed straight away left the game's window behind
     * other windows, without keyboard input (the keys only reach the game
     * while its window is in front). Gives up after a minute, or when the
     * game has already closed. */
    void handForegroundToGame(HWND launcherWindow)
    {
        SetWindowTextW(launcherWindow, L"Folklore Launcher - starting the game...");
        const ULONGLONG giveUpMs = GetTickCount64() + 60000ull;
        while (GetTickCount64() < giveUpMs)
        {
            MSG message;
            while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
            {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (WaitForSingleObject(gameProcess, 0) == WAIT_OBJECT_0)
            {
                return;
            }
            const HWND gameWindow = FindWindowA("ps3recomp_null", nullptr);
            if (gameWindow && IsWindowVisible(gameWindow))
            {
                SetForegroundWindow(gameWindow);
                BringWindowToTop(gameWindow);
                return;
            }
            Sleep(50);
        }
    }

    /* ---- The frame -------------------------------------------------------- */

    int currentPage = 0;

    /* Returns true when the launcher should close (the game started). */
    bool drawFrame(float scale)
    {
        const ImGuiIO& io = ImGui::GetIO();
        const float width = io.DisplaySize.x;
        const float height = io.DisplaySize.y;
        ImDrawList* background = ImGui::GetBackgroundDrawList();
        background->AddRectFilled(ImVec2(0.0f, 0.0f), ImVec2(width, height), ImGui::GetColorU32(kBackground));

        /* The art fills the left of the window at full height, fading into
         * the background on its right edge. */
        float artPanel = std::min(height * 2.0f / 3.0f, width * 0.45f);
        if (artView && artHeight > 0)
        {
            const float artAspect = (float)artWidth / (float)artHeight;
            const float panelAspect = artPanel / height;
            /* Crop to the panel's shape, keeping the middle. */
            float uvLeft = 0.0f;
            float uvRight = 1.0f;
            float uvTop = 0.0f;
            float uvBottom = 1.0f;
            if (artAspect > panelAspect)
            {
                const float visible = panelAspect / artAspect;
                uvLeft = (1.0f - visible) * 0.5f;
                uvRight = uvLeft + visible;
            }
            else
            {
                const float visible = artAspect / panelAspect;
                uvTop = (1.0f - visible) * 0.5f;
                uvBottom = uvTop + visible;
            }
            background->AddImage((ImTextureID)(intptr_t)artView, ImVec2(0.0f, 0.0f), ImVec2(artPanel, height),
                                 ImVec2(uvLeft, uvTop), ImVec2(uvRight, uvBottom));
        }
        else
        {
            const ImU32 top = ImGui::GetColorU32(ImVec4(0.42f, 0.16f, 0.36f, 1.0f));
            const ImU32 bottom = ImGui::GetColorU32(kBackground);
            background->AddRectFilledMultiColor(ImVec2(0.0f, 0.0f), ImVec2(artPanel, height), top, top, bottom, bottom);
            ImGui::PushFont(titleFont);
            const ImVec2 titleSize = ImGui::CalcTextSize("Folklore");
            background->AddText(titleFont, titleFont->FontSize, ImVec2((artPanel - titleSize.x) * 0.5f, height * 0.12f),
                                ImGui::GetColorU32(ImVec4(0.93f, 0.80f, 0.93f, 1.0f)), "Folklore");
            ImGui::PopFont();
        }
        const float fade = 140.0f * scale;
        const ImU32 clear = ImGui::GetColorU32(ImVec4(kBackground.x, kBackground.y, kBackground.z, 0.0f));
        const ImU32 solid = ImGui::GetColorU32(kBackground);
        background->AddRectFilledMultiColor(ImVec2(artPanel - fade, 0.0f), ImVec2(artPanel + 1.0f, height), clear, solid,
                                            solid, clear);

        /* The settings, to the right of the art. */
        const float panelLeft = artPanel - fade * 0.35f;
        ImGui::SetNextWindowPos(ImVec2(panelLeft, 0.0f));
        ImGui::SetNextWindowSize(ImVec2(width - panelLeft, height));
        const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                       ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus;
        bool closeLauncher = false;
        ImGui::Begin("##launcher", nullptr, flags);
        {
            ImGui::PushFont(headingFont);
            ImGui::TextColored(kText, "Folklore for PC");
            ImGui::PopFont();
            ImGui::SameLine();
            ImGui::TextColored(kTextDim, "  %s release, v%s build", FOLKLORE_RELEASE_NAME, FOLKLORE_VERSION_NAME);
            ImGui::Spacing();

            const char* pages[6] = {"Game", "Saves", "DLC", "Graphics", "Display", "Gameplay"};
            for (int pageIndex = 0; pageIndex < 6; pageIndex++)
            {
                if (pageIndex > 0)
                {
                    ImGui::SameLine();
                }
                const bool selected = pageIndex == currentPage;
                ImGui::PushStyleColor(ImGuiCol_Button, selected ? kAccent : ImVec4(0.17f, 0.10f, 0.21f, 1.0f));
                ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
                if (ImGui::Button(pages[pageIndex], ImVec2(106.0f * scale, 0.0f)))
                {
                    currentPage = pageIndex;
                }
                ImGui::PopStyleColor(2);
            }
            ImGui::Spacing();

            const float footer = 92.0f * scale;
            ImGui::BeginChild("##page", ImVec2(0.0f, -footer), ImGuiChildFlags_AlwaysUseWindowPadding);
            bool changed = false;
            if (currentPage == 0)
            {
                changed = drawGamePage();
            }
            else if (currentPage == 1)
            {
                changed = drawSavesPage();
            }
            else if (currentPage == 2)
            {
                drawDlcPage();
            }
            else if (currentPage == 3)
            {
                changed = drawGraphicsPage();
            }
            else if (currentPage == 4)
            {
                changed = drawDisplayPage();
            }
            else
            {
                changed = drawGameplayPage();
            }
            ImGui::EndChild();
            if (changed)
            {
                settingsDirty = true;
                checkFolders();
            }

            /* Footer: why the game cannot start yet, or Play. */
            ImGui::Spacing();
            std::string reason;
            const bool ready = readyToPlay(&reason);
            bool dlcRunning = false;
            {
                std::lock_guard<std::mutex> guard(dlcWork.lock);
                dlcRunning = dlcWork.running;
            }
            const float playWidth = 220.0f * scale;
            const float playHeight = 56.0f * scale;
            const float donateWidth = 190.0f * scale;
            const float buttonGap = ImGui::GetStyle().ItemSpacing.x * 1.5f;
            const float buttonsLeft = ImGui::GetWindowWidth() - playWidth - donateWidth - buttonGap -
                                      ImGui::GetStyle().WindowPadding.x;
            /* The message wraps before Ko-fi rather than running under it. */
            ImGui::PushTextWrapPos(buttonsLeft - buttonGap);
            ImGui::BeginGroup();
            if (!launchProblem.empty())
            {
                ImGui::TextColored(kBad, "%s", launchProblem.c_str());
            }
            else if (!ready)
            {
                ImGui::TextColored(kWarning, "%s", reason.c_str());
            }
            else if (dlcRunning)
            {
                ImGui::TextColored(kTextDim, "Adding DLC...");
            }
            else if (dumpTexturesThisRun)
            {
                ImGui::TextColored(kWarning, "Texture dump on for this run.");
            }
            else
            {
                ImGui::TextColored(kTextDim, "Settings are saved when you press Play.");
            }
            {
                /* Just the file's name: the full path would run under Play. */
                const std::string settingsPath = engineSettingsPath();
                const size_t separator = settingsPath.find_last_of("\\/");
                ImGui::TextColored(kTextDim, "Settings file: %s",
                                   separator == std::string::npos ? settingsPath.c_str()
                                                                  : settingsPath.c_str() + separator + 1);
            }
            ImGui::EndGroup();
            ImGui::PopTextWrapPos();

            /* Ko-fi, then Play, at the right. */
            ImGui::SameLine(buttonsLeft);
            const ImVec4 kofiBlue(0.161f, 0.671f, 0.878f, 1.0f);
            const ImVec4 kofiBlueHover(0.302f, 0.749f, 0.929f, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Button, kofiBlue);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kofiBlueHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kofiBlueHover);
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
            if (ImGui::Button("Support on Ko-fi", ImVec2(donateWidth, playHeight)))
            {
                ShellExecuteA(nullptr, "open", kDonateUrl, nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::PopStyleColor(4);
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Donate to this project: opens ko-fi.com/mavab in your browser");
            }
            ImGui::SameLine(0.0f, buttonGap);
            ImGui::BeginDisabled(!ready || dlcRunning);
            ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentHover);
            ImGui::PushFont(headingFont);
            if (ImGui::Button("Play", ImVec2(playWidth, playHeight)))
            {
                closeLauncher = startGame();
            }
            ImGui::PopFont();
            ImGui::PopStyleColor(3);
            ImGui::EndDisabled();
        }
        ImGui::End();
        return closeLauncher;
    }

    LRESULT CALLBACK windowProcedure(HWND window, UINT message, WPARAM wideParam, LPARAM longParam)
    {
        if (ImGui_ImplWin32_WndProcHandler(window, message, wideParam, longParam))
        {
            return TRUE;
        }
        switch (message)
        {
        case WM_SIZE:
            if (wideParam != SIZE_MINIMIZED)
            {
                pendingWidth = (UINT)LOWORD(longParam);
                pendingHeight = (UINT)HIWORD(longParam);
            }
            return 0;
        case WM_GETMINMAXINFO:
        {
            MINMAXINFO* limits = (MINMAXINFO*)longParam;
            limits->ptMinTrackSize.x = 900;
            limits->ptMinTrackSize.y = 600;
            return 0;
        }
        case WM_SYSCOMMAND:
            if ((wideParam & 0xFFF0) == SC_KEYMENU)
            {
                return 0;
            }
            break;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        default:
            break;
        }
        return DefWindowProcW(window, message, wideParam, longParam);
    }
}

int WINAPI WinMain(HINSTANCE instance, HINSTANCE previousInstance, LPSTR commandLine, int showCommand)
{
    (void)previousInstance;
    (void)commandLine;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    engineSettingsLoad();
    setDataEnvironment();
    /* Packs copied in by hand: decrypt what the licences found allow, and
     * say which still need one (as folklore.exe does at its start). */
    engineDlcRepairInstalled();
    checkFolders();

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_CLASSDC;
    windowClass.lpfnWndProc = windowProcedure;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(1));
    windowClass.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512) /* IDC_ARROW */);
    windowClass.lpszClassName = L"FolkloreLauncher";
    RegisterClassExW(&windowClass);

    const UINT systemDpi = GetDpiForSystem();
    const float scale = systemDpi > 0 ? (float)systemDpi / 96.0f : 1.0f;
    const int windowWidth = (int)(1280.0f * scale);
    const int windowHeight = (int)(800.0f * scale);
    RECT workArea = {};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &workArea, 0);
    const int left = workArea.left + std::max(0, (int)(workArea.right - workArea.left - windowWidth) / 2);
    const int top = workArea.top + std::max(0, (int)(workArea.bottom - workArea.top - windowHeight) / 2);
    HWND window = CreateWindowW(windowClass.lpszClassName, L"Folklore", WS_OVERLAPPEDWINDOW, left, top, windowWidth,
                                windowHeight, nullptr, nullptr, instance, nullptr);
    const BOOL darkTitleBar = TRUE;
    DwmSetWindowAttribute(window, 20 /* DWMWA_USE_IMMERSIVE_DARK_MODE */, &darkTitleBar, sizeof(darkTitleBar));

    if (!createDevice(window))
    {
        MessageBoxA(nullptr, "Could not start Direct3D 11 for the launcher.", "Folklore", MB_OK | MB_ICONERROR);
        destroyDevice();
        return 1;
    }
    ShowWindow(window, showCommand == SW_HIDE ? SW_SHOWDEFAULT : showCommand);
    UpdateWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    loadFonts(scale);
    applyStyle(scale);
    ImGui_ImplWin32_Init(window);
    ImGui_ImplDX11_Init(device, deviceContext);
    loadArt();

    bool running = true;
    while (running)
    {
        MSG message;
        while (PeekMessageW(&message, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            if (message.message == WM_QUIT)
            {
                running = false;
            }
        }
        if (!running)
        {
            break;
        }
        if (IsIconic(window))
        {
            Sleep(16);
            continue;
        }
        if (pendingWidth != 0 && pendingHeight != 0)
        {
            releaseRenderTarget();
            swapChain->ResizeBuffers(0, pendingWidth, pendingHeight, DXGI_FORMAT_UNKNOWN, 0);
            pendingWidth = 0;
            pendingHeight = 0;
            createRenderTarget();
        }
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        ImGui::PushFont(bodyFont);
        const bool gameStarted = drawFrame(scale);
        ImGui::PopFont();
        ImGui::Render();
        const float clearColour[4] = {kBackground.x, kBackground.y, kBackground.z, 1.0f};
        deviceContext->OMSetRenderTargets(1, &renderTarget, nullptr);
        deviceContext->ClearRenderTargetView(renderTarget, clearColour);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        swapChain->Present(1, 0);
        if (gameStarted)
        {
            running = false;
        }
    }

    if (settingsDirty)
    {
        engineSettingsSave();
    }
    if (gameProcess)
    {
        handForegroundToGame(window);
        CloseHandle(gameProcess);
        gameProcess = nullptr;
    }
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (artView)
    {
        artView->Release();
    }
    destroyDevice();
    DestroyWindow(window);
    UnregisterClassW(windowClass.lpszClassName, instance);
    return 0;
}
