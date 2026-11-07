#!/bin/zsh
# setup_mac.sh -- build Folklore for macOS (Apple silicon, Metal) from your own
# disc. The macOS counterpart of setup.ps1 + build.ps1.
#
#   ./setup_mac.sh <game>     first time: <game> is your disc image (.iso) or a
#                             dumped folder that contains PS3_GAME
#   ./setup_mac.sh            again later: rebuilds with the game saved before
#
# Steps (each skipped when already done):
#   1. Homebrew packages: cmake ninja sdl2 glslang spirv-cross ffmpeg
#   2. a Python environment for the lifter (tools/venv-mac)
#   3. decrypt PS3_GAME/USRDIR/EBOOT.BIN with the port's own decrypter
#      (folklore/tools/unself_main.cpp) into decrypted/<hash>/EBOOT.elf
#   4. lift the game code (folklore/tools/relift.sh)
#   5. promote guest registers (folklore/tools/promote_regs.py) and build
#      folklore/build-mac/folklore
#
#   JOBS=n         parallel compiles (default 4)
#   NO_PROMOTE=1   build the lifted code as is (slower game, simpler build)
#   RELIFT=1       lift again even if folklore/src/recomp is there
set -e
ROOT=${0:A:h}
source "$ROOT/mac_common.sh"

if [ -n "$1" ]; then
    game=${1:A}
    [ -e "$game" ] || die "not found: $game"
    print -r -- "FOLKLORE_GAME=${(q)game}" > "$SETTINGS"
    step "game saved in folklore_mac.env: $game"
fi
[ -f "$SETTINGS" ] || die "first run: ./setup_mac.sh <your Folklore .iso, or the folder that contains PS3_GAME>"
source "$SETTINGS"

# ---- 1. Homebrew --------------------------------------------------------
step "Homebrew packages"
command -v brew >/dev/null || die "Homebrew is needed: https://brew.sh"
missing=()
for pkg in cmake ninja sdl2 glslang spirv-cross ffmpeg; do
    brew list --versions $pkg >/dev/null 2>&1 || missing+=$pkg
done
if (( ${#missing} )); then
    echo "    installing: $missing"
    brew install $missing
fi

# ---- 2. Python ----------------------------------------------------------
step "Python environment (tools/venv-mac)"
if [ ! -x "$VENV/bin/python" ]; then
    py=""
    for c in python3.14 python3.13 python3.12 python3.11 python3.10 python3; do
        command -v $c >/dev/null && { py=$(command -v $c); break; }
    done
    [ -n "$py" ] || die "no python3 found (brew install python)"
    mkdir -p "$ROOT/tools"
    "$py" -m venv "$VENV"
fi
"$VENV/bin/python" -m pip install --quiet --disable-pip-version-check -r "$ROOT/ps3recomp/requirements.txt"

# ---- 3. EBOOT -----------------------------------------------------------
game_dir=$(mount_game)
step "Game files: $game_dir"
eboot_bin="$game_dir/PS3_GAME/USRDIR/EBOOT.BIN"
[ -f "$eboot_bin" ] || die "no PS3_GAME/USRDIR/EBOOT.BIN in $game_dir"
mkdir -p "$ROOT/tools"
if [ ! -x "$ROOT/tools/unself" ] || [ "$ROOT/folklore/src/engine/engine_self.cpp" -nt "$ROOT/tools/unself" ]; then
    step "Building the EBOOT decrypter (tools/unself)"
    clang++ -std=c++17 -O2 -I "$ROOT/folklore/src/engine" \
        "$ROOT/folklore/tools/unself_main.cpp" "$ROOT/folklore/src/engine/engine_self.cpp" \
        -o "$ROOT/tools/unself"
fi
elf=$(elf_for "$eboot_bin")
if [ ! -f "$elf" ]; then
    step "Decrypting EBOOT.BIN"
    mkdir -p "${elf:h}"
    "$ROOT/tools/unself" "$eboot_bin" "$elf"
fi
size=$(stat -f %z "$elf")
case $size in
    6518936) echo "    BCUS98147 (North America) v1.00" ;;
    6518912) echo "    BCES00050 (Europe) v1.00" ;;
    6520504|6520480) echo "    1.10 update" ;;
    *) die "$elf is $size bytes: not a supported Folklore EBOOT (folklore/docs/REGIONS.md)" ;;
esac

# ---- 4. Lift ------------------------------------------------------------
lifted_from="$ROOT/folklore/src/recomp/.lifted_from"
if [ -n "$RELIFT" ] || [ ! -f "$ROOT/folklore/src/recomp/ppu_recomp.h" ] || \
   [ "$(cat "$lifted_from" 2>/dev/null)" != "$elf" ]; then
    step "Lifting the game code (a few minutes)"
    mkdir -p "$ROOT/logs"
    ( cd "$ROOT/folklore" && KEEP_GEN=1 PS3RECOMP="$ROOT/ps3recomp" EBOOT="$elf" PYTHON="$VENV/bin/python" \
        bash tools/relift.sh ) > "$ROOT/logs/relift-mac.log" 2>&1 || die "lift failed -- see logs/relift-mac.log"
    # relift.sh empties src/recomp, including its tracked README.
    git -C "$ROOT" checkout -- folklore/src/recomp/README.md 2>/dev/null || true
    print -r -- "$elf" > "$lifted_from"
fi

# ---- 5. Build -----------------------------------------------------------
recomp_dir=""
if [ -z "$NO_PROMOTE" ]; then
    step "Promoting guest registers"
    recomp_dir="$BUILD/recomp_promoted"
    "$VENV/bin/python" -I "$ROOT/folklore/tools/promote_regs.py" --dir "$ROOT/folklore/src/recomp" "$recomp_dir" \
        > "$ROOT/logs/promote-mac.log" 2>&1 || die "register promotion failed -- see logs/promote-mac.log"
fi
step "Building (folklore/build-mac)"
cmake -S "$ROOT/folklore" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DPS3RECOMP_DIR="$ROOT/ps3recomp" -DFOLKLORE_RECOMP_DIR="$recomp_dir" > "$ROOT/logs/configure-mac.log" 2>&1 \
    || die "CMake failed -- see logs/configure-mac.log"
ninja -C "$BUILD" -j${JOBS:-4} folklore > "$ROOT/logs/build-mac.log" 2>&1 || {
    grep -E "error:|FAILED" "$ROOT/logs/build-mac.log" | head -20
    die "build failed -- see logs/build-mac.log"
}
step "Done: ./run_mac.sh starts the game"
