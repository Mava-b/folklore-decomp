# Folklore (Game Republic / SCE, 2007) -- BCUS98147 or BCES00050, disc v1.00 (docs/REGIONS.md)
# Static recompilation port on the shared ps3recomp runtime.
#
# Layout follows sp00nznet/tornado (Tornado Outbreak), the closest in-game
# reference port: boot harness + loader + HLE dispatch from ps3recomp, the
# lifted PPU tree, the generated NID table and the lifted SPURS jobs.
#
# Windows (reference platform, D3D12 renderer):
#   build.ps1   (from I:\Recomp\Folklore) does all of this for you.
#   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang-cl -DCMAKE_CXX_COMPILER=clang-cl
#   cmake --build build
#
# Linux (compile/link check only; ps3recomp does not boot titles on POSIX yet):
#   cmake -S . -B build -G Ninja -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++
cmake_minimum_required(VERSION 3.20)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "build type" FORCE)
endif()

project(folklore CXX C)

# clang-cl with no /O flag is /Od, so -O2 is explicit. Lifted code type-puns
# guest memory and must not grow FMAs the PPU did not have.
add_compile_options($<$<CONFIG:Release,RelWithDebInfo>:-O2> -fno-strict-aliasing)
if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    # clang-cl ignores the GCC-style spelling ("unknown argument ignored");
    # pass it through to the clang driver explicitly.
    add_compile_options(/clang:-ffp-contract=off)
else()
    add_compile_options(-ffp-contract=off)
endif()
# CPU level the lifted code is compiled for. x86-64-v3 (Intel Haswell / AMD
# Zen and newer: AVX2, FMA, BMI2, MOVBE) turns every VMX multiply-add into one
# vfmadd instead of a libm fmaf call, byte-swapped loads into movbe, and lets
# ppu_vmx.h use its SIMD paths. -ffp-contract=off above still keeps the
# compiler from fusing anything the PPU code did not fuse. Set it to empty
# (-DFOLKLORE_CPU_LEVEL=) for a build that runs on any x86-64 CPU.
set(FOLKLORE_CPU_LEVEL "x86-64-v3" CACHE STRING "-march level for the build (empty = baseline x86-64)")
if(FOLKLORE_CPU_LEVEL)
    if(CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
        add_compile_options(/clang:-march=${FOLKLORE_CPU_LEVEL})
    else()
        add_compile_options(-march=${FOLKLORE_CPU_LEVEL})
    endif()
endif()
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_C_STANDARD 17)

get_filename_component(DEFAULT_PS3RECOMP_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ps3recomp" ABSOLUTE)
set(PS3RECOMP_DIR "${DEFAULT_PS3RECOMP_DIR}" CACHE PATH "ps3recomp repo root")

if(WIN32)
    set(DEFAULT_RUNTIME_LIB "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib")
else()
    set(DEFAULT_RUNTIME_LIB "${PS3RECOMP_DIR}/build-linux/libps3recomp_runtime.a")
endif()
set(PS3RECOMP_RUNTIME_LIB "${DEFAULT_RUNTIME_LIB}" CACHE FILEPATH "prebuilt ps3recomp runtime library")

if(NOT EXISTS "${PS3RECOMP_RUNTIME_LIB}")
    message(FATAL_ERROR "ps3recomp runtime not found at ${PS3RECOMP_RUNTIME_LIB} -- build it first (setup.ps1 does)")
endif()

# CONFIGURE_DEPENDS: a build re-checks these folders, so files added by a
# relift (a new job placement, say) are picked up without a reconfigure.
# FOLKLORE_RECOMP_DIR: which copy of the lifted PPU code to compile.
# build.ps1 points it at build/recomp_promoted, the output of
# tools/promote_regs.py (guest registers kept in C locals); empty or unset
# builds src/recomp as lifted. ppu_recomp.h always comes from src/recomp.
set(FOLKLORE_RECOMP_DIR "" CACHE PATH "lifted PPU sources to compile (empty = src/recomp)")
if(FOLKLORE_RECOMP_DIR)
    file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "${FOLKLORE_RECOMP_DIR}/*.cpp")
else()
    file(GLOB RECOMP_SOURCES CONFIGURE_DEPENDS "src/recomp/*.cpp")
endif()
file(GLOB GEN_SOURCES CONFIGURE_DEPENDS    "src/gen/*.cpp" "src/gen/*.c")
file(GLOB_RECURSE SPU_SOURCES CONFIGURE_DEPENDS "src/spu_gen/*.c")
file(GLOB HLE_SOURCES CONFIGURE_DEPENDS    "src/hle/*.cpp" "src/hle/*.c")
# The port's engine menu (L3+R3 / F1) and its settings; the menu itself is
# Windows-only and draws with Dear ImGui (src/third_party/imgui, MIT).
file(GLOB ENGINE_SOURCES CONFIGURE_DEPENDS "src/engine/*.cpp")
set(IMGUI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/third_party/imgui")
set(IMGUI_SOURCES)
if(WIN32)
    set(IMGUI_SOURCES
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/imgui_draw.cpp"
        "${IMGUI_DIR}/imgui_tables.cpp"
        "${IMGUI_DIR}/imgui_widgets.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_dx12.cpp")
endif()

# Optional, for comparison runs only (run.ps1 -Libsre): Sony's real libsre
# (SPURS), lifted from the USER'S OWN decrypted firmware by
# tools/lift_firmware.sh into firmware_lle/ (setup.ps1 -LibsrePrx). Never
# shipped. The game runs on the port's own SPURS (src/hle/spurs_native*,
# docs/SPURS.md) either way.
set(FIRMWARE_LLE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/firmware_lle")
set(LIBSRE_SOURCES)
if(EXISTS "${FIRMWARE_LLE_DIR}/libsre_recomp.cpp" AND EXISTS "${FIRMWARE_LLE_DIR}/libsre_exports.c")
    set(LIBSRE_SOURCES
        "${FIRMWARE_LLE_DIR}/libsre_recomp.cpp"
        "${FIRMWARE_LLE_DIR}/libsre_exports.c"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/prx/libsre_load.cpp")
    # SPU side: the SPURS kernel, system service and job-chain policy module.
    file(GLOB_RECURSE LIBSRE_SPU_SOURCES CONFIGURE_DEPENDS "${FIRMWARE_LLE_DIR}/spu/*.c")
    list(APPEND LIBSRE_SOURCES ${LIBSRE_SPU_SOURCES})
    # Lifted module code is C++ like the title's own; the export table too, so
    # its extern "C" block links against libsre_load.cpp.
    set_source_files_properties("${FIRMWARE_LLE_DIR}/libsre_exports.c" PROPERTIES LANGUAGE CXX)
    message(STATUS "SPURS: the port's own, plus Sony's libsre from firmware_lle for run.ps1 -Libsre")
else()
    message(STATUS "SPURS: the port's own (src/hle/spurs_native*); no firmware_lle")
endif()
# The game's jobs lifted where the job manager loads them
# (tools/relift_lle_jobs.sh). Both SPURS implementations run them: Sony's
# libsre above, and the port's own (src/hle/spurs_native*.c*), which is all a
# build without firmware_lle has.
file(GLOB_RECURSE JOBS_LLE_SOURCES CONFIGURE_DEPENDS "src/spu_gen_lle/*.c")
list(APPEND SPU_SOURCES ${JOBS_LLE_SOURCES})
if(NOT RECOMP_SOURCES)
    message(FATAL_ERROR "src/recomp/ is empty -- run tools/relift.sh first")
endif()

add_executable(folklore
    "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp"   # generic boot harness
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_loader.cpp"        # vm + ELF loader + run
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_hle.cpp"           # NID -> handler dispatch
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_sysprx.cpp"        # boot-critical CRT
    "${PS3RECOMP_DIR}/runtime/ppu/ppu_fs.cpp"            # cellFs VFS
    ${GEN_SOURCES}
    ${HLE_SOURCES}
    ${ENGINE_SOURCES}
    ${IMGUI_SOURCES}
    ${LIBSRE_SOURCES}
    ${SPU_SOURCES}
    ${RECOMP_SOURCES}
)

file(GLOB LIB_INC_DIRS CONFIGURE_DEPENDS LIST_DIRECTORIES true "${PS3RECOMP_DIR}/libs/*")
target_include_directories(folklore PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"
    "${PS3RECOMP_DIR}/runtime/ppu"
    "${PS3RECOMP_DIR}/runtime/spu"
    "${PS3RECOMP_DIR}/runtime/prx"
    "${PS3RECOMP_DIR}/include"
    ${LIB_INC_DIRS}
    "${CMAKE_CURRENT_SOURCE_DIR}/src/engine"
    "${IMGUI_DIR}"
)
if(WIN32)
    # POSIX shims (dirent.h / unistd.h) the boot harness needs under clang-cl.
    target_include_directories(folklore PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/compat")
endif()

if(LIBSRE_SOURCES)
    target_compile_definitions(folklore PRIVATE
        FOLKLORE_LIBSRE_IMAGE="${FIRMWARE_LLE_DIR}/libsre_image.bin")
    set_source_files_properties("${FIRMWARE_LLE_DIR}/libsre_recomp.cpp" PROPERTIES
        INCLUDE_DIRECTORIES "${FIRMWARE_LLE_DIR}")
    # src/prx/libsre_load.cpp defines ps3_load_prx_modules(); drop the boot
    # harness's weak default rather than rely on weak symbols under link.exe.
    set_source_files_properties("${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp" PROPERTIES
        COMPILE_DEFINITIONS PS3_HAVE_PRX_MODULES)
endif()
# The boot harness asks src/engine/engine_portable.cpp for the EBOOT and the
# default environment first (patches/0046), so folklore.exe runs on its own.
if(WIN32)
    set_property(SOURCE "${PS3RECOMP_DIR}/runtime/ppu/tests/boot_main.cpp" APPEND PROPERTY
        COMPILE_DEFINITIONS PS3_HAVE_HOST_BOOTSTRAP)
endif()

target_link_libraries(folklore PRIVATE "${PS3RECOMP_RUNTIME_LIB}")
if(WIN32)
    target_link_libraries(folklore PRIVATE ws2_32 bcrypt xinput ole32 d3d12 dxgi dxguid d3dcompiler)
    # Every guest call is a host call, so recompiled call chains nest deeply.
    target_link_options(folklore PRIVATE /STACK:268435456 /MAP:folklore.map)
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(folklore PRIVATE SDL2::SDL2 Threads::Threads ${CMAKE_DL_LIBS} m)
endif()
