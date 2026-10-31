/*
 * engine_self.h -- decrypting the game's EBOOT.BIN (a PS3 SELF) to the ELF
 * this port loads and lifts, so nobody needs RPCS3's "Decrypt PS3 Binaries".
 *
 * Handles the two kinds Folklore ships: the disc's EBOOT.BIN (an application
 * SELF) and the 1.10 update's (an NPDRM SELF: free licence for the European
 * update, a local licence -- a .rap -- for the North American one). The
 * output is byte for byte what RPCS3 writes. folklore/docs/SELF.md has the
 * format and where the keys come from.
 *
 * Used by the engine (engine_portable.cpp: a dump or update folder with only
 * EBOOT.BIN) and by tools/unself_main.cpp, the command-line tool setup.ps1
 * builds and runs before lifting.
 */
#pragma once

#include <string>
#include <vector>

/* Decrypt the SELF at `selfPath` and write the ELF to `elfPath`. Licences
 * (.rap, named by content ID) are looked for in `licenceFolders`, in order.
 * On failure returns false with a sentence for the user in `problem`. */
bool selfDecryptFile(const std::string& selfPath, const std::string& elfPath,
                     const std::vector<std::string>& licenceFolders, std::string* problem);

/* The same, in memory. */
bool selfDecrypt(const std::vector<unsigned char>& self, std::vector<unsigned char>* elf,
                 const std::vector<std::string>& licenceFolders, std::string* problem);

/* True when the file starts with the SELF magic ("SCE\0"). */
bool selfIsSelfFile(const std::string& path);
