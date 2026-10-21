# patches/

The changes this port made to [ps3recomp](https://github.com/sp00nznet/ps3recomp)
while bringing Folklore up, one fix per patch, in the order they apply on top
of upstream commit `39a36296511c445707adabcd312f0b740cfbd694`.

The `ps3recomp/` folder in this repository already has all of them applied,
so setup.ps1 uses it as it is. The patches are kept as a record of what
changed and why (each one's code carries comments explaining the fix), and so
they can be sent upstream. `folklore/docs/STATUS.md` tells the story behind
most of them.

If `ps3recomp/` is instead a git clone of upstream (an older working folder),
setup.ps1 resets it to the pinned commit and applies these patches in order.
