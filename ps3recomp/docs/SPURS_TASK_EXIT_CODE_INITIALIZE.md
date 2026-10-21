# SPURS exit-container initialization

`cellSpursTaskExitCodeInitialize` (NID `0x34552FA6`) takes one guest pointer
to a 128-byte `CellSpursTaskExitCode`, aligned to 128 bytes. It returns task
error `0x80410911` for a null guest pointer and `0x80410910` for misalignment,
in that order. A valid call clears exactly 128 bytes and returns zero.
Reinitialization clears the container again.

This contract was independently read from the original firmware export and
its memset call. No original firmware code or payload is included here. The
container's opaque size/alignment is also corroborated by the
[RPCS3 header](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSpurs.h).
RPCS3's initializer is unimplemented and is not the behavioral oracle.

Pointer arguments follow the runtime's low-32-bit guest-EA convention. Before
writing, the HLE checks VM readiness, a nonwrapping span, and the configured
backing limit. A zero `ppu_vm_size` retains the runtime's full-VM convention;
a nonzero size permits native-VA ports with a zero base. An invalid backing
span returns task `FAULT` (`0x8041090D`) as host safety behavior, rather than
attempting an out-of-bounds host write.

Initialization does not attach the container to a task attribute or publish
an exit result. `cellSpursTaskAttributeSetExitCodeContainer`, retention through
task creation, and result publication/retrieval/wakeup remain separate work.

`tools/test_spurs_task_exit_code_initialize.py` compiles the real production
handler and the actual generated target NID registration. Synthetic guest
memory verifies errors, exact clearing with canaries, repeated initialization,
guest pointer narrowing, and backing failures. The standalone fixture uses a
POSIX C/C++ toolchain; Windows skips it explicitly. It runs through CTest when
tests and Python are enabled.
