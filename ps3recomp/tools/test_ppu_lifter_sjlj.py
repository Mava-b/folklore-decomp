#!/usr/bin/env python3
"""--sjlj: a title's setjmp/longjmp lifted as host setjmp/longjmp.

A small hand-assembled program -- a newlib-shaped setjmp/longjmp pair, a
Lua-shaped protected call (luaD_rawrunprotected: setjmp, then the body
through bctrl), a throw (luaD_throw: longjmp to the innermost handler, or
"panic" when it returns) and a body that recurses before throwing -- is lifted
and checked:

1. Without --sjlj nothing changes: no sjlj text in the output.
2. With it the setjmp call is a PPU_SJLJ_ENTER site in the caller, every exit
   of that caller frees its setjmps, the longjmp body ends in
   ppu_sjlj_longjmp, ppu_sjlj.h is included before both, and the serial and
   parallel lifts agree.
3. A spec whose address does not hold newlib's words stops the lift; a
   newlib-shaped setjmp the spec leaves out is reported; a setjmp call in a
   function whose guest frame continues in another host function stays a
   plain call, with a warning.
4. The lifted program, built with the real ppu_loader.cpp and ppu_sjlj.c,
   runs: a throw from 6 calls deep reaches the protected call with the value
   it threw and the caller's r1/r31 restored; a throw through a jmp_buf whose
   protected call has returned comes back as before (the "panic" path).

Run: python tools/test_ppu_lifter_sjlj.py
"""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parent))
from ppu_disasm import disassemble_bytes  # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
BASE = 0x10000


# ---- a tiny assembler: just the forms the program uses ----------------------
def _d(op, rt, ra, imm):
    return (op << 26) | (rt << 21) | (ra << 16) | (imm & 0xFFFF)


def addi(rt, ra, imm): return _d(14, rt, ra, imm)
def li(rt, imm): return _d(14, rt, 0, imm)
def lwz(rt, d, ra): return _d(32, rt, ra, d)
def stw(rs, d, ra): return _d(36, rs, ra, d)
def std(rs, ds, ra): return (62 << 26) | (rs << 21) | (ra << 16) | (ds & 0xFFFC)
def stdu(rs, ds, ra): return std(rs, ds, ra) | 1
def ld(rt, ds, ra): return (58 << 26) | (rt << 21) | (ra << 16) | (ds & 0xFFFC)
def cmpwi(ra, imm): return (11 << 26) | (ra << 16) | (imm & 0xFFFF)
def mr(ra, rs): return (31 << 26) | (rs << 21) | (ra << 16) | (rs << 11) | (444 << 1)


MFLR0, MTLR0, MFCR0, MTCRF0 = 0x7C0802A6, 0x7C0803A6, 0x7C000026, 0x7C0FF120
MTCTR0, BCTRL, BLR, NOP = 0x7C0903A6, 0x4E800421, 0x4E800020, 0x60000000
ALIGN16 = (0x3863000F, 0x786306E4)            # addi r3,r3,15; rldicr r3,r3,0,59
SUBFC_R0_R4_R3, ADDZE_R3_R4 = 0x7C041810, 0x7C640194


def assemble(items):
    """items: ints, ('label', name), or ('bl'|'b'|'beq'|'bne', name)."""
    labels, addr = {}, BASE
    for it in items:
        if isinstance(it, tuple) and it[0] == "label":
            labels[it[1]] = addr
        else:
            addr += 4
    out, addr = [], BASE
    for it in items:
        if isinstance(it, tuple):
            kind, name = it
            if kind == "label":
                continue
            d = labels[name] - addr
            w = {"bl": (18 << 26) | (d & 0x3FFFFFC) | 1,
                 "b": (18 << 26) | (d & 0x3FFFFFC),
                 "beq": (16 << 26) | (12 << 21) | (2 << 16) | (d & 0xFFFC),
                 "bne": (16 << 26) | (4 << 21) | (2 << 16) | (d & 0xFFFC)}[kind]
        else:
            w = it
        out.append(w)
        addr += 4
    return b"".join(w.to_bytes(4, "big") for w in out), labels


def program():
    items = [
        # setjmp: jmp_buf = r1 r2 r31 lr cr
        ("label", "setjmp"), *ALIGN16,
        std(1, 0, 3), std(2, 8, 3), std(31, 0x10, 3),
        MFLR0, std(0, 0x18, 3), MFCR0, std(0, 0x20, 3), li(3, 0), BLR,
        ("label", "longjmp"), *ALIGN16,
        ld(1, 0, 3), ld(2, 8, 3), ld(31, 0x10, 3),
        ld(0, 0x18, 3), MTLR0, ld(0, 0x20, 3), MTCRF0,
        li(3, 0), SUBFC_R0_R4_R3, ADDZE_R3_R4, BLR,
        # protect(L = r3, body = r4) -> r3 = 0 or the thrown value
        ("label", "protect"),
        MFLR0, std(31, -8, 1), std(0, 16, 1), stdu(1, -0x100, 1),
        mr(31, 3), std(4, 0xC8, 1),
        addi(3, 1, 0x70), stw(3, 0, 31),          # L->errorJmp = jmp_buf
        ("bl", "setjmp"), NOP,
        cmpwi(3, 0), ("bne", "caught"),
        ld(0, 0xC8, 1), MTCTR0, mr(3, 31), BCTRL,
        li(3, 0),
        ("label", "caught"),
        li(0, 0), stw(0, 0, 31),                  # L->errorJmp = NULL
        addi(1, 1, 0x100), ld(0, 16, 1), MTLR0, ld(31, -8, 1), BLR,
        # throw(L = r3, value = r4): longjmp, or -1 ("panic") if that returns
        ("label", "throw"),
        MFLR0, std(0, 16, 1), stdu(1, -0x70, 1),
        lwz(3, 0, 3), cmpwi(3, 0), ("beq", "panic"),
        ("bl", "longjmp"), NOP,
        ("label", "panic"),
        li(3, -1), addi(1, 1, 0x70), ld(0, 16, 1), MTLR0, BLR,
        # deep(L): L->count levels of recursion (clobbering r31), then throw 7
        ("label", "deep"),
        MFLR0, std(31, -8, 1), std(0, 16, 1), stdu(1, -0x80, 1),
        lwz(9, 4, 3), cmpwi(9, 0), ("beq", "do_throw"),
        addi(9, 9, -1), stw(9, 4, 3), li(31, 0x55),
        ("bl", "deep"), ("b", "deep_out"),
        ("label", "do_throw"),
        li(4, 7), ("bl", "throw"),
        ("label", "deep_out"),
        addi(1, 1, 0x80), ld(0, 16, 1), MTLR0, ld(31, -8, 1), BLR,
        # split(r3 = jmp_buf): its guest frame continues in another function
        ("label", "split"),
        ("bl", "setjmp"), ("b", "other_mid"),
        ("label", "other"),
        li(3, 1), li(3, 2),
        ("label", "other_mid"),
        li(3, 3), BLR,
        ("label", "end"),
    ]
    return assemble(items)


CODE, L = program()
FUNCS = ["setjmp", "longjmp", "protect", "throw", "deep", "split", "other"]


def a(name):
    return f"0x{L[name]:08X}"


def lift(root, spec, jobs="1", tag=""):
    out = root / f"out{tag}{jobs}"
    cmd = [sys.executable, str(Path(__file__).with_name("ppu_lifter.py")),
           str(root / "code.bin"), "--raw", "--base", hex(BASE),
           "--functions", str(root / "functions.json"), "--jobs", jobs,
           "--output", str(out)]
    if spec is not None:
        (root / f"sjlj{tag}.json").write_text(json.dumps(spec))
        cmd += ["--sjlj", str(root / f"sjlj{tag}.json")]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       text=True, timeout=300)
    text = "".join(p.read_text() for p in sorted(out.glob("ppu_recomp*.cpp"))) \
        if r.returncode == 0 else ""
    return r, text, out


def func_text(text, name):
    start = text.index(f"void func_{L[name]:08X}(ppu_context* ctx) {{")
    return text[start:text.index("\n}\n", start)]


def run_program(root, out):
    """Build the lifted program with the real scaffold and run it."""
    sys.path.insert(0, str(Path(__file__).resolve().parent))
    driver = root / "driver.cpp"
    driver.write_text(DRIVER.replace("@PROTECT@", a("protect")).replace("@THROW@", a("throw"))
                      .replace("@DEEP@", a("deep")))
    inc = ["-I" + str(out), "-I" + str(ROOT / "include"), "-I" + str(ROOT / "runtime/platform"),
           "-I" + str(ROOT / "runtime/ppu")]
    flags = ["-Wl,-dead_strip"] if sys.platform == "darwin" else ["-Wl,--gc-sections", "-pthread", "-ldl"]
    obj = root / "ppu_sjlj.o"
    subprocess.run([os.environ.get("CC", "cc"), "-std=c11", "-O2", "-c", *inc,
                    str(ROOT / "runtime/ppu/ppu_sjlj.c"), "-o", str(obj)], check=True)
    binary = root / "prog"
    subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-O2", "-w",
                    "-ffunction-sections", "-fdata-sections", *inc,
                    str(ROOT / "runtime/ppu/ppu_loader.cpp"), str(driver),
                    *map(str, sorted(out.glob("ppu_recomp*.cpp"))), str(obj),
                    *flags, "-o", str(binary)], check=True)
    env = {k: v for k, v in os.environ.items() if not k.startswith(("PPU_", "PS3_", "PS3RECOMP_", "SPU_"))}
    return subprocess.run([str(binary)], env=env, capture_output=True, text=True, timeout=60)


DRIVER = r"""
#include "ppu_recomp.h"
#include "ppu_sjlj.h"
#include <cstdio>
#include <cstdlib>
#include "win32_compat.h"
extern "C" {
uint8_t* vm_base;
uint32_t ppu_hle_inject_base = 0x20000000;
int spu_coh_is_reserved(uint32_t) { return 0; }
void spu_coh_notify_write(uint32_t) { std::abort(); }
void spu_lockline_lock(void) { std::abort(); }
void spu_lockline_unlock(void) { std::abort(); }
int spu_raw_reg_load(uint32_t, uint32_t*) { std::abort(); }
void spu_raw_reg_store(uint32_t, uint32_t, unsigned) { std::abort(); }
void cellGcm_ref_on_poll(void) { std::abort(); }
HANDLE CreateThread(void*, SIZE_T, PS3_THREAD_FN, LPVOID, DWORD, DWORD*) { std::abort(); }
BOOL CloseHandle(HANDLE) { std::abort(); }
DWORD GetFileAttributesA(const char*) { return INVALID_FILE_ATTRIBUTES; }
uint64_t ppu_thread_register_main(void) { return 1; }
void (*g_ppu_thread_entry_trampoline)(uint32_t, uint64_t, uint32_t, uint64_t);
uint32_t ps3_hle_count(void) { return 0; }
void ps3_hle_call(unsigned int, ppu_context*) { std::abort(); }
void lv2_syscall(ppu_context*) { std::abort(); }
uint64_t ppu_timebase_now(void) { return 0; }
void ppu_register_function(uint64_t addr, void (*fn)(ppu_context*));
int* ps3_ppu_dispatch_depth_slot(void);
static int s_hle_depth;
int* ps3_hle_depth_slot(void) { return &s_hle_depth; }   /* ppu_hle.cpp's, not linked here */
extern PPU_THREAD_LOCAL ppu_context* g_active_ctx;
extern PPU_THREAD_LOCAL void (*g_trampoline_fn)(void*);
}
static void call(ppu_context* c, uint32_t addr)
{
    for (uint64_t i = 0; i < function_table_count; i++)
        if (function_table[i].addr == addr) {
            c->lr = 0x10;   /* any return address */
            function_table[i].func(c);
            while (g_trampoline_fn) { void (*f)(void*) = g_trampoline_fn; g_trampoline_fn = 0; f(c); }
            return;
        }
    std::abort();
}
int main()
{
    vm_base = (uint8_t*)std::calloc(1, 0x100000);
    for (uint64_t i = 0; i < function_table_count; i++)
        ppu_register_function(function_table[i].addr, function_table[i].func);
    static ppu_context ctx;
    ctx.gpr[1] = 0xF0000; ctx.gpr[2] = 0x2222; ctx.gpr[31] = 0x3131;
    g_active_ctx = &ctx;
    const uint32_t Lst = 0x40000;
    unsigned fails = 0;

    vm_write32(Lst + 4, 6);
    ctx.gpr[3] = Lst; ctx.gpr[4] = @DEEP@;
    call(&ctx, @PROTECT@);
    ppu_sjlj_stats st; ppu_sjlj_get_stats(&st);
    bool ok = ctx.gpr[3] == 7 && ctx.gpr[1] == 0xF0000 && ctx.gpr[31] == 0x3131 && st.jumps == 1 &&
              *ps3_ppu_dispatch_depth_slot() == 0 && st.live == 0;
    std::printf("[%s] throw from 6 calls deep: protect returned %lld (want 7), r1 %llX, r31 %llX, "
                "jumps %llu, live slots %u\n", ok ? "PASS" : "FAIL", (long long)ctx.gpr[3],
                (unsigned long long)ctx.gpr[1], (unsigned long long)ctx.gpr[31],
                (unsigned long long)st.jumps, st.live);
    fails += !ok;

    vm_write32(Lst, 0xF0000 - 0x100 + 0x70);   /* a jmp_buf whose protect has returned */
    ctx.gpr[3] = Lst; ctx.gpr[4] = 9;
    call(&ctx, @THROW@);
    ppu_sjlj_get_stats(&st);
    ok = (int64_t)ctx.gpr[3] == -1 && st.fallbacks == 1 && st.jumps == 1;
    std::printf("[%s] throw through a stale jmp_buf comes back (panic path): r3 %lld, fallbacks %llu\n",
                ok ? "PASS" : "FAIL", (long long)ctx.gpr[3], (unsigned long long)st.fallbacks);
    fails += !ok;
    return fails ? 1 : 0;
}
"""


def main():
    # The encodings above are checked against the disassembler the lifter uses.
    insns = disassemble_bytes(CODE, BASE, True)
    by = {i.addr: i for i in insns}
    assert by[L["setjmp"] + 8].mnemonic == "std" and by[L["longjmp"] + 8].mnemonic == "ld"
    assert by[L["protect"] + 0x20].mnemonic == "bl", by[L["protect"] + 0x20]

    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        (root / "code.bin").write_bytes(CODE)
        bounds = [L[n] for n in FUNCS] + [L["end"]]
        (root / "functions.json").write_text(json.dumps(
            [{"start": hex(s), "end": hex(e)} for s, e in zip(bounds, bounds[1:])]))
        spec = {"setjmp": [a("setjmp")], "longjmp": [a("longjmp")]}

        # 1
        r, plain, _ = lift(root, None, tag="plain")
        assert r.returncode == 0, r.stderr[-2000:]
        assert "sjlj" not in plain.lower(), "sjlj text without --sjlj"

        # 2
        r, text, out = lift(root, spec)
        serial = r
        assert r.returncode == 0, r.stderr[-2000:]
        assert "sjlj: 1 setjmp call site(s), 1 longjmp body(ies) hooked" in r.stdout, r.stdout
        site = L["protect"] + 0x24
        prot = func_text(text, "protect")
        assert (f"ctx->lr = 0x{site:08X}; if (PPU_SJLJ_ENTER(ctx, 0x{site:08X}u) == 0) "
                f"func_{L['setjmp']:08X}(ctx); DRAIN_TRAMPOLINE(ctx);") in prot, prot
        assert "{ ppu_sjlj_leave(PPU_SJLJ_FRAME()); return; }" in prot, prot
        assert "return;" not in prot.replace("{ ppu_sjlj_leave(PPU_SJLJ_FRAME()); return; }", ""), prot
        lj = func_text(text, "longjmp")
        assert "const uint64_t _sjlj_buf = ctx->gpr[3];" in lj, lj
        assert lj.rstrip().endswith("ppu_sjlj_longjmp(ctx, _sjlj_buf); return;  /* sjlj longjmp */"), lj
        for n in ("protect", "longjmp"):
            i = text.index(f"void func_{L[n]:08X}(")
            assert text[:i].rstrip().endswith('#include "ppu_sjlj.h"  /* --sjlj; runtime/ppu */'), n
        # the other functions are lifted exactly as without --sjlj
        for n in ("setjmp", "throw", "deep", "other"):
            assert func_text(text, n) == func_text(plain, n), n
        r2, text2, _ = lift(root, spec, jobs="2")
        assert r2.returncode == 0 and text2 == text, "parallel lift differs from serial"

        # 3
        bad = {"setjmp": [a("protect")], "longjmp": [a("longjmp")]}
        r, _, _ = lift(root, bad, tag="bad")
        assert r.returncode != 0 and "expected 3863000F 786306E4 F8230000" in r.stderr, r.stderr[-600:]
        r, _, _ = lift(root, {"setjmp": [a("setjmp")], "longjmp": ["0x20000"]}, tag="nofunc")
        assert r.returncode != 0 and "not a function start" in r.stderr, r.stderr[-600:]
        assert f"WARNING: sjlj: func_{L['split']:08X} keeps its setjmp call(s) plain" in serial.stdout, \
            serial.stdout
        split = func_text(text, "split")
        assert (f"ctx->lr = 0x{L['split'] + 4:08X}; func_{L['setjmp']:08X}(ctx); DRAIN_TRAMPOLINE(ctx);"
                in split and "SJLJ" not in split), split
        # a second newlib setjmp the spec does not name
        code2 = CODE + CODE[L["setjmp"] - BASE:L["longjmp"] - BASE]
        (root / "code.bin").write_bytes(code2)
        extra = BASE + len(CODE)
        (root / "functions.json").write_text(json.dumps(
            [{"start": hex(s), "end": hex(e)} for s, e in zip(bounds, bounds[1:])] +
            [{"start": hex(extra), "end": hex(extra + L["longjmp"] - L["setjmp"])}]))
        r, _, _ = lift(root, spec, tag="extra")
        assert r.returncode == 0 and f"WARNING: sjlj: 0x{extra:08X} starts like newlib's setjmp" \
            in r.stdout, r.stdout[-800:]
        (root / "code.bin").write_bytes(CODE)

        # 4
        if os.name == "nt":
            print("SKIP: running the lifted program (POSIX scaffold stubs)")
        else:
            rr = run_program(root, out)
            print(rr.stdout, end="")
            assert rr.returncode == 0, rr.stderr[-2000:]
    print("PASS: --sjlj lifts setjmp sites and the longjmp hook, refuses a wrong spec, "
          "and the lifted program unwinds")


if __name__ == "__main__":
    main()
