# Resume lifted code after interpreting an unlifted SPU region

Base: `5244db92669b54831c6d3ebb7a96d58c4dbfa6df`.
Branch: `fix/spu-interpreter-lifted-handoff`.

The SMC fallback in `runtime/spu/spu_channels.c` entered
`spu_interp_run_until()` and treated its numeric return as a success flag. When
interpretation reached a registered lifted entry, it returned that PC without
queuing a trampoline. A nested `spu_drain_call()` could repair that empty
trampoline, but the top-level `SPU_DRAIN` simply finished, leaving the context
RUNNING at a function that had never executed.

A second route makes the defect reachable after an initially nested call:

1. A lifted caller invokes an unlifted job under a nested drain.
2. The interpreted job calls a lifted helper. The nested drain resumes that
   helper, which returns to an unlifted job continuation.
3. Because that continuation has no lifted entry and is not the drain's own
   return address, the existing runtime restarts dispatch on the driver stack.
4. The job calls another lifted helper. This time the empty trampoline ends
   the top-level driver before that helper executes.

The change queues `spu_indirect_branch` only for a live, verified lifted rejoin:
nonnegative image ID, RUNNING status, a successful `spu_lifted_lookup`, and a PC
other than the already-owning drain's return address. The interpreter remains
scheduling-neutral. Existing SMC microsteps, stop/halt behavior, diagnostic
step limits, and nested drain restart behavior are unchanged. A valid lifted
entry at LS zero also succeeds; zero is not used as a success/failure test for
that confirmed transfer.

## Reproduction and validation

Run `python3 tools/test_spu_interpreter_handoff.py` with a POSIX C compiler.
The fixture links the real channel runtime, interpreter, function registry,
drain driver, and helpers. It uses hand-encoded synthetic instructions and
small generated-style C functions. External PPU/event hooks are link-only
abort-on-use boundaries; none participates in these CPU-only cases. Each mode
runs in a fresh child with a five-second deadline and a temporary working
directory. Behavior-changing SPU diagnostic environment variables are cleared
except for the deliberately configured step-limit controls.

The final fixture covers 13 modes:

- Top-level interpreter-to-lifted transfer, including an entry at LS zero.
- Nested interpreted job calling a lifted helper twice, crossing the existing
  driver restart between calls; both calls and final completion must execute.
- Registered and unregistered return addresses belonging to an already-active
  nested drain; the host continuation executes once and guest continuation
  bytes or an independently registered function at that address do not run.
- An existing SMC-only instruction/branch path and scheduling-neutral direct
  interpreter use.
- Diagnostic step-limit return at an unlifted PC, and pure interpretation with
  a negative image ID despite a conflicting image-zero wildcard entry.
- Baseline-equivalence observations for stop zero, nested stop zero, nonzero
  stop, and conditional halt. These are not claims of architectural stop
  correctness.

On the unchanged base, `top`, `top-zero`, and `nested-calls` fail; the other
controls pass. The nested job executes only its first helper before returning
RUNNING at the second helper. The initial broad rejoin condition was also
shown to violate the step-limit control and the negative-image wildcard
control. Both were narrowed only after reproducing those failures.

All 13 focused modes pass with the final change. The base's 17 tool scripts
passed before implementation; the complete 18-script suite including this
fixture passes with the change (`CI=1`, each `tools/test_*.py`).

## Limits and inherited behavior

A generated immediate `bisl` can call `spu_indirect_branch` before
`spu_drain_call` publishes the new return address. The owning-return tests
exercise an already-active drain and do not claim to repair that separate
initial-call boundary. Recursive calls returning to the same PC also retain
the interpreter's existing PC-only boundary matching.

The old fallback treats an interpreted stop code of zero as failure and can
misclassify it through the unresolved-branch path; nonzero interpreted stops
and halts do not necessarily hard-unwind the outer driver. The controls record
that existing behavior deliberately. This patch does not change `spu_stop`,
whose default lifted behavior differs from its optional hard-unwind mode.

The motivating native observation was a context left RUNNING at a registered
lifted helper after interpreted code had run. The synthetic fixture proves a
runtime handoff defect consistent with that observation. It does not establish
that this change alone resolves a title's wait, completes its jobs, or produces
rendering. No native title execution is part of this branch's validation.
