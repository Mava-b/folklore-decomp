/*
 * engine_saves.h -- snapshots of the game's save data, for the engine menu.
 *
 * A true save state (freeze the whole game and resume it later) is not
 * possible in this port: every guest thread runs as a native thread inside
 * recompiled code, so its state is a host call stack that cannot be written
 * out and rebuilt. What can be kept is the game's own save data. A snapshot
 * copies every save directory the game has written (gamedata\dev_hdd0\home\
 * 00000001\savedata, relative to the working directory run.ps1 uses) into
 * save_snapshots\<name>; restoring copies one back over the current saves,
 * after which the game's own Load menu picks it up.
 */
#pragma once

#include <string>
#include <vector>

struct SaveSnapshot
{
    std::string name;      /* folder name: yyyyMMdd-HHmmss */
    int saveCount = 0;     /* save directories inside it */
};

/* Save directories the game currently has. */
int saveDataCount();

/* Copy the current save data to a new snapshot. Returns false with a
 * reason in `message` when there is nothing to copy or the copy fails. */
bool saveSnapshotTake(std::string& message);

/* The same, with a few words appended to the snapshot's name (letters,
 * digits and '-' kept; e.g. "before-removing-DLC"), so it can be found
 * later. `name` receives the snapshot's folder name. */
bool saveSnapshotTakeLabelled(const std::string& label, std::string& message, std::string& name);

/* Snapshots, newest first. */
std::vector<SaveSnapshot> saveSnapshotList();

/* Replace the current save data with a snapshot's. */
bool saveSnapshotRestore(const std::string& name, std::string& message);
