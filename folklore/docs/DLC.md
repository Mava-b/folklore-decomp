# Downloadable content

Folklore's add-on packs (new Folks, quests and costumes, sold on the
PlayStation Store from late 2007) work in this port from your own purchase.
The engine menu (F1) has an **Add DLC from RPCS3...** button that brings them
in.

## Using it

1. Install each pack in RPCS3 — **File → Install Packages/Raps/Edats**, select
   the pack's `.pkg` and its `.rap` licence — or just use an RPCS3 that
   already has them. (You do not need to start the game in RPCS3.)
2. In the port, open the engine menu (F1 or L3+R3), go to **Downloadable
   content** and press **Add DLC from RPCS3...**.
3. Choose RPCS3's own folder (the one with `dev_hdd0` in it). Its
   `dev_hdd0`, `dev_hdd0\game` or `dev_hdd0\game\BCUS98147` (`BCES00050`)
   work too.
4. A progress bar shows the import (about half a minute for all six paid
   packs); then the menu lists what was installed, and what was not and why.
5. Restart the game. The new quests and Folks are there as they were on a
   PS3.

Running it again only adds what is new. Nothing is written to RPCS3's folder.

Licences are looked for in RPCS3's `dev_hdd0\home\<user>\exdata` (where its
*Install Packages/Raps/Edats* puts them), in a `licences` folder beside the
port's `hdd0`, and in the folder you picked. A pack whose licence is missing
— or belongs to another account — is left out whole, with a line saying so;
the others still go in.

## What the packs are

On the North American release (BCUS98147) the game's HDD folder holds these
packs, each in its own numbered folder under `USRDIR\bin`:

| Folder | Pack | Licence |
|---|---|---|
| 10–13, 15, 16 | Add-on packs 1–6 (`UP9000-BCUS98147_00-FOLKLORE20000001`..`06`) | one `.rap` each |
| 17 | the 1.10 update's own data (handled by the 1.10 build, not here) | none |
| 18, 19 | free packs | none |

Each paid pack adds four quests, a Folk and a costume (PlayStation Blog,
December 2007: pack 1 "The Kidnapped Folk", pack 2 "Bottom of the Sea"). The
European release's packs have their own content IDs; the importer reads them
from the files, so the same steps apply.

## How it works

**The game's pack format** (found in the EBOOT; v1.00 `func_00155FA0` and the
loop that calls it):

- A pack is a folder `USRDIR\bin\NN` (`00`–`89`) with a list, `download.dat_`,
  and the files it names. The list — and the game's record of installed files,
  `install.dat_` — is a big-endian file count, then per file its pack number
  (one byte) and its path under `bin` (NUL-terminated).
- On start the game copies each new pack into `bin` and adds its files to
  `install.dat_`; RPCS3 and a PS3 leave the pack folders empty afterwards.
- A file the game finds as `<name>.edat` on the HDD it reads through
  `sceNpDrmIsAvailable` and `cellFsOpen` (`func_0014EC0C`, `func_0014F278`), so
  paid packs' files stay encrypted in `bin`. The klicensee the game passes is
  `GemuripaMCStudio`.

**The paid packs' files** (all 259 of them, ~96 MB) are NPD version 1 EDATs of
licence type 2. The game's klicensee verifies their headers, but the data is
encrypted with the RIF key of the buyer's licence. RPCS3 keeps that licence as
`<content id>.rap`; it becomes the RIF key by the published conversion (one
AES-128 block decryption with the RAP key, then five rounds of a byte
permutation, XOR and subtraction — RPCS3's `rap_to_rif`).

**The import** (`src/engine/engine_dlc.cpp`):

1. Find the game's folder from the picked one, and the licence folders.
2. List the packs: those already installed there (from its `install.dat_`)
   and those still waiting in `bin\NN` (their `download.dat_`, or an encrypted
   `download.edat`, read with the pack's licence). The update's pack 17 is
   skipped.
3. Per pack, all files or none: copy plain files; decrypt encrypted ones with
   ps3recomp's `edat_decrypt_file` (`libs/filesystem/edat.c`), handing it the
   RIF key through its `<file>.rifkey` sidecar on a working copy in the
   port's own folder. It checks every block's hash, so a wrong licence is
   refused, never written. If any file fails, the pack's files written so far
   are removed.
4. The decrypted files keep their `.edat` names: the game opens that name, and
   the runtime opens a file that does not start with the NPD header as it is.
5. Record the packs in the port's `install.dat_` as the game's installer would,
   and their content IDs in `port_dlc_packs.txt` beside it (for the menu's
   names).

The `.rap` conversion is in this project's engine code, not in ps3recomp,
whose `edat.c` deliberately carries no licence conversion; the one AES block
uses Windows' BCrypt.

## Testing it off Windows

`tools/test_dlc_import.cpp` runs the same importer on Linux, with OpenSSL for
the AES block (instructions at its top). On 2026-10-29 it was run against a
copy of an RPCS3 install with all six paid packs and the free ones (a sample
of each pack's files): every pack installed, the decrypted files matched
their EDAT headers' sizes, a missing licence left that pack out, a licence
for the wrong pack was refused with nothing written, and a second import
changed nothing. Decrypting ran at about 3 MB/s.
