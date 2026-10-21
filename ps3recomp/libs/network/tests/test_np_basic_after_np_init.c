/*
 * test_np_basic_after_np_init - sceNpBasic follows sceNpInit.
 *
 * A title calls sceNpInit and then sceNpBasic*; many never call
 * sceNpBasicInit. Basic must be usable between sceNpInit and sceNpTerm,
 * refuse before and after, and keep working for titles that do call
 * sceNpBasicInit.
 *
 * sceNp.h and sceNpBasic.h cannot share a translation unit, so sceNp.c is
 * built from the second file, test_np_basic_after_np_init_np.c:
 *   cc -std=gnu17 -I include -o test_np_basic_after_np_init \
 *      libs/network/tests/test_np_basic_after_np_init.c \
 *      libs/network/tests/test_np_basic_after_np_init_np.c
 * Driven by tools/test_np_basic_after_np_init.py.
 */
#include "../sceNpBasic.c"
#include <stdlib.h>

s32 sceNpInit(u32 poolSize, void* poolPtr);
s32 sceNpTerm(void);

#define NOTINIT ((s32)SCE_NP_BASIC_ERROR_NOT_INITIALIZED)
#define PRES ((const SceNpBasicPresence*)(uintptr_t)0x1000)   /* guest EA */
#define HANDLER ((SceNpBasicEventHandler)(uintptr_t)0x2000)

static int fails = 0;
#define CHECK(expr, want, what) do { s32 got_ = (s32)(expr); int ok_ = got_ == (s32)(want);              \
        printf("[%s] %s: 0x%08X (want 0x%08X)\n", ok_ ? "PASS" : "FAIL", what, (u32)got_, (u32)(want)); \
        if (!ok_) fails++; } while (0)

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    vm_base = calloc(1, 65536);
    if (!vm_base) return 2;
    memcpy(vm_base + 0x1000, "presence", 8);

    CHECK(sceNpBasicSetPresence(PRES), NOTINIT, "before sceNpInit: SetPresence");
    CHECK(sceNpBasicRegisterHandler(HANDLER, NULL, NULL), NOTINIT, "before sceNpInit: RegisterHandler");

    CHECK(sceNpInit(131072, (void*)0x1000), CELL_OK, "sceNpInit");
    CHECK(sceNpBasicSetPresence(PRES), CELL_OK, "after sceNpInit: SetPresence");
    CHECK(sceNpBasicSetPresence(NULL), SCE_NP_BASIC_ERROR_INVALID_ARGUMENT, "after sceNpInit: SetPresence(NULL)");
    CHECK(sceNpBasicRegisterHandler(HANDLER, NULL, NULL), CELL_OK, "after sceNpInit: RegisterHandler");
    CHECK(sceNpBasicUnregisterHandler(), CELL_OK, "after sceNpInit: UnregisterHandler");

    CHECK(sceNpBasicInit(NULL, 0), CELL_OK, "sceNpBasicInit");
    CHECK(sceNpBasicInit(NULL, 0), SCE_NP_BASIC_ERROR_ALREADY_INITIALIZED, "sceNpBasicInit twice");
    CHECK(sceNpBasicTerm(), CELL_OK, "sceNpBasicTerm");
    CHECK(sceNpBasicSetPresence(PRES), CELL_OK, "after sceNpBasicTerm, sceNp still up: SetPresence");

    CHECK(sceNpTerm(), CELL_OK, "sceNpTerm");
    CHECK(sceNpBasicSetPresence(PRES), NOTINIT, "after sceNpTerm: SetPresence");
    CHECK(sceNpBasicUnregisterHandler(), NOTINIT, "after sceNpTerm: UnregisterHandler");

    CHECK(sceNpBasicInit(NULL, 0), CELL_OK, "sceNpBasicInit alone");
    CHECK(sceNpBasicSetPresence(PRES), CELL_OK, "sceNpBasicInit alone: SetPresence");
    CHECK(sceNpBasicTerm(), CELL_OK, "sceNpBasicTerm");
    CHECK(sceNpBasicSetPresence(PRES), NOTINIT, "after sceNpBasicTerm: SetPresence");

    free(vm_base);
    printf("%s (%d failed)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}
