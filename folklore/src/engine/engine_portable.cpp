/*
 * engine_portable.cpp -- start-up for folklore.exe on its own.
 *
 * run.ps1 used to be the only way to start the game: it passed the EBOOT path
 * and set a dozen environment variables the runtime reads. A copy of the
 * built game (package.ps1 -Kind Binaries) is started by double-clicking
 * folklore.exe instead, so the boot harness calls ps3_host_bootstrap
 * (patches/0046) before anything else, and it
 *
 *   - fills in the environment run.ps1 sets, wherever it is not already set
 *     (so run.ps1, and anyone's own overrides, still win);
 *   - keeps hdd0 and hdd1 (the game's installed data), gamedata (saves), the
 *     shader cache and logs in one data folder: beside the exe, or the
 *     data_dir setting;
 *   - finds the game: the EBOOT given on the command line, else the game_dir
 *     setting, else a folder picker, and checks the folder before using it;
 *   - writes the log to logs\run-<date-time>.log when nothing else captures it.
 */
#include "engine_portable.h"
#include "engine_settings.h"

#ifdef _WIN32

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shobjidl.h>

#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "user32.lib")

namespace
{
    /* What this build was lifted from (tools/relift.sh): the North American
     * disc, v1.00. Its decrypted EBOOT.elf is this size; the lifted code
     * matches no other version. */
    constexpr const char* kTitleId = "BCUS98147";
    constexpr unsigned long long kEbootSize = 6518936ull;

    bool fileExists(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool directoryExists(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    void makeDirectory(const std::string& path)
    {
        if (!directoryExists(path))
        {
            CreateDirectoryA(path.c_str(), nullptr);
        }
    }

    bool environmentSet(const char* name)
    {
        const char* value = std::getenv(name);
        return value && *value;
    }

    /* Set name=value unless the environment already has a value for it. */
    void defaultEnvironment(const char* name, const std::string& value)
    {
        if (environmentSet(name))
        {
            return;
        }
        _putenv_s(name, value.c_str());
        SetEnvironmentVariableA(name, value.c_str());
    }

    std::vector<unsigned char> readFile(const std::string& path, size_t limit)
    {
        std::vector<unsigned char> bytes;
        FILE* file = std::fopen(path.c_str(), "rb");
        if (!file)
        {
            return bytes;
        }
        bytes.resize(limit);
        const size_t got = std::fread(bytes.data(), 1, limit, file);
        bytes.resize(got);
        std::fclose(file);
        return bytes;
    }

    unsigned long long fileSize(const std::string& path)
    {
        WIN32_FILE_ATTRIBUTE_DATA data;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &data))
        {
            return 0;
        }
        return ((unsigned long long)data.nFileSizeHigh << 32) | data.nFileSizeLow;
    }

    bool containsText(const std::vector<unsigned char>& bytes, const char* text)
    {
        const size_t textLength = std::strlen(text);
        if (bytes.size() < textLength)
        {
            return false;
        }
        for (size_t position = 0; position + textLength <= bytes.size(); position++)
        {
            if (std::memcmp(bytes.data() + position, text, textLength) == 0)
            {
                return true;
            }
        }
        return false;
    }

    bool endsWithFolder(const std::string& path, const char* folderName)
    {
        const size_t separator = path.find_last_of("\\/");
        const std::string last = separator == std::string::npos ? path : path.substr(separator + 1);
        return _stricmp(last.c_str(), folderName) == 0;
    }

    std::string parentFolder(const std::string& path)
    {
        const size_t separator = path.find_last_of("\\/");
        return separator == std::string::npos ? path : path.substr(0, separator);
    }

    std::string wideToNarrow(const wchar_t* text)
    {
        const int length = WideCharToMultiByte(CP_ACP, 0, text, -1, nullptr, 0, nullptr, nullptr);
        if (length <= 1)
        {
            return std::string();
        }
        std::string narrow((size_t)length - 1, '\0');
        WideCharToMultiByte(CP_ACP, 0, text, -1, &narrow[0], length, nullptr, nullptr);
        return narrow;
    }

    std::wstring narrowToWide(const std::string& text)
    {
        const int length = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
        if (length <= 1)
        {
            return std::wstring();
        }
        std::wstring wide((size_t)length - 1, L'\0');
        MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, &wide[0], length);
        return wide;
    }

    void showError(const std::string& text)
    {
        MessageBoxA(nullptr, text.c_str(), "Folklore", MB_OK | MB_ICONWARNING | MB_SETFOREGROUND);
    }

    /* Nothing (run.ps1, a console) is capturing the output: write it to a
     * log file, and close the console window Windows opened for the exe. */
    void redirectLogIfStandalone(const std::string& dataDirectory)
    {
        if (environmentSet("FOLKLORE_LAUNCHER"))
        {
            return;     /* run.ps1 tees the output into its own log */
        }
        const std::string logDirectory = dataDirectory + "\\logs";
        makeDirectory(logDirectory);
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm localTime;
        localtime_s(&localTime, &now);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &localTime);
        const std::string logPath = logDirectory + "\\run-" + stamp + ".log";
        /* One handle on the file, put under file descriptors 1 and 2 and the
         * Win32 standard handles. (The earlier freopen of stdout and stderr
         * left the log empty in a packaged run, 2026-10-26.) */
        HANDLE logFile = CreateFileA(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile == INVALID_HANDLE_VALUE)
        {
            return;
        }
        const int logDescriptor = _open_osfhandle(reinterpret_cast<intptr_t>(logFile), _O_APPEND | _O_BINARY);
        if (logDescriptor < 0)
        {
            CloseHandle(logFile);
            return;
        }
        std::fflush(stdout);
        std::fflush(stderr);
        if (_dup2(logDescriptor, 1) != 0 || _dup2(logDescriptor, 2) != 0)
        {
            /* No console at all (stdout/stderr were never opened): attach the
             * streams to the file instead. */
            std::freopen("NUL", "w", stdout);
            std::freopen("NUL", "w", stderr);
            _dup2(logDescriptor, _fileno(stdout));
            _dup2(logDescriptor, _fileno(stderr));
        }
        SetStdHandle(STD_OUTPUT_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(1)));
        SetStdHandle(STD_ERROR_HANDLE, reinterpret_cast<HANDLE>(_get_osfhandle(2)));
        std::setvbuf(stdout, nullptr, _IONBF, 0);
        std::setvbuf(stderr, nullptr, _IONBF, 0);
        /* Only this process on the console: it was opened for us by a
         * double-click, and would sit there empty. A console someone started
         * the exe from is left alone. */
        DWORD processes[4];
        if (GetConsoleProcessList(processes, 4) == 1)
        {
            FreeConsole();
        }
        std::fprintf(stderr, "[engine] log: %s\n", logPath.c_str());
    }

    void applyEnvironmentDefaults(const std::string& baseDirectory, const std::string& dataDirectory)
    {
        const EngineSettings& settings = engineSettings();
        /* The same values run.ps1 sets (see the comments there). */
        defaultEnvironment("PS3_TITLE", "Folklore");
        defaultEnvironment("RSX_LIVE_DRAW", "1");
        defaultEnvironment("PS3_SPURS_JOBCTX_SBUF70", "1");
        defaultEnvironment("SPU_INTERP_UNLIFTED", "1");
        defaultEnvironment("GCM_SEMA_ACQUIRE", "1");
        defaultEnvironment("GCM_WAITFLIP_DEPTH", "1");
        defaultEnvironment("PS3_VERBOSE", "0");

        const std::string hdd0 = dataDirectory + "\\hdd0";
        const std::string hdd1 = dataDirectory + "\\hdd1";
        const std::string spuMiss = dataDirectory + "\\logs\\spu_miss";
        if (!environmentSet("PS3_HDD0_ROOT"))
        {
            makeDirectory(hdd0);
            defaultEnvironment("PS3_HDD0_ROOT", hdd0);
        }
        if (!environmentSet("PS3_HDD1_ROOT"))
        {
            makeDirectory(hdd1);
            defaultEnvironment("PS3_HDD1_ROOT", hdd1);
        }
        if (!environmentSet("SPU_DUMP_MISS"))
        {
            makeDirectory(dataDirectory + "\\logs");
            makeDirectory(spuMiss);
            defaultEnvironment("SPU_DUMP_MISS", spuMiss);
        }

        /* Firmware fonts: the dev_flash setting, else a dev_flash folder
         * beside the data or the exe. Optional. */
        if (!environmentSet("PS3_DEV_FLASH"))
        {
            const std::string candidates[3] = {
                settings.devFlashDir, dataDirectory + "\\dev_flash", baseDirectory + "\\dev_flash"};
            for (const std::string& candidate : candidates)
            {
                if (!candidate.empty() && directoryExists(candidate))
                {
                    defaultEnvironment("PS3_DEV_FLASH", candidate);
                    break;
                }
            }
        }

        /* Sony's SPURS library, lifted from the builder's own firmware: the
         * image beside the exe when there is one, else the path baked in at
         * build time (src/prx/libsre_load.cpp). */
        const std::string libsreImage = baseDirectory + "\\libsre_image.bin";
        if (fileExists(libsreImage))
        {
            defaultEnvironment("PS3_LIBSRE_IMAGE", libsreImage);
        }
    }
}

std::string engineNormaliseGameFolder(const std::string& folder)
{
    std::string normalised = folder;
    while (normalised.size() > 3 && (normalised.back() == '\\' || normalised.back() == '/'))
    {
        normalised.pop_back();
    }
    if (endsWithFolder(normalised, "USRDIR"))
    {
        normalised = parentFolder(normalised);
    }
    if (endsWithFolder(normalised, "PS3_GAME"))
    {
        normalised = parentFolder(normalised);
    }
    return normalised;
}

bool engineCheckGameFolder(const std::string& folder, std::string* problem)
{
    std::string reason;
    const std::string usrdir = folder + "\\PS3_GAME\\USRDIR";
    const std::string eboot = usrdir + "\\EBOOT.elf";
    if (folder.empty())
    {
        reason = "No game folder is set.";
    }
    else if (!directoryExists(folder + "\\PS3_GAME"))
    {
        reason = "\"" + folder + "\" has no PS3_GAME folder. Choose the folder of your Folklore disc dump "
                 "(the one that contains PS3_GAME).";
    }
    else if (!fileExists(eboot))
    {
        reason = fileExists(usrdir + "\\EBOOT.BIN")
                     ? "This dump's EBOOT.BIN is still encrypted. Decrypt it with RPCS3 (Utilities > Decrypt PS3 "
                       "Binaries, choose PS3_GAME\\USRDIR\\EBOOT.BIN), which writes EBOOT.elf beside it, then try again."
                     : "There is no PS3_GAME\\USRDIR\\EBOOT.elf in \"" + folder + "\".";
    }
    else
    {
        const std::vector<unsigned char> header = readFile(eboot, 4);
        const std::vector<unsigned char> sfo = readFile(folder + "\\PS3_GAME\\PARAM.SFO", 64 * 1024);
        if (header.size() < 4 || header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F')
        {
            reason = "PS3_GAME\\USRDIR\\EBOOT.elf is not a decrypted ELF. Decrypt EBOOT.BIN with RPCS3 "
                     "(Utilities > Decrypt PS3 Binaries) and try again.";
        }
        else if (!sfo.empty() && !containsText(sfo, kTitleId))
        {
            reason = std::string("This is not the North American release of Folklore (") + kTitleId +
                     "), the version this build was made from. Other versions will not run.";
        }
        else if (fileSize(eboot) != kEbootSize)
        {
            reason = "This EBOOT.elf is not the v1.00 disc version this build was made from (the decrypted file "
                     "should be 6,518,936 bytes).";
        }
    }
    if (problem)
    {
        *problem = reason;
    }
    return reason.empty();
}

std::string enginePickGameFolder(const std::string& startFolder)
{
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string chosen;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"Folklore: choose your game folder (the one that contains PS3_GAME)");
        if (!startFolder.empty() && directoryExists(startFolder))
        {
            IShellItem* startItem = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(narrowToWide(startFolder).c_str(), nullptr,
                                                      IID_PPV_ARGS(&startItem))))
            {
                dialog->SetFolder(startItem);
                startItem->Release();
            }
        }
        if (SUCCEEDED(dialog->Show(nullptr)))
        {
            IShellItem* result = nullptr;
            if (SUCCEEDED(dialog->GetResult(&result)))
            {
                PWSTR widePath = nullptr;
                if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &widePath)))
                {
                    chosen = engineNormaliseGameFolder(wideToNarrow(widePath));
                    CoTaskMemFree(widePath);
                }
                result->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(initialised))
    {
        CoUninitialize();
    }
    return chosen;
}

std::string engineDataDirectory()
{
    const EngineSettings& settings = engineSettings();
    if (!settings.dataDir.empty())
    {
        return settings.dataDir;
    }
    return engineExecutableDirectory();
}

/* Called by the boot harness (patches/0046) before it loads the ELF. Returns
 * the EBOOT to run, or nullptr to quit (the user closed the picker). */
extern "C" const char* ps3_host_bootstrap(const char* elfFromCommandLine)
{
    static std::string ebootPath;
    EngineSettings& settings = engineSettings();
    const std::string baseDirectory = engineExecutableDirectory();
    const std::string dataDirectory = engineDataDirectory();
    makeDirectory(dataDirectory);
    redirectLogIfStandalone(dataDirectory);
    applyEnvironmentDefaults(baseDirectory, dataDirectory);

    if (elfFromCommandLine && *elfFromCommandLine)
    {
        char fullPath[MAX_PATH * 2];
        const DWORD length = GetFullPathNameA(elfFromCommandLine, (DWORD)sizeof(fullPath), fullPath, nullptr);
        ebootPath = (length > 0 && length < sizeof(fullPath)) ? std::string(fullPath, length) : elfFromCommandLine;
    }
    /* Started on its own, the working directory is wherever the shortcut or
     * Explorer says. The runtime keeps saves (gamedata\), the shader cache
     * (scratch\) and the engine menu's save backups relative to it, so make
     * it the data folder. run.ps1 chooses its own. */
    if (!environmentSet("FOLKLORE_LAUNCHER"))
    {
        SetCurrentDirectoryA(dataDirectory.c_str());
    }
    if (!ebootPath.empty())
    {
        return ebootPath.c_str();     /* run.ps1, or a shortcut with the EBOOT path */
    }

    std::string gameFolder = engineNormaliseGameFolder(settings.gameDir);
    std::string problem;
    bool firstAttempt = true;
    while (!engineCheckGameFolder(gameFolder, &problem))
    {
        if (!firstAttempt || !settings.gameDir.empty())
        {
            showError(problem);
        }
        else
        {
            MessageBoxA(nullptr,
                        "Choose the folder of your Folklore (BCUS98147) disc dump: the folder that contains "
                        "PS3_GAME. Its PS3_GAME\\USRDIR\\EBOOT.BIN must be decrypted to EBOOT.elf with RPCS3 "
                        "first.\n\nThe game remembers the folder; change it later in the engine menu (F1).",
                        "Folklore", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        }
        firstAttempt = false;
        const std::string picked = enginePickGameFolder(gameFolder.empty() ? baseDirectory : gameFolder);
        if (picked.empty())
        {
            std::fprintf(stderr, "[engine] no game folder chosen -- quitting\n");
            return nullptr;
        }
        gameFolder = picked;
    }
    if (gameFolder != settings.gameDir)
    {
        settings.gameDir = gameFolder;
        engineSettingsSave();
    }
    defaultEnvironment("PS3_VFS_ROOT", gameFolder);
    ebootPath = gameFolder + "\\PS3_GAME\\USRDIR\\EBOOT.elf";
    std::fprintf(stderr, "[engine] game folder: %s\n", gameFolder.c_str());
    return ebootPath.c_str();
}

#endif /* _WIN32 */
