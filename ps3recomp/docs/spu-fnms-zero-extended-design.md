# FNMS zero multiplied by an extended-range operand

The previous FNMS helper passed operands to host IEEE `fmaf`. SPU single-precision values with exponent FF are finite extended-range numbers; the host interprets those bits as infinity or NaN. A signed-zero multiplicand therefore produces a host NaN for inputs whose exact SPU product is zero. FNMS subtracts the product from its addend, so this narrow case should preserve the addend.

The change is limited to `spu_fnms`, per lane:

1. Use integer bits to recognize either signed zero in one multiplicand and exponent FF in the other, in both operand orders.
2. Return the addend bits unchanged when its exponent is nonzero. Return positive zero when its exponent is zero, including signed zero and denormal encodings.
3. Keep the existing host `fmaf` expression for every other input lane.

The implementation follows this guard. General nonzero extended multiplication, rounding, exception flags, denormal multiplicands, and other float helpers remain outside this change. The tests include ordinary finite controls but do not claim complete SPU floating-point conformance.

## References

[IBM/Sony/Toshiba SPU ISA v1.2, January 27, 2007](https://arcb.csc.ncsu.edu/~mueller/cluster/ps3/SDK3.0/docs/arch/SPU_ISA_v1.2_27Jan2007_pub.pdf), section 9.1 (printed pages 195–196) defines the extended range, signed-zero inputs, positive-zero results and denormal behavior. FNMS (printed page 210) defines addend minus exact product.

[RPCS3 precise FMA implementation](https://github.com/RPCS3/rpcs3/blob/bbb998b2977a2017baf04b03cb29455710f164f3/rpcs3/Emu/Cell/SPUInterpreter.cpp#L2780) at commit `bbb998b2977a2017baf04b03cb29455710f164f3`, file blob `546d94b354985a54c5ed8c5b880c9894a5807f1a`, independently special-cases an extended multiplicand with a zero product and preserves the addend. Its FNMS entry supplies the corresponding sign controls. This is a reference, not an imported implementation.

## Focused test

Run `python3 tests/test_spu_fnms_zero_extended.py`. It generates synthetic FNMS instructions and actual lifted C using the repository decoder/lifter, then compiles the real interpreter and helper at O2 with UBSan. No original program bytes or game data are needed. Unexpected channel/file operations abort instead of supplying invented runtime state.

448 mixed-lane cases cover both operand orders, both zero signs, eight extended-range encodings across both signs, and fourteen positive/negative normal/extended/zero/denormal addends. Every case exercises the helper, interpreter and compiled lift; interpreter/lift use a separate destination and each of the three input aliases. One ordinary finite control verifies exact arithmetic in the unchanged path.

The oracle follows only exact zero-product arithmetic plus documented exponent-zero normalization. The test is RED on upstream base `a2c20108af45174dbb7d3e2f87f6acaaef09c6b4` and GREEN with the guard. This test does not establish an application-level outcome.
