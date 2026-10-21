/*
 * libsre_load.cpp -- run Sony's real SPURS library (libsre) instead of HLE.
 *
 * Built only when folklore/firmware_lle/ holds a lift of the user's OWN
 * decrypted libsre.prx (tools/lift_firmware.sh makes it; nothing derived from
 * firmware is shipped with this port). Overrides the boot harness's weak
 * ps3_load_prx_modules(): the relocated image goes into guest RAM at its lift
 * base, every lifted function is registered with the PPU dispatcher, and the
 * module's exports are published by NID -- after which ps3_hle_call routes the
 * game's cellSpurs / cellSync imports into the real library rather than the
 * runtime's HLE (runtime/ppu/ppu_hle.cpp, "Real-PRX bridge").
 *
 * Why: Folklore's SPU jobs depend on the exact job-manager context Sony's jm2
 * builds (see docs/STATUS.md, patches/0005); the real library removes the
 * guesswork for every job at once.
 *
 * FOLKLORE_SPURS=native (or PS3_NO_LIBSRE=1) skips loading: the game's
 * cellSpurs calls then go to the port's own SPURS (src/hle/spurs_native.cpp).
 */
#include "prx_loader.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" const prx_func_entry libsre_function_table[];
extern "C" const uint64_t       libsre_function_table_count;
extern "C" const prx_export     libsre_exports[];
extern "C" const uint32_t       libsre_export_count;

extern "C" void ppu_register_function(uint64_t addr, void (*fn)(void*));

namespace
{
    constexpr uint32_t kLibsreBase = 0x30000000u;   /* must match tools/lift_firmware.sh */

    void registerLifted(uint32_t guestAddress, void (*hostFunction)(void*))
    {
        ppu_register_function(guestAddress, hostFunction);
    }

    std::string imagePath()
    {
        const char* fromEnvironment = std::getenv("PS3_LIBSRE_IMAGE");
        if (fromEnvironment && *fromEnvironment)
        {
            return fromEnvironment;
        }
#ifdef FOLKLORE_LIBSRE_IMAGE
        return FOLKLORE_LIBSRE_IMAGE;
#else
        return "libsre_image.bin";
#endif
    }
}

extern "C" void ps3_load_prx_modules(void)
{
    if (std::getenv("PS3_NO_LIBSRE"))
    {
        std::fprintf(stderr, "[libsre] PS3_NO_LIBSRE set -- using the native SPURS (src/hle/spurs_native.cpp)\n");
        return;
    }
    const char* spursChoice = std::getenv("FOLKLORE_SPURS");
    if (spursChoice && std::strcmp(spursChoice, "native") == 0)
    {
        std::fprintf(stderr, "[libsre] FOLKLORE_SPURS=native -- using the native SPURS (src/hle/spurs_native.cpp)\n");
        return;
    }

    std::string path = imagePath();
    uint32_t imageSize = 0;
    uint8_t* image = prx_image_load_file(path.c_str(), &imageSize);
    if (!image)
    {
        std::fprintf(stderr, "[libsre] image not found at %s -- using HLE cellSpurs\n", path.c_str());
        return;
    }

    prx_module module;
    std::memset(&module, 0, sizeof(module));
    module.name         = "libsre";
    module.base         = kLibsreBase;
    module.image        = image;
    module.image_size   = imageSize;
    module.funcs        = libsre_function_table;
    module.func_count   = libsre_function_table_count;
    module.exports      = libsre_exports;
    module.export_count = libsre_export_count;

    prx_load_result result = prx_load_module(&module, registerLifted);
    std::free(image);
    if (!result.ok)
    {
        std::fprintf(stderr, "[libsre] load FAILED -- cellSpurs falls back to HLE\n");
        return;
    }
    std::fprintf(stderr, "[libsre] real libsre active: %u functions, %u exports\n",
                 result.funcs_registered, prx_export_registry_count());
}
