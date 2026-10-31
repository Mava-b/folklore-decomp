/*
 * engine_portable.h -- finding the game files when folklore.exe is started on
 * its own (double-clicked, or from a shared folder) rather than by run.ps1.
 */
#pragma once

#include <string>

/* The folder the user means: a picked PS3_GAME or USRDIR folder becomes the
 * folder above PS3_GAME; trailing separators go. */
std::string engineNormaliseGameFolder(const std::string& folder);

/* True when `folder` is a dump of the release this port was built from: its
 * PS3_GAME, and for a v1.00 build its EBOOT (a 1.10 build runs the update's
 * EBOOT instead). Otherwise false, with a sentence for the user in *problem.
 * With `ebootPath`, also the ELF to run: PS3_GAME\USRDIR\EBOOT.elf when there
 * is one (RPCS3's), otherwise EBOOT.BIN decrypted into the data folder's
 * "decrypted" folder (engine_self.cpp; done once, then reused). */
bool engineCheckGameFolder(const std::string& folder, std::string* problem, std::string* ebootPath = nullptr);

/* 1.10 builds: the installed 1.10 update (RPCS3's or a PS3's
 * dev_hdd0\game\<title id>). A picked USRDIR folder becomes the folder above
 * it; trailing separators go. */
std::string engineNormaliseUpdateFolder(const std::string& folder);

/* True when `folder` holds the 1.10 update of this build's release: its
 * PARAM.SFO and the EBOOT this build was made from. Otherwise false, with a
 * sentence for the user in *problem. `ebootPath` as above: USRDIR\EBOOT.elf,
 * or USRDIR\EBOOT.BIN decrypted (the North American update needs its
 * licence, looked for in RPCS3's dev_hdd0\home\<user>\exdata beside the
 * update and in a "licences" folder beside the port's data). */
bool engineCheckUpdateFolder(const std::string& folder, std::string* problem, std::string* ebootPath = nullptr);

/* Show a folder picker (blocking; any thread). Returns the normalised folder,
 * or an empty string when cancelled. forUpdate: the 1.10 update folder
 * rather than the disc dump. */
std::string enginePickGameFolder(const std::string& startFolder, bool forUpdate = false);

/* The same picker with any title; the folder as picked (trailing separators
 * removed), or an empty string when cancelled. */
std::string enginePickFolder(const std::string& startFolder, const std::string& title);

/* Where hdd0, hdd1 and logs live: the data_dir setting, else beside the exe. */
std::string engineDataDirectory();
