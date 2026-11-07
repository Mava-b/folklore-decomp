/*
 * engine_save_slots.cpp -- see engine_save_slots.h.
 */
#include "engine_save_slots.h"

#include "engine_portable.h"
#include "engine_region.h"
#include "engine_settings.h"

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    constexpr int kMaxSlot = 9999;
    const char* const kSelectVariable = "PS3_SAVEDATA_SELECT";

    std::string directoryPrefix()
    {
        return std::string(FOLKLORE_TITLE_ID) + "-SAVEDATA-";
    }

    bool isDirectory(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    bool isFile(const std::string& path)
    {
        const DWORD attributes = GetFileAttributesA(path.c_str());
        return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
    }

    std::vector<unsigned char> readWholeFile(const std::string& path, size_t limit)
    {
        std::vector<unsigned char> bytes;
        std::ifstream stream(path, std::ios::binary);
        if (!stream)
        {
            return bytes;
        }
        bytes.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
        if (bytes.size() > limit)
        {
            bytes.clear();
        }
        return bytes;
    }

    std::vector<std::string> subfolders(const std::string& folder)
    {
        std::vector<std::string> names;
        WIN32_FIND_DATAA found;
        const HANDLE search = FindFirstFileA((folder + "\\*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
        {
            return names;
        }
        do
        {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && found.cFileName[0] != '.')
            {
                names.push_back(found.cFileName);
            }
        } while (FindNextFileA(search, &found));
        FindClose(search);
        std::sort(names.begin(), names.end());
        return names;
    }

    uint32_t readLittle32(const unsigned char* bytes)
    {
        return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) | ((uint32_t)bytes[2] << 16) |
               ((uint32_t)bytes[3] << 24);
    }

    uint16_t readLittle16(const unsigned char* bytes)
    {
        return (uint16_t)(bytes[0] | (bytes[1] << 8));
    }

    /* A PARAM.SFO's text values: "\0PSF", version, key table, data table,
     * entry count, then 16-byte entries (key offset 2, format 2, length 4,
     * max length 4, data offset 4). Only text (format 0x0204) is kept. */
    bool readSfoText(const std::string& path, std::vector<std::pair<std::string, std::string>>& values)
    {
        const std::vector<unsigned char> sfo = readWholeFile(path, 64 * 1024);
        if (sfo.size() < 20 || sfo[0] != 0 || sfo[1] != 'P' || sfo[2] != 'S' || sfo[3] != 'F')
        {
            return false;
        }
        const uint32_t keyTable = readLittle32(&sfo[8]);
        const uint32_t dataTable = readLittle32(&sfo[12]);
        const uint32_t entryCount = readLittle32(&sfo[16]);
        for (uint32_t entryIndex = 0; entryIndex < entryCount; entryIndex++)
        {
            const size_t entry = 20 + (size_t)entryIndex * 16;
            if (entry + 16 > sfo.size())
            {
                break;
            }
            const uint32_t keyOffset = keyTable + readLittle16(&sfo[entry]);
            const uint16_t format = readLittle16(&sfo[entry + 2]);
            const uint32_t length = readLittle32(&sfo[entry + 4]);
            const uint32_t dataOffset = dataTable + readLittle32(&sfo[entry + 12]);
            if (format != 0x0204 || keyOffset >= sfo.size() || (size_t)dataOffset + length > sfo.size())
            {
                continue;
            }
            std::string key;
            for (size_t index = keyOffset; index < sfo.size() && sfo[index] != 0; index++)
            {
                key.push_back((char)sfo[index]);
            }
            std::string value((const char*)&sfo[dataOffset], length);
            const size_t terminator = value.find('\0');
            if (terminator != std::string::npos)
            {
                value.resize(terminator);
            }
            values.emplace_back(key, value);
        }
        return true;
    }

    std::string sfoValue(const std::vector<std::pair<std::string, std::string>>& values, const char* key)
    {
        for (const auto& value : values)
        {
            if (value.first == key)
            {
                return value.second;
            }
        }
        return std::string();
    }

    /* The last time a file was written, as local time. */
    bool fileWriteTime(const std::string& path, std::string& text, long long& seconds)
    {
        WIN32_FILE_ATTRIBUTE_DATA attributes;
        if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &attributes))
        {
            return false;
        }
        const unsigned long long ticks = ((unsigned long long)attributes.ftLastWriteTime.dwHighDateTime << 32) |
                                         attributes.ftLastWriteTime.dwLowDateTime;
        seconds = (long long)(ticks / 10000000ull) - 11644473600ll;
        FILETIME localTime;
        SYSTEMTIME clock;
        if (!FileTimeToLocalFileTime(&attributes.ftLastWriteTime, &localTime) ||
            !FileTimeToSystemTime(&localTime, &clock))
        {
            return false;
        }
        static const char* const kDays[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
        static const char* const kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                              "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%s %u %s %u, %02u:%02u", kDays[clock.wDayOfWeek % 7],
                      (unsigned)clock.wDay, kMonths[(clock.wMonth + 11) % 12], (unsigned)clock.wYear,
                      (unsigned)clock.wHour, (unsigned)clock.wMinute);
        text = buffer;
        return true;
    }

    std::string timeStampNow()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local = {};
        localtime_s(&local, &now);
        char text[32];
        std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
        return text;
    }

    /* Slot number from a directory name of this release, or -1. */
    int slotNumberOf(const std::string& directoryName)
    {
        const std::string prefix = directoryPrefix();
        if (directoryName.size() <= prefix.size() || directoryName.compare(0, prefix.size(), prefix) != 0)
        {
            return -1;
        }
        int number = 0;
        for (size_t index = prefix.size(); index < directoryName.size(); index++)
        {
            const char character = directoryName[index];
            if (character < '0' || character > '9')
            {
                return -1;
            }
            number = number * 10 + (character - '0');
            if (number > kMaxSlot)
            {
                return -1;
            }
        }
        return number;
    }

    /* A Folklore save folder: a PARAM.SFO and SAVEDATA.BIN, from either
     * release. */
    bool isFolkloreSave(const std::string& folder)
    {
        if (!isFile(folder + "\\PARAM.SFO") || !isFile(folder + "\\SAVEDATA.BIN"))
        {
            return false;
        }
        std::vector<std::pair<std::string, std::string>> values;
        if (!readSfoText(folder + "\\PARAM.SFO", values))
        {
            return false;
        }
        const std::string directory = sfoValue(values, "SAVEDATA_DIRECTORY");
        const std::string title = sfoValue(values, "TITLE");
        const bool folkloreDirectory = directory.rfind(FOLKLORE_US_TITLE_ID, 0) == 0 ||
                                       directory.rfind(FOLKLORE_EU_TITLE_ID, 0) == 0;
        return folkloreDirectory || title.rfind("Folklore", 0) == 0;
    }

    bool sameFileContents(const std::string& firstPath, const std::string& secondPath)
    {
        const std::vector<unsigned char> first = readWholeFile(firstPath, 64u * 1024u * 1024u);
        if (first.empty())
        {
            return false;
        }
        return first == readWholeFile(secondPath, 64u * 1024u * 1024u);
    }

    /* Copy a folder's files and subfolders with CopyFileA, which keeps each
     * file's time (the slot's "saved" time comes from it). */
    bool copyFolderContents(const std::string& source, const std::string& destination, std::string& problem)
    {
        if (!CreateDirectoryA(destination.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        {
            problem = "could not make \"" + destination + "\" (error " + std::to_string(GetLastError()) + ")";
            return false;
        }
        WIN32_FIND_DATAA found;
        const HANDLE search = FindFirstFileA((source + "\\*").c_str(), &found);
        if (search == INVALID_HANDLE_VALUE)
        {
            problem = "could not read \"" + source + "\"";
            return false;
        }
        bool copied = true;
        do
        {
            const std::string name = found.cFileName;
            if (name == "." || name == "..")
            {
                continue;
            }
            const std::string from = source + "\\" + name;
            const std::string to = destination + "\\" + name;
            if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            {
                copied = copyFolderContents(from, to, problem);
            }
            else if (!CopyFileA(from.c_str(), to.c_str(), FALSE))
            {
                problem = "could not copy \"" + from + "\" (error " + std::to_string(GetLastError()) + ")";
                copied = false;
            }
        } while (copied && FindNextFileA(search, &found));
        FindClose(search);
        return copied;
    }

    /* Copy `source` to a new folder `destination`; on a failure, nothing is
     * left behind. */
    bool copyFolder(const std::string& source, const std::string& destination, std::string& problem)
    {
        std::error_code error;
        fs::create_directories(fs::path(destination).parent_path(), error);
        if (copyFolderContents(source, destination, problem))
        {
            return true;
        }
        fs::remove_all(destination, error);
        return false;
    }

    /* An imported save keeps the directory name it was made under in its
     * PARAM.SFO (SAVEDATA_DIRECTORY); a PS3 writes the folder's own name
     * there. Rename it in place for a save of this release, so the file
     * agrees with the slot (a save of the other release keeps its own, which
     * is how the Saves page knows to warn about it). */
    void renameSfoDirectory(const std::string& folder, const std::string& directoryName)
    {
        const std::string path = folder + "\\PARAM.SFO";
        std::vector<unsigned char> sfo = readWholeFile(path, 64 * 1024);
        if (sfo.size() < 20 || sfo[1] != 'P' || sfo[2] != 'S' || sfo[3] != 'F')
        {
            return;
        }
        const uint32_t keyTable = readLittle32(&sfo[8]);
        const uint32_t dataTable = readLittle32(&sfo[12]);
        const uint32_t entryCount = readLittle32(&sfo[16]);
        for (uint32_t entryIndex = 0; entryIndex < entryCount; entryIndex++)
        {
            const size_t entry = 20 + (size_t)entryIndex * 16;
            if (entry + 16 > sfo.size())
            {
                return;
            }
            const uint32_t keyOffset = keyTable + readLittle16(&sfo[entry]);
            if (keyOffset >= sfo.size() ||
                std::strncmp((const char*)&sfo[keyOffset], "SAVEDATA_DIRECTORY", sfo.size() - keyOffset) != 0)
            {
                continue;
            }
            const uint32_t length = readLittle32(&sfo[entry + 4]);
            const uint32_t maxLength = readLittle32(&sfo[entry + 8]);
            const uint32_t dataOffset = dataTable + readLittle32(&sfo[entry + 12]);
            if ((size_t)dataOffset + maxLength > sfo.size() || directoryName.size() + 1 > maxLength)
            {
                return;
            }
            const std::string current((const char*)&sfo[dataOffset], strnlen((const char*)&sfo[dataOffset], length));
            if (current == directoryName || current.rfind(FOLKLORE_TITLE_ID, 0) != 0)
            {
                return;
            }
            std::memset(&sfo[dataOffset], 0, maxLength);
            std::memcpy(&sfo[dataOffset], directoryName.c_str(), directoryName.size());
            const uint32_t newLength = (uint32_t)directoryName.size() + 1;
            sfo[entry + 4] = (unsigned char)newLength;
            sfo[entry + 5] = (unsigned char)(newLength >> 8);
            sfo[entry + 6] = (unsigned char)(newLength >> 16);
            sfo[entry + 7] = (unsigned char)(newLength >> 24);
            std::ofstream stream(path, std::ios::binary | std::ios::trunc);
            stream.write((const char*)sfo.data(), (std::streamsize)sfo.size());
            return;
        }
    }

    /* Save folders under `pickedFolder`, in the ways a player might point
     * at them. */
    std::vector<std::string> findSaves(const std::string& pickedFolder)
    {
        std::vector<std::string> saves;
        std::string folder = pickedFolder;
        while (!folder.empty() && (folder.back() == '\\' || folder.back() == '/'))
        {
            folder.pop_back();
        }
        if (isFolkloreSave(folder))
        {
            saves.push_back(folder);
            return saves;
        }
        for (const std::string& name : subfolders(folder))
        {
            if (isFolkloreSave(folder + "\\" + name))
            {
                saves.push_back(folder + "\\" + name);
            }
        }
        if (!saves.empty())
        {
            return saves;
        }
        /* RPCS3's folder, its dev_hdd0, or a home folder: <...>\home\<user>\savedata. */
        const std::string homeCandidates[3] = {folder + "\\dev_hdd0\\home", folder + "\\home", folder};
        for (const std::string& home : homeCandidates)
        {
            for (const std::string& user : subfolders(home))
            {
                const std::string savedata = home + "\\" + user + "\\savedata";
                for (const std::string& name : subfolders(savedata))
                {
                    if (isFolkloreSave(savedata + "\\" + name))
                    {
                        saves.push_back(savedata + "\\" + name);
                    }
                }
            }
            if (!saves.empty())
            {
                break;
            }
        }
        return saves;
    }
}

std::string engineSaveSlotFolder()
{
    return engineDataDirectory() + "\\gamedata\\dev_hdd0\\home\\00000001\\savedata";
}

std::string engineSaveSlotDirectoryName(int number)
{
    char digits[8];
    std::snprintf(digits, sizeof(digits), "%04d", std::clamp(number, 0, kMaxSlot));
    return directoryPrefix() + digits;
}

SaveSlot engineSaveSlotRead(int number)
{
    SaveSlot slot;
    slot.number = number;
    slot.directoryName = engineSaveSlotDirectoryName(number);
    slot.folder = engineSaveSlotFolder() + "\\" + slot.directoryName;
    std::vector<std::pair<std::string, std::string>> values;
    if (!readSfoText(slot.folder + "\\PARAM.SFO", values))
    {
        return slot;
    }
    slot.exists = true;
    slot.subtitle = sfoValue(values, "SUB_TITLE");
    const std::string detail = sfoValue(values, "DETAIL");
    size_t lineStart = 0;
    while (lineStart <= detail.size())
    {
        const size_t lineEnd = detail.find('\n', lineStart);
        const std::string line = detail.substr(lineStart, lineEnd == std::string::npos ? std::string::npos
                                                                                       : lineEnd - lineStart);
        if (!line.empty())
        {
            slot.detailLines.push_back(line);
        }
        if (lineEnd == std::string::npos)
        {
            break;
        }
        lineStart = lineEnd + 1;
    }
    const std::string savedDirectory = sfoValue(values, "SAVEDATA_DIRECTORY");
    if (savedDirectory.rfind(FOLKLORE_TITLE_ID, 0) != 0)
    {
        if (savedDirectory.rfind(FOLKLORE_EU_TITLE_ID, 0) == 0)
        {
            slot.otherRelease = "European";
        }
        else if (savedDirectory.rfind(FOLKLORE_US_TITLE_ID, 0) == 0)
        {
            slot.otherRelease = "North American";
        }
    }
    const std::string dataFile = isFile(slot.folder + "\\SAVEDATA.BIN") ? slot.folder + "\\SAVEDATA.BIN"
                                                                         : slot.folder + "\\PARAM.SFO";
    fileWriteTime(dataFile, slot.savedAt, slot.savedAtSeconds);
    if (isFile(slot.folder + "\\ICON0.PNG"))
    {
        slot.iconPath = slot.folder + "\\ICON0.PNG";
    }
    return slot;
}

std::vector<SaveSlot> engineSaveSlotList()
{
    std::vector<SaveSlot> slots;
    for (const std::string& name : subfolders(engineSaveSlotFolder()))
    {
        const int number = slotNumberOf(name);
        if (number < 0)
        {
            continue;
        }
        SaveSlot slot = engineSaveSlotRead(number);
        if (slot.exists)
        {
            slots.push_back(slot);
        }
    }
    std::sort(slots.begin(), slots.end(),
              [](const SaveSlot& first, const SaveSlot& second) { return first.number < second.number; });
    return slots;
}

int engineSaveSlotNextFree()
{
    for (int number = 0; number <= kMaxSlot; number++)
    {
        if (!isDirectory(engineSaveSlotFolder() + "\\" + engineSaveSlotDirectoryName(number)))
        {
            return number;
        }
    }
    return kMaxSlot;
}

std::string engineSaveSlotImport(const std::string& pickedFolder, int* firstSlot)
{
    if (firstSlot)
    {
        *firstSlot = -1;
    }
    const std::vector<std::string> saves = findSaves(pickedFolder);
    if (saves.empty())
    {
        return "No Folklore saves found in \"" + pickedFolder +
               "\". Choose a save folder (it has PARAM.SFO and SAVEDATA.BIN in it), a folder of them, or "
               "RPCS3's folder.";
    }
    const std::vector<SaveSlot> existing = engineSaveSlotList();
    std::string report;
    int imported = 0;
    for (const std::string& save : saves)
    {
        const std::string name = save.substr(save.find_last_of('\\') + 1);
        bool duplicate = false;
        for (const SaveSlot& slot : existing)
        {
            if (sameFileContents(save + "\\SAVEDATA.BIN", slot.folder + "\\SAVEDATA.BIN"))
            {
                report += name + ": already in slot " + std::to_string(slot.number) + ".\n";
                duplicate = true;
                break;
            }
        }
        if (duplicate)
        {
            continue;
        }
        const int number = engineSaveSlotNextFree();
        const std::string destination = engineSaveSlotFolder() + "\\" + engineSaveSlotDirectoryName(number);
        std::string problem;
        if (!copyFolder(save, destination, problem))
        {
            report += name + ": not imported, " + problem + ".\n";
            continue;
        }
        renameSfoDirectory(destination, engineSaveSlotDirectoryName(number));
        imported++;
        if (firstSlot && *firstSlot < 0)
        {
            *firstSlot = number;
        }
        const SaveSlot slot = engineSaveSlotRead(number);
        report += name + ": now slot " + std::to_string(number) +
                  (slot.subtitle.empty() ? std::string() : " (" + slot.subtitle + ")") +
                  (slot.otherRelease.empty() ? std::string()
                                             : ". Made with the " + slot.otherRelease +
                                                   " release; it may not load in this one") +
                  ".\n";
    }
    std::fprintf(stderr, "[saves] import from %s: %d of %zu\n", pickedFolder.c_str(), imported, saves.size());
    return (imported > 0 ? std::to_string(imported) + (imported == 1 ? " save imported.\n" : " saves imported.\n")
                         : std::string("No saves imported.\n")) +
           report;
}

std::string engineSaveSlotExport(int number, const std::string& destinationFolder)
{
    const SaveSlot slot = engineSaveSlotRead(number);
    if (!slot.exists)
    {
        return "Slot " + std::to_string(number) + " has no save to export.";
    }
    const std::string destination = destinationFolder + "\\" + slot.directoryName;
    if (isDirectory(destination))
    {
        return "\"" + destinationFolder + "\" already has a " + slot.directoryName +
               " folder. Choose another folder, or move that one first.";
    }
    std::string problem;
    if (!copyFolder(slot.folder, destination, problem))
    {
        return "Slot " + std::to_string(number) + " was not exported: " + problem + ".";
    }
    std::fprintf(stderr, "[saves] exported slot %d to %s\n", number, destination.c_str());
    return "Slot " + std::to_string(number) + " exported to \"" + destination +
           "\". It can go straight into RPCS3's dev_hdd0\\home\\00000001\\savedata folder.";
}

std::string engineSaveSlotDelete(int number)
{
    const SaveSlot slot = engineSaveSlotRead(number);
    if (!isDirectory(slot.folder))
    {
        return "Slot " + std::to_string(number) + " has no save.";
    }
    const std::string bin = engineDataDirectory() + "\\deleted_saves";
    const std::string destination = bin + "\\" + timeStampNow() + "-" + slot.directoryName;
    std::error_code error;
    fs::create_directories(bin, error);
    fs::rename(slot.folder, destination, error);
    if (error)
    {
        std::string problem;
        if (!copyFolder(slot.folder, destination, problem))
        {
            return "Slot " + std::to_string(number) + " was not deleted: " + problem + ".";
        }
        fs::remove_all(slot.folder, error);
        if (error)
        {
            return "Slot " + std::to_string(number) + " was copied to \"" + destination +
                   "\" but could not be removed: " + error.message() + ".";
        }
    }
    std::fprintf(stderr, "[saves] slot %d moved to %s\n", number, destination.c_str());
    return "Slot " + std::to_string(number) + " deleted. A copy is in \"" + destination +
           "\" (Import brings it back).";
}

void engineSaveSlotApplyEnvironment()
{
    const char* existing = std::getenv(kSelectVariable);
    if (existing && *existing)
    {
        std::fprintf(stderr, "[saves] save slot: %s (from the environment)\n", existing);
        return;
    }
    const std::string directory = engineSaveSlotDirectoryName(engineSettings().saveSlot);
    _putenv_s(kSelectVariable, directory.c_str());
    SetEnvironmentVariableA(kSelectVariable, directory.c_str());
    const SaveSlot slot = engineSaveSlotRead(engineSettings().saveSlot);
    std::fprintf(stderr, "[saves] save slot %d: %s (%s)\n", slot.number, directory.c_str(),
                 slot.exists ? slot.subtitle.c_str() : "empty: a new game");
}
