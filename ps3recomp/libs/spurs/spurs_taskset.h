/* spurs_taskset.h -- SPURS taskset Policy-Module shared-memory layout (Option B).
 *
 * ============================ BIG-ENDIAN WARNING ============================
 * These structures live in GUEST memory and are read by REAL lifted SPU code (the
 * leaf task) and by our reimplemented PM. The guest (PPC/SPU) is BIG-ENDIAN; the
 * host (x86) is little-endian. Therefore EVERY multi-byte field MUST be accessed
 * byte-swapped. Do NOT define native C structs over guest memory and write fields
 * directly (that stores little-endian -> the SPU reads garbage) -- that mismatch is
 * exactly why our prior HLE (which memset/native-wrote simplified structs) left the
 * SPU reading zeros/garbage (docs/12 root cause).
 *
 * RULE: access taskset/context fields ONLY via the BE accessors vm_read32/vm_read64/
 * vm_write32/vm_write64 (ppu_loader.cpp -- they byte-swap), at guest_EA + the offset
 * constants below. The offsets are the source of truth (reimplemented clean from the
 * public SDK / RPCS3 cellSpurs.h contracts; no GPL code copied).
 * ===========================================================================
 */
#ifndef SPURS_TASKSET_H
#define SPURS_TASKSET_H

#include <stdint.h>

/* BE guest-memory accessors. Pull in the canonical definitions rather than
 * re-declaring them here: ppu_memory.h defines this family `static inline`, and
 * a preceding extern declaration gives them external linkage instead, so every
 * translation unit including this header emitted its own vm_read32/vm_read64/
 * vm_write32/vm_write64.
 *
 * That stayed invisible while no title linked both this and the loader's
 * same-named (64-bit EA) family -- a static library only pulls in the objects
 * it needs. Tokyo Jungle references SPURS, which drags cellSpurs.c in, and the
 * link then failed with five duplicate symbols.
 *
 * It is also a hard error on Clang, which rejects the non-static
 * re-declaration outright where MSVC accepted it -- so this is what kept
 * the tree from compiling on macOS at all. */
#include "../../runtime/ppu/ppu_memory.h"

/* ---- CellSpursTaskset (main memory, 128-byte aligned) --------------------
 * The PM reads the task bitsets to pick a ready task and the TaskInfo array for
 * its ELF/context/args. Max 128 tasks; each TaskInfo is 48 bytes. */
enum {
    CSTS_RUNNING        = 0x00,   /* atomic 128-bit bitset (4 BE u32) */
    CSTS_READY          = 0x10,
    CSTS_PENDING_READY  = 0x20,
    CSTS_ENABLED        = 0x30,
    CSTS_SIGNALLED      = 0x40,
    CSTS_WAITING        = 0x50,
    CSTS_SPURS          = 0x60,   /* be u64: CellSpurs* */
    CSTS_ARGS           = 0x68,   /* be u64 */
    CSTS_ENABLE_CLEAR_LS= 0x70,   /* u8  */
    CSTS_WKL_FLAG_WAIT  = 0x72,   /* u8  */
    CSTS_LAST_SCHEDULED = 0x73,   /* u8  */
    CSTS_WID            = 0x74,   /* be u32: workload id */
    CSTS_X78            = 0x78,   /* be u64: on-task-exit handler EA */
    CSTS_TASKINFO       = 0x80,   /* TaskInfo[128] */
    CSTS_EXC_HANDLER    = 0x1880, /* be u64 */
    CSTS_EXC_HANDLER_ARG= 0x1888, /* be u64 */
    CSTS_SIZE_FIELD     = 0x1890, /* be u32 */
    CSTS_EVENT_FLAG_ID1 = 0x1898, /* be u32 */
    CSTS_EVENT_FLAG_ID2 = 0x189C, /* be u32 */
    CSTS_STRUCT_END     = 0x1900,
};

/* CellSpursTaskset::TaskInfo (48 bytes), at CSTS_TASKINFO + taskId*48 */
enum {
    TI_SIZE       = 48,
    TI_ARGS       = 0x00,   /* CellSpursTaskArgument (16 bytes) */
    TI_ELF        = 0x10,   /* be u64: task ELF EA (low 3 bits = flags) */
    TI_CONTEXT    = 0x18,   /* be u64: context_save_storage | alloc_ls_blocks */
    TI_LS_PATTERN = 0x20,   /* CellSpursTaskLsPattern (16 bytes) */
};

static inline uint32_t spurs_taskset_taskinfo_ea(uint32_t taskset_ea, uint32_t taskId)
{ return taskset_ea + CSTS_TASKINFO + taskId * TI_SIZE; }

/* set/clear/test a task bit in a 128-bit bitset (bit 0 = MSB of word 0, per RPCS3
 * atomic_tasks_bitset::get_bit). bitset_ea = taskset_ea + CSTS_READY etc. */
static inline void spurs_bitset_set(uint32_t bitset_ea, uint32_t taskId)
{
    uint32_t word_ea = bitset_ea + (taskId / 32) * 4;
    uint32_t mask = (1u << 31) >> (taskId % 32);
    vm_write32(word_ea, vm_read32(word_ea) | mask);
}
static inline void spurs_bitset_clear(uint32_t bitset_ea, uint32_t taskId)
{
    uint32_t word_ea = bitset_ea + (taskId / 32) * 4;
    uint32_t mask = (1u << 31) >> (taskId % 32);
    vm_write32(word_ea, vm_read32(word_ea) & ~mask);
}
static inline int spurs_bitset_test(uint32_t bitset_ea, uint32_t taskId)
{
    uint32_t word_ea = bitset_ea + (taskId / 32) * 4;
    uint32_t mask = (1u << 31) >> (taskId % 32);
    return (vm_read32(word_ea) & mask) != 0;
}

/* ---- SpursTasksetContext (SPU local store @ 0x2700, size 0x900) ----------
 * Built by the PM in LS before/while running the leaf task. */
enum {
    STC_BASE              = 0x2700,
    STC_TEMP_TASKSET      = 0x2700, /* 0x80 scratch */
    STC_TEMP_TASKINFO     = 0x2780, /* 0x30 (DMA'd TaskInfo of the selected task) */
    STC_TASKSET_PTR       = 0x27B8, /* be u64: CellSpursTaskset EA */
    STC_KERNEL_MGMT_ADDR  = 0x27C0, /* be u32 */
    STC_SYSCALL_ADDR      = 0x27C4, /* be u32 */
    STC_SPU_NUM           = 0x27CC, /* be u32 */
    STC_DMA_TAG_ID        = 0x27D0, /* be u32 */
    STC_TASK_ID           = 0x27D4, /* be u32 */
    STC_MODULE_ID         = 0x2840, /* 16 bytes */
    STC_SAVED_LR          = 0x2C80, /* v128: entry point / saved LR */
    STC_SAVED_SP          = 0x2C90, /* v128 */
    STC_SAVED_R80_R127    = 0x2CA0, /* v128[48] */
    STC_SAVED_FPSCR       = 0x2FA0, /* v128 */
    STC_SAVED_EVENT_MASK  = 0x2FB4, /* be u32 */
    STC_TASKSET_MGMT_ADDR = 0x2FB8, /* be u32 */
    STC_X2FC0             = 0x2FC0, /* be u64 (exit-handler addr alt) */
    STC_X2FC8             = 0x2FC8, /* be u64 (exit-handler args) */
    STC_TASK_EXIT_CODE    = 0x2FD0, /* be u32 */
    STC_X2FD4             = 0x2FD4, /* be u32 */
    STC_STRUCT_END        = 0x3000,
};

/* Fixed LS addresses of the taskset Policy Module entry points (RPCS3 cellSpurs.h).
 * A SPURS task calls its taskset PM's syscall entry via the SpursTasksetContext
 * syscallAddr field (STC_SYSCALL_ADDR), which the kernel sets to this address.
 * Our HLE intercepts a branch to CELL_SPURS_TASKSET_PM_SYSCALL_ADDR (spu_channels.c)
 * to process the task syscall, since we don't have the real PM code resident in LS. */
enum {
    CELL_SPURS_TASKSET_PM_ENTRY_ADDR   = 0xA00,
    CELL_SPURS_TASKSET_PM_SYSCALL_ADDR = 0xA70,
};

/* CELL_SPURS_TASK_SYSCALL_* (leaf-task `stop <code>` operand low nibble). */
enum {
    CELL_SPURS_TASK_SYSCALL_EXIT          = 0,
    CELL_SPURS_TASK_SYSCALL_YIELD         = 1,
    CELL_SPURS_TASK_SYSCALL_WAIT_SIGNAL   = 2,
    CELL_SPURS_TASK_SYSCALL_POLL          = 3,
    CELL_SPURS_TASK_SYSCALL_RECV_WKL_FLAG = 4,
};

/* ---- create-path layout builders (spurs_taskset.c, phase B2) ------------- */
void spurs_taskset_init(uint32_t taskset_ea, uint32_t spurs_ea, uint64_t args,
                        uint32_t wid, uint32_t size, uint32_t evf1, uint32_t evf2);
void spurs_taskset_add_task(uint32_t taskset_ea, uint32_t taskId, uint64_t elf_ea,
                            uint64_t context, const uint32_t arg[4],
                            const uint32_t ls_pattern[4]);
void spurs_taskset_set_exit_handler(uint32_t taskset_ea, uint64_t handler_ea);

/* ---- PM logic (spurs_pm.c, phase B3) ------------------------------------- */
int      spurs_pm_select_task(uint32_t taskset_ea, uint32_t last_scheduled_task);
void     spurs_pm_mark_running(uint32_t taskset_ea, uint32_t taskId);
uint64_t spurs_pm_build_context(uint8_t* ls, uint32_t taskset_ea, uint32_t taskId,
                                uint32_t spuNum, uint32_t dmaTagId);

#endif /* SPURS_TASKSET_H */
