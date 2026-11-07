#!/bin/zsh
# run_mac.sh -- start Folklore on macOS (after ./setup_mac.sh). The counterpart
# of run.ps1: saves in hdd0/ and hdd1/ beside this script, the settings in
# folklore_settings.ini, the log in logs/run-mac.log.
#
# Extra arguments are environment overrides, e.g.:
#   ./run_mac.sh PS3_VERBOSE=1                 the full runtime log
#   ./run_mac.sh PS3_AA=smaa PS3_ANISO=16      SMAA and 16x anisotropic filtering
#   ./run_mac.sh PS3RECOMP_METAL_FRAME_DUMP=/tmp/f.ppm   a frame every 120 to a file
set -e
ROOT=${0:A:h}
source "$ROOT/mac_common.sh"
[ -f "$SETTINGS" ] || die "run ./setup_mac.sh <your Folklore .iso or folder> first"
source "$SETTINGS"
EXE=$BUILD/folklore
[ -x "$EXE" ] || die "not built yet: ./setup_mac.sh"

game_dir=$(mount_game)
elf=$(elf_for "$game_dir/PS3_GAME/USRDIR/EBOOT.BIN")
[ -f "$elf" ] || die "EBOOT not decrypted yet: ./setup_mac.sh"

mkdir -p "$ROOT/hdd0/game" "$ROOT/hdd0/home/00000001/savedata" "$ROOT/hdd1/cache" \
         "$ROOT/logs" "$BUILD/msl_cache"
export PS3_VFS_ROOT=$game_dir
export PS3_HDD0_ROOT=$ROOT/hdd0
export PS3_HDD1_ROOT=$ROOT/hdd1
export PS3_TITLE=Folklore
export FOLKLORE_SETTINGS=$ROOT/folklore_settings.ini
export RSX_MSL_CACHE=$BUILD/msl_cache
# The switches run.ps1 sets for every run.
export PS3_SPURS_JOBCTX_SBUF70=1
export SPU_INTERP_UNLIFTED=1
export GCM_SEMA_ACQUIRE=1
# Non-blocking cellGcmSetWaitFlip (patches 0025/0057): without it every frame
# waits out the vblank, and dialogue openings stall for several frames.
export GCM_WAITFLIP_DEPTH=1
export FOLKLORE_SPURS=native
export PS3_VERBOSE=${PS3_VERBOSE:-0}
# Homebrew's FFmpeg, loaded at run time for the movies.
export DYLD_FALLBACK_LIBRARY_PATH=/opt/homebrew/lib:/usr/local/lib${DYLD_FALLBACK_LIBRARY_PATH:+:$DYLD_FALLBACK_LIBRARY_PATH}

cd "$ROOT"
step "Folklore (log: logs/run-mac.log)"
env "$@" "$EXE" "$elf" > "$ROOT/logs/run-mac.log" 2>&1
