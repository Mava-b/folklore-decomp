/*
 * unself_main.cpp -- decrypt Folklore's EBOOT.BIN to EBOOT.elf (the engine's
 * src/engine/engine_self.cpp, as a command-line tool). setup.ps1 builds it
 * (tools\unself.exe) and runs it when a dump or update folder has no
 * EBOOT.elf; it can also be used on its own:
 *
 *   unself <EBOOT.BIN> <output .elf> [licence folder]...
 *
 * Licence folders are where a .rap is looked for (the North American 1.10
 * update needs one). Exit code 0 on success; otherwise a sentence on stderr.
 *
 * Off Windows (a check before a build):
 *   g++ -std=c++17 -O2 -I src/engine tools/unself_main.cpp src/engine/engine_self.cpp
 */
#include "engine_self.h"

#include <cstdio>
#include <string>
#include <vector>

/* stb_image's zlib decoder, the inflate engine_self.cpp uses (in the game it
 * comes from the ps3recomp runtime). */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#include "../../ps3recomp/libs/codec/stb_image.h"

int main(int argumentCount, char** arguments)
{
    if (argumentCount < 3)
    {
        std::fprintf(stderr, "usage: %s <EBOOT.BIN> <output .elf> [licence folder]...\n", arguments[0]);
        return 2;
    }
    std::vector<std::string> licenceFolders;
    for (int argumentIndex = 3; argumentIndex < argumentCount; argumentIndex++)
    {
        licenceFolders.push_back(arguments[argumentIndex]);
    }
    std::string problem;
    if (!selfDecryptFile(arguments[1], arguments[2], licenceFolders, &problem))
    {
        std::fprintf(stderr, "%s\n", problem.c_str());
        return 1;
    }
    std::printf("decrypted %s -> %s\n", arguments[1], arguments[2]);
    return 0;
}
