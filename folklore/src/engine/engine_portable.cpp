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
 *
 * Started on its own (double-clicked: no FOLKLORE_LAUNCHER), the exe does
 * what run.ps1 does instead of running the game in its own process: it finds
 * the game, sets run.ps1's whole environment, and starts itself again as a
 * child with that environment, the data folder as its working folder and its
 * output going to the log file -- then waits for it. Run in the first
 * process, the game drew the title screen wrongly and stopped at a black
 * screen (2026-10-26), while the same exe under run.ps1 played normally.
 */
#include "engine_portable.h"
#include "engine_settings.h"
#include "engine_region.h"
#include "engine_dlc.h"

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
    /* What this build was lifted from (tools/relift.sh, src/recomp/
     * folklore_region.h): the North American (BCUS98147) or European
     * (BCES00050) disc, v1.00. Its decrypted EBOOT.elf is this size; the
     * lifted code matches no other version. */
    constexpr const char* kTitleId = FOLKLORE_TITLE_ID;
    constexpr unsigned long long kEbootSize = FOLKLORE_EBOOT_SIZE;

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

namespace
{
    std::string timeStamp()
    {
        char stamp[32];
        const std::time_t now = std::time(nullptr);
        std::tm localTime;
        localtime_s(&localTime, &now);
        std::strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", &localTime);
        return stamp;
    }

    /* The rest of what run.ps1 sets: diagnostics it always has on, and the
     * render capture (F2 / capture.bat) armed into logs\capture-<stamp>. */
    void applyLauncherEnvironment(const std::string& dataDirectory, const std::string& gameFolder,
                                  const std::string& stamp)
    {
        defaultEnvironment("FOLKLORE_SPURS", "native");
        defaultEnvironment("PS3_FS_AIO_LOG", "1");
        defaultEnvironment("PS3_WATCHDOG_DUMP", "0x10A60C00:0x1000");
        if (!gameFolder.empty())
        {
            defaultEnvironment("PS3_VFS_ROOT", gameFolder);
        }
        const std::string logDirectory = dataDirectory + "\\logs";
        const std::string captureDirectory = logDirectory + "\\capture-" + stamp;
        const std::string captureGate = logDirectory + "\\capture.go";
        makeDirectory(logDirectory);
        makeDirectory(captureDirectory);
        makeDirectory(captureDirectory + "\\hlsl");
        DeleteFileA(captureGate.c_str());
        defaultEnvironment("RSX_DRAW_CSV", captureDirectory + "\\draws.csv");
        defaultEnvironment("RSX_DRAW_CSV_GATE", captureGate);
        defaultEnvironment("LD_DRAW_DUMP", captureDirectory);
        defaultEnvironment("LD_DRAW_DUMP_EVERY", "100000");
        defaultEnvironment("LD_VB_DUMP", captureDirectory);
        defaultEnvironment("LD_HLSL_DUMP", captureDirectory + "\\hlsl");
    }

    /* Start this exe again on `eboot`, the way run.ps1 starts it, and wait.
     * Returns false when it could not be started. */
    bool runAsLauncher(const std::string& eboot, const std::string& dataDirectory, const std::string& logPath,
                       DWORD* exitCode)
    {
        char exePath[MAX_PATH * 2];
        const DWORD exeLength = GetModuleFileNameA(nullptr, exePath, (DWORD)sizeof(exePath));
        if (exeLength == 0 || exeLength >= sizeof(exePath))
        {
            return false;
        }
        SECURITY_ATTRIBUTES inheritable = {sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        HANDLE logFile = CreateFileA(logPath.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                     &inheritable, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (logFile == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        HANDLE nothing = CreateFileA("NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inheritable,
                                     OPEN_EXISTING, 0, nullptr);

        /* The child is the game; this marks it as started by a launcher, so
         * it neither starts another nor redirects its own output. */
        SetEnvironmentVariableA("FOLKLORE_LAUNCHER", "folklore.exe");

        std::string commandLine = std::string("\"") + exePath + "\" \"" + eboot + "\"";
        std::vector<char> commandBuffer(commandLine.begin(), commandLine.end());
        commandBuffer.push_back('\0');
        STARTUPINFOA startup;
        std::memset(&startup, 0, sizeof(startup));
        startup.cb = sizeof(startup);
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nothing;
        startup.hStdOutput = logFile;
        startup.hStdError = logFile;
        PROCESS_INFORMATION process;
        std::memset(&process, 0, sizeof(process));
        const BOOL started = CreateProcessA(exePath, commandBuffer.data(), nullptr, nullptr, TRUE,
                                            CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, dataDirectory.c_str(),
                                            &startup, &process);
        const DWORD startError = GetLastError();
        CloseHandle(logFile);
        if (nothing != INVALID_HANDLE_VALUE)
        {
            CloseHandle(nothing);
        }
        SetEnvironmentVariableA("FOLKLORE_LAUNCHER", nullptr);
        if (!started)
        {
            SetLastError(startError);
            return false;
        }
        /* The game goes when this launcher goes (ended from Task Manager,
         * say): the job is closed with this process and takes the child. */
        HANDLE job = CreateJobObjectA(nullptr, nullptr);
        if (job)
        {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits;
            std::memset(&limits, 0, sizeof(limits));
            limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            SetInformationJobObject(job, JobObjectExtendedLimitInformation, &limits, sizeof(limits));
            AssignProcessToJobObject(job, process.hProcess);
        }
        ResumeThread(process.hThread);
        CloseHandle(process.hThread);
        WaitForSingleObject(process.hProcess, INFINITE);
        GetExitCodeProcess(process.hProcess, exitCode);
        CloseHandle(process.hProcess);
        return true;
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
    else if (FOLKLORE_USES_UPDATE)
    {
        /* A 1.10 build runs the update's EBOOT; only the disc's data is read
         * from here, so its own EBOOT need not even be decrypted. */
        const std::vector<unsigned char> sfo = readFile(folder + "\\PS3_GAME\\PARAM.SFO", 64 * 1024);
        if (!sfo.empty() && !containsText(sfo, kTitleId))
        {
            reason = std::string("This build of the port was made from the ") + FOLKLORE_RELEASE_NAME +
                     " release of Folklore (" + kTitleId + "), and this dump is a different release. Use the "
                     "disc dump of the same release as your 1.10 update.";
        }
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
            const bool otherSupported = containsText(sfo, FOLKLORE_US_TITLE_ID) || containsText(sfo, FOLKLORE_EU_TITLE_ID);
            reason = std::string("This build of the port was made from the ") + FOLKLORE_RELEASE_NAME +
                     " release of Folklore (" + kTitleId + ")" +
                     (otherSupported ? ", and this dump is the other release. Use the build made from this "
                                       "release, or build one from this dump (setup.ps1)."
                                     : ". Other versions will not run.");
        }
        else if (fileSize(eboot) != kEbootSize)
        {
            reason = std::string("This EBOOT.elf is not the v1.00 disc version this build was made from (the "
                                 "decrypted ") + kTitleId + " EBOOT.elf is " + std::to_string(kEbootSize) +
                     " bytes).";
        }
    }
    if (problem)
    {
        *problem = reason;
    }
    return reason.empty();
}

std::string engineNormaliseUpdateFolder(const std::string& folder)
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
    return normalised;
}

bool engineCheckUpdateFolder(const std::string& folder, std::string* problem)
{
    std::string reason;
    const std::string eboot = folder + "\\USRDIR\\EBOOT.elf";
    const std::vector<unsigned char> sfo = readFile(folder + "\\PARAM.SFO", 64 * 1024);
    if (folder.empty())
    {
        reason = "No 1.10 update folder is set.";
    }
    else if (sfo.empty() || !directoryExists(folder + "\\USRDIR"))
    {
        reason = "\"" + folder + "\" is not an installed game update (it needs PARAM.SFO and USRDIR). In RPCS3 "
                 "the update is installed in dev_hdd0\\game\\" + kTitleId + ".";
    }
    else if (!containsText(sfo, kTitleId))
    {
        reason = std::string("This build of the port was made from the ") + FOLKLORE_RELEASE_NAME +
                 " 1.10 update (" + kTitleId + "), and \"" + folder + "\" holds another game or release.";
    }
    else if (!fileExists(eboot))
    {
        reason = fileExists(folder + "\\USRDIR\\EBOOT.BIN")
                     ? "The update's EBOOT.BIN is still encrypted. Decrypt it with RPCS3 (Utilities > Decrypt PS3 "
                       "Binaries, choose USRDIR\\EBOOT.BIN in this folder), which writes EBOOT.elf beside it, then "
                       "try again."
                     : "There is no USRDIR\\EBOOT.elf in \"" + folder + "\".";
    }
    else
    {
        const std::vector<unsigned char> header = readFile(eboot, 4);
        if (header.size() < 4 || header[0] != 0x7F || header[1] != 'E' || header[2] != 'L' || header[3] != 'F')
        {
            reason = "USRDIR\\EBOOT.elf is not a decrypted ELF. Decrypt EBOOT.BIN with RPCS3 (Utilities > Decrypt "
                     "PS3 Binaries) and try again.";
        }
        else if (fileSize(eboot) != kEbootSize)
        {
            reason = std::string("This EBOOT.elf is not the 1.10 update this build was made from (the decrypted ") +
                     kTitleId + " 1.10 EBOOT.elf is " + std::to_string(kEbootSize) + " bytes).";
        }
    }
    if (problem)
    {
        *problem = reason;
    }
    return reason.empty();
}

std::string enginePickGameFolder(const std::string& startFolder, bool forUpdate)
{
    const std::string picked = enginePickFolder(
        startFolder, forUpdate ? "Folklore: choose the installed 1.10 update (the folder that contains PARAM.SFO and USRDIR)"
                               : "Folklore: choose your game folder (the one that contains PS3_GAME)");
    if (picked.empty())
    {
        return picked;
    }
    return forUpdate ? engineNormaliseUpdateFolder(picked) : engineNormaliseGameFolder(picked);
}

std::string enginePickFolder(const std::string& startFolder, const std::string& title)
{
    const HRESULT initialised = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    std::string chosen;
    IFileOpenDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(narrowToWide(title).c_str());
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
                    chosen = wideToNarrow(widePath);
                    while (chosen.size() > 3 && (chosen.back() == '\\' || chosen.back() == '/'))
                    {
                        chosen.pop_back();
                    }
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

namespace
{
    /* ---- The 1.10 update's data -------------------------------------------
     *
     * Besides its EBOOT the 1.10 update carries a few data files, delivered
     * the way the game's downloadable packs are (folklore/docs/REGIONS.md):
     * the package puts them in USRDIR\bin\17 with a list, download.dat_, and
     * on its next start the game copies them into USRDIR\bin and adds them to
     * its record of installed files, install.dat_. Both lists have the same
     * format: a big-endian count, then per file its pack number (one byte)
     * and its path under bin (NUL-terminated). For the update both regions
     * have the same three files: LMonS2.bi5_ (the 1.10 executable's table of
     * Folks; 1.00 reads LMonS.bi5_), a corrected Spanish text file and the
     * list itself.
     *
     * The game reads LMonS2.bi5_ before it looks for packs to install, so the
     * files are put where the game's installer would have put them, and
     * recorded in install.dat_ as it would have, rather than left in bin\17
     * for the game to find. They are taken from the update folder: installed
     * already (USRDIR\bin, once the update has been started in RPCS3 or on a
     * PS3) or not yet (USRDIR\bin\17). Nothing in the update folder is
     * changed. */
    struct InstallEntry
    {
        int pack;
        std::string path;
    };

    bool readInstallList(const std::string& path, std::vector<InstallEntry>* entries)
    {
        const std::vector<unsigned char> bytes = readFile(path, 1024 * 1024);
        if (bytes.size() < 4)
        {
            return false;
        }
        const size_t count = ((size_t)bytes[0] << 24) | ((size_t)bytes[1] << 16) | ((size_t)bytes[2] << 8) | bytes[3];
        size_t position = 4;
        for (size_t index = 0; index < count; index++)
        {
            if (position >= bytes.size())
            {
                return false;
            }
            InstallEntry entry;
            entry.pack = bytes[position++];
            while (position < bytes.size() && bytes[position] != 0)
            {
                entry.path.push_back((char)bytes[position++]);
            }
            if (position >= bytes.size())
            {
                return false;
            }
            position++;
            entries->push_back(entry);
        }
        return true;
    }

    bool writeInstallList(const std::string& path, const std::vector<InstallEntry>& entries)
    {
        std::vector<unsigned char> bytes;
        const size_t count = entries.size();
        bytes.push_back((unsigned char)(count >> 24));
        bytes.push_back((unsigned char)(count >> 16));
        bytes.push_back((unsigned char)(count >> 8));
        bytes.push_back((unsigned char)count);
        for (const InstallEntry& entry : entries)
        {
            bytes.push_back((unsigned char)entry.pack);
            bytes.insert(bytes.end(), entry.path.begin(), entry.path.end());
            bytes.push_back(0);
        }
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file)
        {
            return false;
        }
        const size_t written = std::fwrite(bytes.data(), 1, bytes.size(), file);
        std::fclose(file);
        return written == bytes.size();
    }

    std::string windowsPath(std::string path)
    {
        for (char& character : path)
        {
            if (character == '/')
            {
                character = '\\';
            }
        }
        return path;
    }

    /* Every folder down to the one holding `filePath`. */
    void makeParentDirectories(const std::string& filePath)
    {
        const std::string folder = parentFolder(filePath);
        if (folder.empty() || folder == filePath || directoryExists(folder))
        {
            return;
        }
        makeParentDirectories(folder);
        makeDirectory(folder);
    }

    /* Where /dev_hdd0 is: PS3_HDD0_ROOT (run.ps1's, or the default set
     * above), read from the process environment itself -- the boot harness
     * caches getenv results. */
    std::string hdd0Root(const std::string& dataDirectory)
    {
        char value[MAX_PATH * 2];
        const DWORD length = GetEnvironmentVariableA("PS3_HDD0_ROOT", value, (DWORD)sizeof(value));
        if (length > 0 && length < sizeof(value))
        {
            return std::string(value, length);
        }
        return dataDirectory + "\\hdd0";
    }

    /* The update's data files into hdd0\game\<title id>\USRDIR\bin. Returns
     * false, with a sentence for the user, when the update folder has none. */
    bool installUpdateData(const std::string& updateFolder, const std::string& hdd0Folder, std::string* problem)
    {
        const std::string sourceBin = updateFolder + "\\USRDIR\\bin";
        const std::string packFolder = sourceBin + "\\17";
        std::string listPath = sourceBin + "\\download.dat_";
        if (fileExists(packFolder + "\\download.dat_"))
        {
            listPath = packFolder + "\\download.dat_";     /* not yet installed by the game */
        }
        std::vector<InstallEntry> updateFiles;
        if (!readInstallList(listPath, &updateFiles) || updateFiles.empty())
        {
            *problem = "The 1.10 update folder \"" + updateFolder + "\" has no USRDIR\\bin\\download.dat_ (the list "
                       "of the update's data files). Install the update again in RPCS3.";
            return false;
        }
        const std::string targetBin = hdd0Folder + "\\game\\" + kTitleId + "\\USRDIR\\bin";

        /* What the game has recorded as installed: a file a later pack
         * replaced (a DLC pack ships its own LMonS2.bi5_) is left alone. */
        const std::string recordPath = targetBin + "\\install.dat_";
        std::vector<InstallEntry> installed;
        if (fileExists(recordPath) && !readInstallList(recordPath, &installed))
        {
            installed.clear();
        }
        bool recordChanged = !fileExists(recordPath);
        int copied = 0;
        for (const InstallEntry& entry : updateFiles)
        {
            const std::string relative = windowsPath(entry.path);
            const std::string target = targetBin + "\\" + relative;
            InstallEntry* recorded = nullptr;
            for (InstallEntry& existing : installed)
            {
                if (_stricmp(existing.path.c_str(), entry.path.c_str()) == 0)
                {
                    recorded = &existing;
                }
            }
            if (recorded && recorded->pack > entry.pack && fileExists(target))
            {
                continue;
            }
            std::string source = packFolder + "\\" + relative;
            if (!fileExists(source))
            {
                source = sourceBin + "\\" + relative;
            }
            if (!fileExists(source))
            {
                *problem = "The 1.10 update folder \"" + updateFolder + "\" is missing USRDIR\\bin\\" + relative +
                           ". Install the update again in RPCS3.";
                return false;
            }
            if (!fileExists(target) || fileSize(target) != fileSize(source))
            {
                makeParentDirectories(target);
                if (!CopyFileA(source.c_str(), target.c_str(), FALSE))
                {
                    *problem = "Could not copy the 1.10 update's " + relative + " to \"" + target + "\" (error " +
                               std::to_string(GetLastError()) + ").";
                    return false;
                }
                copied++;
            }
            if (!recorded)
            {
                installed.push_back(entry);
                recordChanged = true;
            }
            else if (recorded->pack < entry.pack)
            {
                recorded->pack = entry.pack;
                recordChanged = true;
            }
        }
        if (recordChanged && !writeInstallList(recordPath, installed))
        {
            *problem = "Could not write \"" + recordPath + "\".";
            return false;
        }
        std::fprintf(stderr, "[engine] 1.10 update data: %d of %d file(s) copied from %s\n", copied,
                     (int)updateFiles.size(), updateFolder.c_str());
        return true;
    }
}

/* Standalone: set run.ps1's environment, start the game as a child the way
 * run.ps1 does, wait for it and exit with its code. If the child cannot be
 * started, run the game here after all, logging to the file. */
static const char* launchAndExit(const std::string& eboot, const std::string& gameFolder,
                                 const std::string& dataDirectory)
{
    static std::string fallback;
    const std::string stamp = timeStamp();
    applyLauncherEnvironment(dataDirectory, gameFolder, stamp);
    const std::string logPath = dataDirectory + "\\logs\\run-" + stamp + ".log";
    DWORD exitCode = 0;
    if (runAsLauncher(eboot, dataDirectory, logPath, &exitCode))
    {
        ExitProcess(exitCode);
    }
    const DWORD startError = GetLastError();
    redirectLogIfStandalone(dataDirectory);
    SetCurrentDirectoryA(dataDirectory.c_str());
    std::fprintf(stderr, "[engine] could not start the game as a child process (error %lu) -- running it here\n",
                 startError);
    fallback = eboot;
    return fallback.c_str();
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
    const bool standalone = !environmentSet("FOLKLORE_LAUNCHER");
    if (standalone)
    {
        /* This first process only finds the game and starts the real run;
         * the console Windows opened for a double-click has nothing to show.
         * A console someone started the exe from is left alone. */
        DWORD processes[4];
        if (GetConsoleProcessList(processes, 4) == 1)
        {
            FreeConsole();
        }
    }
    applyEnvironmentDefaults(baseDirectory, dataDirectory);
    std::fprintf(stderr, "[engine] this build: %s (%s) v%s\n", FOLKLORE_TITLE_ID, FOLKLORE_REGION_NAME,
                 FOLKLORE_VERSION_NAME);
    /* DLC packs the engine menu was asked to remove: now, before the game
     * opens any of their files (engine_dlc.cpp). */
    engineDlcApplyPendingRemovals();

    if (elfFromCommandLine && *elfFromCommandLine)
    {
        char fullPath[MAX_PATH * 2];
        const DWORD length = GetFullPathNameA(elfFromCommandLine, (DWORD)sizeof(fullPath), fullPath, nullptr);
        ebootPath = (length > 0 && length < sizeof(fullPath)) ? std::string(fullPath, length) : elfFromCommandLine;
    }
    if (!ebootPath.empty())
    {
        /* A 1.10 build given the update's EBOOT (run.ps1, a shortcut): the
         * update folder is two folders up, and the disc dump comes from
         * PS3_VFS_ROOT or the game_dir setting. */
        std::string gameFolder;
        if (FOLKLORE_USES_UPDATE)
        {
            char vfsRoot[MAX_PATH * 2];
            const DWORD vfsLength = GetEnvironmentVariableA("PS3_VFS_ROOT", vfsRoot, (DWORD)sizeof(vfsRoot));
            gameFolder = (vfsLength > 0 && vfsLength < sizeof(vfsRoot)) ? std::string(vfsRoot, vfsLength)
                                                                         : engineNormaliseGameFolder(settings.gameDir);
            const std::string updateFolder = parentFolder(parentFolder(ebootPath));
            std::string problem;
            if (engineCheckUpdateFolder(updateFolder, &problem) &&
                !installUpdateData(updateFolder, hdd0Root(dataDirectory), &problem))
            {
                showError(problem);
                return nullptr;
            }
        }
        else
        {
            gameFolder = parentFolder(parentFolder(parentFolder(ebootPath)));
        }
        if (standalone)
        {
            return launchAndExit(ebootPath, gameFolder, dataDirectory);     /* a shortcut with the EBOOT path */
        }
        return ebootPath.c_str();     /* run.ps1, or this exe as the launcher */
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
        else if (FOLKLORE_USES_UPDATE)
        {
            MessageBoxA(nullptr,
                        "This is the " FOLKLORE_VERSION_NAME " build of Folklore (" FOLKLORE_TITLE_ID "). It needs two "
                        "folders:\n\n1. Your disc dump: the folder that contains PS3_GAME.\n2. The installed 1.10 "
                        "update, with its USRDIR\\EBOOT.BIN decrypted to EBOOT.elf with RPCS3 (in RPCS3 it is "
                        "dev_hdd0\\game\\" FOLKLORE_TITLE_ID ").\n\nChoose the disc dump first. The game remembers "
                        "both; change them later in the engine menu (F1).",
                        "Folklore", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
        }
        else
        {
            MessageBoxA(nullptr,
                        "Choose the folder of your Folklore (" FOLKLORE_TITLE_ID ") disc dump: the folder that contains "
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

    if (FOLKLORE_USES_UPDATE)
    {
        /* The 1.10 update: its EBOOT is what runs, and its data files go into
         * the installed game data (installUpdateData). */
        std::string updateFolder = engineNormaliseUpdateFolder(settings.updateDir);
        bool firstUpdateAttempt = true;
        while (!engineCheckUpdateFolder(updateFolder, &problem))
        {
            if (!firstUpdateAttempt || !settings.updateDir.empty())
            {
                showError(problem);
            }
            else
            {
                MessageBoxA(nullptr,
                            "Now choose the installed 1.10 update: the folder that contains PARAM.SFO and USRDIR (in "
                            "RPCS3: dev_hdd0\\game\\" FOLKLORE_TITLE_ID "). Its USRDIR\\EBOOT.BIN must be decrypted "
                            "to EBOOT.elf with RPCS3 first. Nothing in that folder is changed.",
                            "Folklore", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            }
            firstUpdateAttempt = false;
            const std::string picked = enginePickGameFolder(updateFolder.empty() ? gameFolder : updateFolder, true);
            if (picked.empty())
            {
                std::fprintf(stderr, "[engine] no update folder chosen -- quitting\n");
                return nullptr;
            }
            updateFolder = picked;
        }
        if (updateFolder != settings.updateDir)
        {
            settings.updateDir = updateFolder;
            engineSettingsSave();
        }
        if (!installUpdateData(updateFolder, hdd0Root(dataDirectory), &problem))
        {
            showError(problem);
            return nullptr;
        }
        ebootPath = updateFolder + "\\USRDIR\\EBOOT.elf";
        std::fprintf(stderr, "[engine] 1.10 update folder: %s\n", updateFolder.c_str());
    }
    if (standalone)
    {
        return launchAndExit(ebootPath, gameFolder, dataDirectory);
    }
    return ebootPath.c_str();
}

#endif /* _WIN32 */
