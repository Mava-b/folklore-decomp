/*
 * engine_portable.h -- finding the game files when folklore.exe is started on
 * its own (double-clicked, or from a shared folder) rather than by run.ps1.
 */
#pragma once

#include <string>

/* The folder the user means: a picked PS3_GAME or USRDIR folder becomes the
 * folder above PS3_GAME; trailing separators go. */
std::string engineNormaliseGameFolder(const std::string& folder);

/* True when `folder` holds PS3_GAME\USRDIR\EBOOT.elf, decrypted, from the
 * North American release this port was built from. Otherwise false, with a
 * sentence for the user in *problem. */
bool engineCheckGameFolder(const std::string& folder, std::string* problem);

/* Show a folder picker (blocking; any thread). Returns the normalised folder,
 * or an empty string when cancelled. */
std::string enginePickGameFolder(const std::string& startFolder);

/* Where hdd0, hdd1 and logs live: the data_dir setting, else beside the exe. */
std::string engineDataDirectory();
