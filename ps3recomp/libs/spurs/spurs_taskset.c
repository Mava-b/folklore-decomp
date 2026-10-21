/* spurs_taskset.c -- build the real CellSpursTaskset shared-memory layout (Option B,
 * phase B2). These functions write the BE structures that the reimplemented PM (B3)
 * and the real lifted leaf task read. ALL writes go through the byte-swapping
 * vm_write32/vm_write64 accessors (see the BIG-ENDIAN WARNING in spurs_taskset.h);
 * no native struct writes. Layout cross-referenced vs RPCS3 cellSpurs.h.
 *
 * Pure layout builders -- unit-tested in tests/test_spurs_taskset_main.c. cellSpurs.c
 * (the PPU create path) and the PM dispatcher call these.
 */
#include "spurs_taskset.h"
#include <stdint.h>

/* Initialize a fresh taskset: clear all task bitsets and write the header fields.
 * spurs_ea = the CellSpurs object EA; args = taskset args doubleword; wid = workload
 * id; size = taskset size; evf1/evf2 = attached event-flag ids (0 if none). */
void spurs_taskset_init(uint32_t taskset_ea, uint32_t spurs_ea, uint64_t args,
                        uint32_t wid, uint32_t size, uint32_t evf1, uint32_t evf2)
{
    /* clear running/ready/pending_ready/enabled/signalled/waiting (each 16 bytes) */
    for (uint32_t off = CSTS_RUNNING; off <= CSTS_WAITING; off += 0x10) {
        vm_write64(taskset_ea + off + 0, 0);
        vm_write64(taskset_ea + off + 8, 0);
    }
    vm_write64(taskset_ea + CSTS_SPURS, (uint64_t)spurs_ea); /* CellSpurs* (be u64) */
    vm_write64(taskset_ea + CSTS_ARGS,  args);
    vm_write32(taskset_ea + CSTS_WID,   wid);
    vm_write64(taskset_ea + CSTS_X78,   0);                  /* on-exit handler EA */
    vm_write32(taskset_ea + CSTS_SIZE_FIELD,     size);
    vm_write32(taskset_ea + CSTS_EVENT_FLAG_ID1, evf1);
    vm_write32(taskset_ea + CSTS_EVENT_FLAG_ID2, evf2);
}

/* Register a task: write task_info[taskId] (args/elf/context/ls_pattern) and mark the
 * task enabled + ready so the PM's SELECT_TASK picks it. arg/ls_pattern may be NULL. */
void spurs_taskset_add_task(uint32_t taskset_ea, uint32_t taskId, uint64_t elf_ea,
                            uint64_t context, const uint32_t arg[4],
                            const uint32_t ls_pattern[4])
{
    uint32_t ti = spurs_taskset_taskinfo_ea(taskset_ea, taskId);
    for (int i = 0; i < 4; i++) vm_write32(ti + TI_ARGS + i * 4, arg ? arg[i] : 0);
    vm_write64(ti + TI_ELF, elf_ea);
    vm_write64(ti + TI_CONTEXT, context);
    for (int i = 0; i < 4; i++)
        vm_write32(ti + TI_LS_PATTERN + i * 4, ls_pattern ? ls_pattern[i] : 0);
    spurs_bitset_set(taskset_ea + CSTS_ENABLED, taskId);
    spurs_bitset_set(taskset_ea + CSTS_READY,   taskId);
}

/* Set the taskset's on-task-exit handler EA (CellSpursTaskset.x78). */
void spurs_taskset_set_exit_handler(uint32_t taskset_ea, uint64_t handler_ea)
{
    vm_write64(taskset_ea + CSTS_X78, handler_ea);
}
