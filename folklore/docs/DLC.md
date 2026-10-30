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

## Removing packs

Each installed pack has a **Remove** button in the menu's *Downloadable
content* section (press it twice to confirm), and there is **Remove all
DLC**. A pack waiting to be removed shows *(removed at the next start)* and a
**Keep** button that cancels it; adding the pack again cancels it too.

- Before asking, the menu copies your saves to a snapshot named
  `<date-time>-before-removing-DLC` (*Save data snapshots*). A save that uses
  a removed pack's Folks, items or quests may not load without it — Folklore
  was never meant to lose DLC it had — so if one does not, restore that
  snapshot or add the pack again.
- The removal happens at the **next start**, before the game opens any file
  (`engineDlcApplyPendingRemovals`, called from `ps3_host_bootstrap`): while
  the game runs it may hold the pack's files open, or have their data
  loaded. The files recorded under the pack in `install.dat_` are deleted —
  the `_uk`/`_en` copies of a cross-release import with them — their lines
  taken out, and folders left empty removed. The menu then shows what was
  removed, once. A file that cannot be deleted stays recorded and is tried
  again at the following start.
- Only files the DLC added are touched: the game's own first-boot files on
  the HDD have other names (`LCm4030.bi5_~`, ...), and pack 17, the 1.10
  update's data, is never offered. If a removed pack had replaced the 1.10
  update's `LMonS2.bi5_` (North American free pack 19 does), the 1.10
  build's start-up puts the update's own copy back, as the record no longer
  says a later pack replaced it.
- When two packs shipped the same file, the record only keeps the later
  pack; removing that one takes the file from the earlier pack too, as on a
  PS3. **Add DLC** again puts back whatever is missing.

The pending list is `port_dlc_remove.txt` beside `install.dat_`, the last
report `port_dlc_removed.txt`.

## Packs from the other release

North American packs work in the European build, and European packs in the
North American one. The importer looks for this build's own release in the
picked folder first (`dev_hdd0\game\BCES00050` for a European build) and
uses the other release's folder only when its own has no packs — an RPCS3
with the European update installed but the North American packs, say. The
menu's report then says whose packs they were.

Why it works: the two executables are the same game code (10 functions
differ, none of them in the pack code), with the same pack format, the same
file layout and the same language table. The packs' licences are only needed
to decrypt them, which happens at import; the game itself never checks a
pack's content ID (`sceNpDrmIsAvailable`, which the port answers OK, then
`cellFsOpen` on the already-decrypted file).

The one difference is English. Both executables pick their text files with
the language table at `0x530A44` (`_jp, _uk, _fr, _sp, _ge, _it, -, -, -,
_kr/_ko, _tr, _si, _en`), and the matching folder names (`cmc/<lang>/`,
`talk/<lang>/`). The European release's English is entry 1, `_uk`; the North
American one's is entry 12, `_en`. The North American packs carry all five
languages — `_en`, `_fr`, `_sp`, `_ge`, `_it` — so French, Spanish, German and
Italian need nothing, and the importer installs each English file a second
time under the European name (`lsubm/LSubm50_en.edat` also as
`LSubm50_uk.edat`, `cmc/en/...` also as `cmc/uk/...`), recorded in
`install.dat_` like the rest; European packs get `_en` copies the same way.

Not yet tried in play. One thing to watch: North American free pack 19 brings
its own `LMonS2.bi5_` (the 1.10 table of Folks), replacing the European 1.10
update's; the two differ in 54 bytes, which look like pack 19's added Folk.

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
from the files, so the same steps apply (and either release's packs work in
either build, below).

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
changed nothing. Decrypting ran at about 3 MB/s. Removal was tested the same
way (`--remove`): a pack's files, its `_uk` copies and its record lines go,
emptied folders with them, pack 17 is refused, and adding the DLC again
restores everything.
