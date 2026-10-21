/*
 * ps3recomp -- cellAdec ABI constants
 *
 * Two sets of values in cellAdec.h were invented rather than looked up, and
 * both were load-bearing for ps1_netemu:
 *
 *   * the error codes. The title's audio thread drains the decoder with
 *     cellAdecGetPcm(handle, 0) and stops when it sees 0x80610005 (EMPTY) --
 *     the only cellAdec code it ever compares against, at 8 sites. We returned
 *     0x80610204, so the loop never terminated and the title re-ran its whole
 *     format-change path: 4760 cellAdecEndSeq calls against one StartSeq.
 *     Correcting the codes took that to 2.
 *
 *   * the message types. ERROR is 2 and SEQDONE is 3; they were swapped, so
 *     EndSeq's SEQDONE arrived at the guest as ERROR. The guest callback
 *     (func_000ED918, OPD 0x1B5F00) acts only on msgType == 1 (PCMOUT).
 *
 * Neither is checkable by reading our own source -- that is what made them
 * survive. Both are pinned here against the firmware's own compared value.
 *
 * Build:
 *   clang -std=c11 -O2 -I../.. test_adec_abi.c -o t.exe && ./t.exe
 */
#include <assert.h>
#include <stdio.h>

#include "../cellAdec.h"

int main(void)
{
    /* The value ps1_netemu compares GetPcm's return against (0x000EE238). */
    assert(CELL_ADEC_ERROR_EMPTY == 0x80610005u);

    /* The rest of the real list, in order, from RPCS3 Modules/cellAdec.h:9. */
    assert(CELL_ADEC_ERROR_FATAL == 0x80610001u);
    assert(CELL_ADEC_ERROR_SEQ   == 0x80610002u);
    assert(CELL_ADEC_ERROR_ARG   == 0x80610003u);
    assert(CELL_ADEC_ERROR_BUSY  == 0x80610004u);

    /* Nothing in the 0x8061_02xx range: that block was the invented one. */
    assert((CELL_ADEC_ERROR_ARG   & 0xFF00u) == 0);
    assert((CELL_ADEC_ERROR_EMPTY & 0xFF00u) == 0);

    /* Message types. PCMOUT == 1 is what the guest callback branches on. */
    assert(CELL_ADEC_MSG_TYPE_AUDONE  == 0);
    assert(CELL_ADEC_MSG_TYPE_PCMOUT  == 1);
    assert(CELL_ADEC_MSG_TYPE_ERROR   == 2);
    assert(CELL_ADEC_MSG_TYPE_SEQDONE == 3);

    printf("test_adec_abi: ok\n");
    return 0;
}
