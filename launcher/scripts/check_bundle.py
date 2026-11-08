#!/usr/bin/env python3
"""Fail when a launcher bundle carries anything from the game.

    python3 launcher/scripts/check_bundle.py <bundle.app | .dmg | folder> ...

The launcher ships no game code or data: it finds the player's own disc and
builds at runtime. This walks each bundle (a .dmg is attached read-only
first) and rejects, by name: executables and archives of the game (EBOOT*,
*.self, *.sprx, *.prx, *.elf, *.pkg, *.psarc, *.pup, *.iso, PARAM.SFO,
BCUS98147*, BCES00050*), keys and licences (*.rap, *.rif, act.dat, *.edat), the game's
art and sound (ICON0*, PIC0*, PIC1*, PIC2*, SND0*), saves (*.sav), lifted
output (recompiled/, jobmods/, spu_recomp.*); by content: SCE/PSF/PKG/PSAR
headers and PowerPC or SPU ELF files; and any file over 16 MB.
Standard library only.
"""
import fnmatch, os, subprocess, sys, tempfile

NAMES = ["eboot*", "*.self", "*.sprx", "*.prx", "*.elf", "*.pkg", "*.psarc", "*.pup", "*.iso",
         "param.sfo", "bcus98147*", "bces00050*", "*.rap", "*.rif", "act.dat", "*.edat", "icon0*", "pic0*",
         "pic1*", "pic2*", "snd0*", "*.sav", "spu_recomp.*", "ppu_recomp*"]
DIRS = ["recompiled", "jobmods", "dev_hdd0", "dev_flash", "ps3_game", "usrdir"]
MAGIC = [b"SCE\0", b"\0PSF", b"\x7fPKG", b"PSAR"]
EM_PPC64, EM_SPU = 21, 23
MAX = 16 << 20


def problems(root):
    for d, dirs, files in os.walk(root):
        for n in dirs:
            if n.lower() in DIRS:
                yield os.path.join(d, n), "folder name"
        for n in files:
            p = os.path.join(d, n)
            low = n.lower()
            if any(fnmatch.fnmatch(low, pat) for pat in NAMES):
                yield p, "file name"
                continue
            try:
                size = os.path.getsize(p)
                with open(p, "rb") as f:
                    head = f.read(64)
            except OSError:
                continue
            if size > MAX:
                yield p, f"{size >> 20} MB"
            if any(head.startswith(m) for m in MAGIC):
                yield p, "PS3 file header"
            if head.startswith(b"\x7fELF") and len(head) >= 20:
                be = head[5] == 2
                em = int.from_bytes(head[18:20], "big" if be else "little")
                if em in (EM_PPC64, EM_SPU):
                    yield p, "PowerPC/SPU ELF"


def check(path):
    if path.endswith(".dmg"):
        mnt = tempfile.mkdtemp()
        subprocess.run(["hdiutil", "attach", "-readonly", "-nobrowse", "-mountpoint", mnt, path], check=True, capture_output=True)
        try:
            return list(problems(mnt))
        finally:
            subprocess.run(["hdiutil", "detach", mnt], capture_output=True)
    return list(problems(path))


def main(args):
    if not args:
        print(__doc__)
        return 2
    bad = 0
    for a in args:
        found = check(a)
        for p, why in found:
            print(f"REJECT {p}: {why}")
        bad += len(found)
        print(f"{a}: {'clean' if not found else f'{len(found)} problem(s)'}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
