/*
 * engine_save_slots.h -- save slots, chosen in the launcher before the game
 * starts.
 *
 * Folklore keeps each save in its own directory, <title id>-SAVEDATA-NNNN
 * (a PS3 shows them in a list and the player picks one; RPCS3 and a PS3
 * keep several). The port has no such list, so the game used to load and
 * overwrite whichever came first. A slot is one of those directories: the
 * save_slot setting names it, the bootstrap passes it to the runtime as
 * PS3_SAVEDATA_SELECT, and the game then sees only that directory when it
 * loads and writes only that one when it saves (patches/0086). An empty
 * slot is a new game.
 *
 * Each slot's PARAM.SFO says where and when it was saved (SUB_TITLE: the
 * character, realm and area; DETAIL: both characters' chapters and the play
 * time), and ICON0.PNG is the picture of that area the PS3 shows in its list.
 *
 * Slots are folders of the same shape on a PS3, in RPCS3 and here, so
 * import and export are folder copies: an exported slot can go straight
 * into RPCS3's dev_hdd0\home\00000001\savedata, and RPCS3's saves can be
 * imported.
 */
#pragma once

#include <string>
#include <vector>

struct SaveSlot
{
    int number = 0;
    std::string directoryName;          /* BCUS98147-SAVEDATA-0003 */
    std::string folder;                 /* full path */
    bool exists = false;                /* has a PARAM.SFO */
    std::string subtitle;               /* "Ellen Faery Realm Gesh Forest" */
    std::vector<std::string> detailLines;   /* "Keats Prologue", ..., "Play time 00:39" */
    std::string savedAt;                /* "Sat 7 Nov 2026, 08:13" */
    long long savedAtSeconds = 0;       /* for sorting; 0 when unknown */
    std::string iconPath;               /* ICON0.PNG, "" when there is none */
    std::string otherRelease;           /* "European", "North American" or "" */
};

/* Where the game keeps its saves: <data folder>\gamedata\dev_hdd0\home\
 * 00000001\savedata. */
std::string engineSaveSlotFolder();

/* <title id>-SAVEDATA-NNNN for slot `number`. */
std::string engineSaveSlotDirectoryName(int number);

/* Every slot that has a save, by number. */
std::vector<SaveSlot> engineSaveSlotList();

/* One slot, whether or not it has a save. */
SaveSlot engineSaveSlotRead(int number);

/* The lowest slot number without a save. */
int engineSaveSlotNextFree();

/* Copy saves in from `pickedFolder`: one save folder (with PARAM.SFO), a
 * folder of them (RPCS3's savedata, a folder of exports), or an RPCS3 or
 * dev_hdd0 folder. Each goes into the next free slot; a save already in a
 * slot (same SAVEDATA.BIN) is skipped. Returns a report for the player;
 * `firstSlot` receives the first slot filled, or -1. */
std::string engineSaveSlotImport(const std::string& pickedFolder, int* firstSlot);

/* Copy slot `number` to <destinationFolder>\<its directory name>, the
 * shape RPCS3 and a PS3 use. Returns a report for the player. */
std::string engineSaveSlotExport(int number, const std::string& destinationFolder);

/* Move slot `number` out of the game's saves into <data folder>\
 * deleted_saves\<date-time>-<directory name> (nothing is lost). Returns a
 * report for the player. */
std::string engineSaveSlotDelete(int number);

/* Tell the runtime which save directory to use (PS3_SAVEDATA_SELECT),
 * unless the environment already names one. Called by the bootstrap. */
void engineSaveSlotApplyEnvironment();
