/*
 * engine_saves.cpp -- see engine_saves.h.
 */
#include "engine_saves.h"

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <system_error>

namespace
{
    namespace fs = std::filesystem;

    const fs::path kSaveRoot = fs::path("gamedata") / "dev_hdd0" / "home" / "00000001" / "savedata";
    const fs::path kSnapshotRoot = fs::path("save_snapshots");

    int countDirectories(const fs::path& folder)
    {
        std::error_code error;
        int count = 0;
        for (fs::directory_iterator entry(folder, error), end; !error && entry != end; entry.increment(error))
        {
            if (entry->is_directory(error))
            {
                count++;
            }
        }
        return count;
    }

    std::string timestampName()
    {
        const std::time_t now = std::time(nullptr);
        std::tm local = {};
#ifdef _WIN32
        localtime_s(&local, &now);
#else
        localtime_r(&now, &local);
#endif
        char text[32];
        std::strftime(text, sizeof(text), "%Y%m%d-%H%M%S", &local);
        return text;
    }
}

int saveDataCount()
{
    return countDirectories(kSaveRoot);
}

bool saveSnapshotTake(std::string& message)
{
    if (saveDataCount() == 0)
    {
        message = "The game has no save data yet: save in the game first.";
        return false;
    }
    std::error_code error;
    std::string name = timestampName();
    fs::path target = kSnapshotRoot / name;
    for (int suffix = 2; fs::exists(target, error); suffix++)
    {
        target = kSnapshotRoot / (name + "-" + std::to_string(suffix));
    }
    fs::create_directories(target, error);
    if (error)
    {
        message = "Could not create " + target.string() + ": " + error.message();
        return false;
    }
    fs::copy(kSaveRoot, target, fs::copy_options::recursive, error);
    if (error)
    {
        message = "Copy failed: " + error.message();
        return false;
    }
    message = "Saved snapshot " + target.filename().string();
    return true;
}

std::vector<SaveSnapshot> saveSnapshotList()
{
    std::vector<SaveSnapshot> snapshots;
    std::error_code error;
    for (fs::directory_iterator entry(kSnapshotRoot, error), end; !error && entry != end; entry.increment(error))
    {
        if (!entry->is_directory(error))
        {
            continue;
        }
        SaveSnapshot snapshot;
        snapshot.name = entry->path().filename().string();
        snapshot.saveCount = countDirectories(entry->path());
        snapshots.push_back(snapshot);
    }
    std::sort(snapshots.begin(), snapshots.end(),
              [](const SaveSnapshot& left, const SaveSnapshot& right) { return left.name > right.name; });
    return snapshots;
}

bool saveSnapshotRestore(const std::string& name, std::string& message)
{
    std::error_code error;
    const fs::path source = kSnapshotRoot / name;
    if (name.empty() || name.find_first_of("/\\") != std::string::npos || !fs::is_directory(source, error))
    {
        message = "No snapshot called " + name;
        return false;
    }
    /* Keep what is being replaced, so a restore can itself be undone. */
    if (saveDataCount() > 0)
    {
        std::string backupMessage;
        saveSnapshotTake(backupMessage);
    }
    fs::create_directories(kSaveRoot, error);
    for (fs::directory_iterator entry(kSaveRoot, error), end; !error && entry != end; entry.increment(error))
    {
        std::error_code removeError;
        fs::remove_all(entry->path(), removeError);
    }
    error.clear();
    fs::copy(source, kSaveRoot, fs::copy_options::recursive | fs::copy_options::overwrite_existing, error);
    if (error)
    {
        message = "Restore failed: " + error.message();
        return false;
    }
    message = "Restored " + name + ". Load it from the game's Load menu (return to the title screen if the game is in play).";
    return true;
}
