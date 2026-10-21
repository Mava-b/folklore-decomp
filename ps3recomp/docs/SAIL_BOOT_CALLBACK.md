# Sail boot completion parameter

`cellSailPlayerBoot` now forwards its complete 64-bit `userParam` as callback
`arg1` for `PLAYER_CALL_COMPLETED / BOOT` (event major 2, minor 1).
Previously the runtime discarded this parameter and always delivered zero.
The callback's registered player argument remains the first argument. State
notifications continue to receive zero for `arg1`, and the existing sequence
remains transition state, call completion, settled state. Other player calls
continue to use zero completion parameters.

The motivating guest caller passes a future as its boot token. The guest's
completion callback uses that same token to select the future receiving the
result. Delivering zero instead loses the caller's completion target. This
parameter relationship requires preserving the supplied token; it does not
require recognizing a title, a guest address, or a particular image.

[RPCS3's cellSail implementation](https://github.com/RPCS3/rpcs3/blob/master/rpcs3/Emu/Cell/Modules/cellSail.cpp)
corroborates the typed Boot parameter and callback/event shape. Its current
Boot implementation also sends zero for this parameter; it is incomplete and
is **not** evidence of the corrected echo behavior. The observed guest caller
and callback establish the required parameter relationship here.

The runtime still delivers its existing synchronous completion. This change
neither completes a future directly nor implements decoding, playback, new
events or broader asynchronous lifecycle semantics.

`python3 tools/test_sail_boot_callback.py` compiles actual `cellSail.c`, generated
NID registration, and the real PPU HLE dispatcher. Forty checks cover full-width,
zero and guest-pointer tokens, event order and all callback argument positions,
two players, repeated calls, Stop/Close notification preservation, a destroyed
player, an absent callback and an absent dispatch hook. The regression failed
on the lost completion parameter before the source fix; all 18 SDK regression
scripts pass afterward.
