# Folklore (Game Republic / SCE, 2007) -- BCUS98147 or BCES00050, disc v1.00 or the 1.10 update (docs/REGIONS.md)
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
# macOS (Apple silicon, Metal renderer):
#   ../setup_mac.sh does all of this for you; the runtime is built as part of
#   this project (add_subdirectory) so its Metal/SDL2/shader libraries link.
#
# Linux (compile/link check only):
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
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64|ARM64)$")
    # Apple silicon / arm64: no x86 -march level (ppu_vmx.h uses its portable path).
    set(_folklore_default_cpu_level "")
else()
    set(_folklore_default_cpu_level "x86-64-v3")
endif()
set(FOLKLORE_CPU_LEVEL "${_folklore_default_cpu_level}" CACHE STRING "-march level for the build (empty = baseline x86-64)")
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

if(APPLE)
    # macOS: build the runtime as part of this project, so its Metal, SDL2,
    # glslang and spirv-cross link requirements come with the target.
    # FFmpeg (Homebrew) is loaded at run time for the cutscenes.
    if(NOT DEFINED PS3RECOMP_FFMPEG_RUNTIME AND EXISTS "/opt/homebrew/include/libavcodec/avcodec.h")
        set(PS3RECOMP_FFMPEG_RUNTIME ON CACHE BOOL "" FORCE)
        set(PS3RECOMP_FFMPEG_INCLUDE_DIR "/opt/homebrew/include" CACHE PATH "" FORCE)
    endif()
    add_subdirectory("${PS3RECOMP_DIR}" "${CMAKE_BINARY_DIR}/ps3recomp" EXCLUDE_FROM_ALL)
    set(PS3RECOMP_RUNTIME_LIB ps3recomp_runtime)
else()
if(WIN32)
    set(DEFAULT_RUNTIME_LIB "${PS3RECOMP_DIR}/build/ps3recomp_runtime.lib")
else()
    set(DEFAULT_RUNTIME_LIB "${PS3RECOMP_DIR}/build-linux/libps3recomp_runtime.a")
endif()
set(PS3RECOMP_RUNTIME_LIB "${DEFAULT_RUNTIME_LIB}" CACHE FILEPATH "prebuilt ps3recomp runtime library")

if(NOT EXISTS "${PS3RECOMP_RUNTIME_LIB}")
    message(FATAL_ERROR "ps3recomp runtime not found at ${PS3RECOMP_RUNTIME_LIB} -- build it first (setup.ps1 does)")
endif()
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
elseif(APPLE)
    # SDL2, pthreads and the Metal frameworks come with ps3recomp_runtime.
    target_link_libraries(folklore PRIVATE "-framework CoreGraphics")
    # The guest's main thread runs on the process's main thread, and every
    # guest call is a host call: give it the same 256 MB Windows gets (/STACK).
    target_link_options(folklore PRIVATE "LINKER:-stack_size,0x10000000")
    # An app bundle, so macOS treats the game as a game (Game Mode in
    # fullscreen: CPU/GPU priority, half the Bluetooth latency for controllers;
    # macos/Info.plist.in). build-mac/folklore links to the bundle's
    # executable, so run_mac.sh and the launcher start it directly and keep the
    # environment and the log. The icon is your disc's ICON0.PNG, which
    # setup_mac.sh copies to build-mac/icon0.png (never distributed).
    set(FOLKLORE_ICON_PNG "${CMAKE_BINARY_DIR}/icon0.png")
    set(FOLKLORE_ICNS_NAME "")
    if(EXISTS "${FOLKLORE_ICON_PNG}")
        set(FOLKLORE_ICNS "${CMAKE_BINARY_DIR}/Folklore.icns")
        # ICON0.PNG is 320x176 and an .icns wants a square: pad, scale, convert.
        set(FOLKLORE_ICON_SQUARE "${CMAKE_BINARY_DIR}/icon0_square.png")
        add_custom_command(OUTPUT "${FOLKLORE_ICNS}"
            COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/macos/make_icns.sh" "${FOLKLORE_ICON_PNG}" "${FOLKLORE_ICON_SQUARE}" "${FOLKLORE_ICNS}"
            DEPENDS "${FOLKLORE_ICON_PNG}" "${CMAKE_CURRENT_SOURCE_DIR}/macos/make_icns.sh"
            COMMENT "Making the bundle icon")
        target_sources(folklore PRIVATE "${FOLKLORE_ICNS}")
        set_source_files_properties("${FOLKLORE_ICNS}" PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
        set(FOLKLORE_ICNS_NAME "Folklore.icns")
    endif()
    set_target_properties(folklore PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "io.github.erikomis.folklorerecomp.game"
        MACOSX_BUNDLE_BUNDLE_NAME "Folklore"
        MACOSX_BUNDLE_EXECUTABLE_NAME "folklore"
        MACOSX_BUNDLE_ICON_FILE "${FOLKLORE_ICNS_NAME}"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "0.5"
        MACOSX_BUNDLE_BUNDLE_VERSION "0.5")
    # The window the player sees (ps3recomp's tools/host_window, from the
    # Resistance / inFAMOUS 2 ports): it runs the game with
    # PS3RECOMP_HOST_SURFACES set, the game renders into IOSurfaces it shares,
    # and this process presents them -- with MetalFX upscaling, macOS
    # fullscreen / borderless / windowed, a display of choice and vsync
    # (PS3_UPSCALE, PS3_WINDOW_MODE, PS3_WINDOW_SIZE, PS3_DISPLAY, PS3_VSYNC).
    # It sits inside folklore.app, so it is the game for Game Mode too.
    # run_mac.sh starts the game through it unless FOLKLORE_HOST_WINDOW=0.
    enable_language(OBJC)
    add_executable(folklore-window "${PS3RECOMP_DIR}/tools/host_window/host_window.m")
    target_include_directories(folklore-window PRIVATE "${PS3RECOMP_DIR}/libs/video")
    target_compile_definitions(folklore-window PRIVATE HOST_WINDOW_TITLE="Folklore")
    target_compile_options(folklore-window PRIVATE -fobjc-arc)
    target_link_libraries(folklore-window PRIVATE
        "-framework AppKit" "-framework Metal" "-framework QuartzCore" "-framework IOSurface")
    set_target_properties(folklore-window PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/folklore.app/Contents/MacOS")
    add_dependencies(folklore folklore-window)
    add_custom_command(TARGET folklore POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E rm -f "${CMAKE_BINARY_DIR}/folklore"
        COMMAND ${CMAKE_COMMAND} -E create_symlink "folklore.app/Contents/MacOS/folklore"
                "${CMAKE_BINARY_DIR}/folklore"
        COMMENT "build-mac/folklore -> folklore.app/Contents/MacOS/folklore")
else()
    find_package(SDL2 REQUIRED)
    find_package(Threads REQUIRED)
    target_link_libraries(folklore PRIVATE SDL2::SDL2 Threads::Threads ${CMAKE_DL_LIBS} m)
endif()

# FolkloreLauncher.exe (src/launcher): the game's folders, DLC and settings
# in one window, then Play starts folklore.exe beside it. It shares the
# settings, folder checks and DLC importer with the game (src/engine), and
# draws with Dear ImGui's Win32 + Direct3D 11 backends.
if(WIN32)
    add_executable(FolkloreLauncher WIN32
        "src/launcher/launcher_main.cpp"
        "src/launcher/launcher_stubs.cpp"
        "src/engine/engine_settings.cpp"
        "src/engine/engine_portable.cpp"
        "src/engine/engine_dlc.cpp"
        "src/engine/engine_self.cpp"
        "src/engine/engine_save_slots.cpp"
        "${PS3RECOMP_DIR}/libs/filesystem/edat.c"
        "${IMGUI_DIR}/imgui.cpp"
        "${IMGUI_DIR}/imgui_draw.cpp"
        "${IMGUI_DIR}/imgui_tables.cpp"
        "${IMGUI_DIR}/imgui_widgets.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_win32.cpp"
        "${IMGUI_DIR}/backends/imgui_impl_dx11.cpp")
    if(FOLKLORE_RECOMP_DIR)
        set(LAUNCHER_REGION_DIR "${FOLKLORE_RECOMP_DIR}")
    else()
        set(LAUNCHER_REGION_DIR "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp")
    endif()
    target_include_directories(FolkloreLauncher PRIVATE
        "${LAUNCHER_REGION_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/recomp"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/engine"
        "${IMGUI_DIR}"
        "${IMGUI_DIR}/backends"
        "${PS3RECOMP_DIR}/libs/filesystem"
        "${PS3RECOMP_DIR}/libs/codec"
        "${PS3RECOMP_DIR}/include")
    target_link_libraries(FolkloreLauncher PRIVATE d3d11 dxgi dwmapi ole32 shell32 uuid bcrypt)
endif()

# The icon of folklore.exe and FolkloreLauncher.exe: folklore_icon.ico in the
# project folder (beside run.ps1), when there is one. Like launcher_art.jpg
# it is made from the game's own art, so it is not in the repository; without
# it both exes get Windows' default icon. Both windows use resource 1.
set(FOLKLORE_ICON_FILE "${CMAKE_CURRENT_SOURCE_DIR}/../folklore_icon.ico")
if(WIN32 AND EXISTS "${FOLKLORE_ICON_FILE}")
    enable_language(RC)
    get_filename_component(FOLKLORE_ICON_FULL "${FOLKLORE_ICON_FILE}" ABSOLUTE)
    file(TO_NATIVE_PATH "${FOLKLORE_ICON_FULL}" FOLKLORE_ICON_NATIVE)
    string(REPLACE "\\" "\\\\" FOLKLORE_ICON_ESCAPED "${FOLKLORE_ICON_NATIVE}")
    set(FOLKLORE_ICON_RC "${CMAKE_CURRENT_BINARY_DIR}/folklore_icon.rc")
    file(CONFIGURE OUTPUT "${FOLKLORE_ICON_RC}" CONTENT "1 ICON \"${FOLKLORE_ICON_ESCAPED}\"\n")
    target_sources(folklore PRIVATE "${FOLKLORE_ICON_RC}")
    target_sources(FolkloreLauncher PRIVATE "${FOLKLORE_ICON_RC}")
    set_property(SOURCE "${FOLKLORE_ICON_RC}" APPEND PROPERTY OBJECT_DEPENDS "${FOLKLORE_ICON_FULL}")
    message(STATUS "Icon: ${FOLKLORE_ICON_FULL}")
endif()
