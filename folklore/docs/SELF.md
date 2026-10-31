# Decrypting EBOOT.BIN

The game's executable on the disc, `PS3_GAME\USRDIR\EBOOT.BIN`, and the 1.10
update's are encrypted PS3 executables (SELF files). The port decrypts them
itself, so RPCS3's *Utilities → Decrypt PS3 Binaries* is no longer needed:

- **setup.ps1** builds `tools\unself.exe` (clang-cl, a few seconds, once) and
  decrypts the EBOOT it lifts into `decrypted\<hash>\EBOOT.elf`. run.ps1 uses
  the same copy.
- **folklore.exe**, given a dump or update folder with only `EBOOT.BIN`,
  decrypts it into `decrypted\<title id>-<version>.elf` beside its data
  (once; it is reused).
- An `EBOOT.elf` RPCS3 already wrote beside `EBOOT.BIN` is used as it is.
- Nothing is written to the dump or the update folder.

The output is byte for byte RPCS3's: checked on 2026-10-31 against RPCS3's
`EBOOT.elf` for all four executables (both discs, both updates), each in
about 0.15 s.

| Executable | Program type | Licence |
|---|---|---|
| BCUS98147 / BCES00050 disc, v1.00 | application (4), key revision 1 | none |
| BCES00050 1.10 update | NPDRM (8), key revision 1 | free (`EP9000-BCES00050_00-FOLKLOREVIBPATCH`) |
| BCUS98147 1.10 update | NPDRM (8), key revision 1 | local: `UP9000-BCUS98147_00-FOLKLORE20000003.rap` |

The North American update's licence is installed by RPCS3 with the update
(*File → Install Packages/Raps/Edats*) into `dev_hdd0\home\<user>\exdata`;
setup and the game look there (two folders above the update's
`dev_hdd0\game\BCUS98147`), in a `licences` folder beside the port's data,
and in the update folder itself.

## How

`folklore/src/engine/engine_self.cpp` (its header comment has the layout):

1. Read the SCE header and the extended header: the offsets of the
   application info (program type), the ELF and program headers, the section
   headers and the control info.
2. Decrypt the 0x40-byte metadata info: for NPDRM first AES-128-CBC (zero
   IV) with the klicensee -- the free one, or the `.rap`'s RIF key -- itself
   decrypted with NP_KLIC_KEY; then AES-256-CBC with the program type's
   key-revision-1 key. A wrong licence shows here (its padding is not zero).
3. AES-128-CTR the metadata headers with the key and IV from step 2: the
   section list (offset, size, type, program index, encryption, key and IV
   indices, compression) and the section keys.
4. Each program section: AES-128-CTR with its key and IV, zlib-inflate when
   compressed (stb_image's inflate, which the ps3recomp runtime already
   contains), and write it at its program header's file offset, after the
   ELF header and program headers; the section headers go at `e_shoff`.

The AES, CBC and CTR code is the project's own (FIPS-197); the keys are the
published values (RPCS3's `Crypto/key_vault`, scetool, psdevwiki) -- the same
source as the RAP conversion the DLC importer uses (DLC.md). Only key
revision 1 is accepted, which is all Folklore uses.

## Testing it off Windows

`folklore/tools/unself_main.cpp` is the same code as a command-line tool:

    g++ -std=c++17 -O2 -I src/engine tools/unself_main.cpp src/engine/engine_self.cpp
    ./a.out EBOOT.BIN EBOOT.elf [licence folder]...

It also refuses, with a sentence, a missing or wrong licence, a file that is
not a SELF and a truncated one.
