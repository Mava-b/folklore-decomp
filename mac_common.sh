# mac_common.sh -- shared by setup_mac.sh and run_mac.sh (sourced, zsh).
export PATH=/opt/homebrew/bin:/usr/local/bin:$PATH
SETTINGS=$ROOT/folklore_mac.env       # FOLKLORE_GAME=<.iso or folder>
VENV=$ROOT/tools/venv-mac
BUILD=$ROOT/folklore/build-mac
DISC_MOUNT=$ROOT/gamedisc             # where a disc image is mounted (read only)

step() { print -P "%F{cyan}==>%f $*"; }
die()  { print -u2 -P "%F{red}error:%f $*"; exit 1; }

# The folder that contains PS3_GAME: the saved folder itself, or the disc
# image mounted read only at gamedisc/ (left mounted for the next run).
mount_game() {
    if [ -d "$FOLKLORE_GAME" ]; then
        print -r -- "$FOLKLORE_GAME"
        return
    fi
    if [ ! -d "$DISC_MOUNT/PS3_GAME" ]; then
        mkdir -p "$DISC_MOUNT"
        hdiutil attach -readonly -nobrowse -noverify -mountpoint "$DISC_MOUNT" "$FOLKLORE_GAME" >/dev/null 2>&1 \
            || die "could not mount $FOLKLORE_GAME"
    fi
    [ -d "$DISC_MOUNT/PS3_GAME" ] || die "no PS3_GAME in $FOLKLORE_GAME"
    print -r -- "$DISC_MOUNT"
}

# decrypted/<first 16 hex of the EBOOT.BIN's SHA-256>/EBOOT.elf, as setup.ps1 names it.
elf_for() {
    local hash=$(shasum -a 256 "$1" | cut -c1-16 | tr a-f A-F)
    print -r -- "$ROOT/decrypted/$hash/EBOOT.elf"
}
