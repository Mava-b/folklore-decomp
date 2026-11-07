/*
 * ps3recomp - cellFiber HLE
 *
 * The SDK's PPU fibers: cooperative user-level threads that a scheduler runs on
 * one PPU thread. A scheduler (cellFiberPpuInitializeScheduler) owns a set of
 * fibers (cellFiberPpuCreateFiber), cellFiberPpuRunFibers runs them until every
 * one has ended, and fibers block on each other with WaitSignal / SendSignal /
 * JoinFiber. Each fiber has its own guest stack (the caller's eaStack) and its
 * own host stack: ucontext on POSIX, Windows fibers on Windows.
 *
 * Every pointer argument is a guest address, like in the other HLE libraries; the
 * structs are opaque to the host and only have the SDK's size and alignment.
 */

#ifndef PS3RECOMP_CELL_FIBER_H
#define PS3RECOMP_CELL_FIBER_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Error codes (the SDK's, module 0x8076)
 * -----------------------------------------------------------------------*/
#define CELL_FIBER_ERROR_AGAIN             0x80760001
#define CELL_FIBER_ERROR_INVAL             0x80760002
#define CELL_FIBER_ERROR_NOMEM             0x80760004
#define CELL_FIBER_ERROR_DEADLK            0x80760008
#define CELL_FIBER_ERROR_PERM              0x80760009
#define CELL_FIBER_ERROR_BUSY              0x8076000A
#define CELL_FIBER_ERROR_ABORT             0x8076000C
#define CELL_FIBER_ERROR_STAT              0x8076000F
#define CELL_FIBER_ERROR_ALIGN             0x80760010
#define CELL_FIBER_ERROR_NULL_POINTER      0x80760011
#define CELL_FIBER_ERROR_NOSYSINIT         0x80760020

/* ---------------------------------------------------------------------------
 * Guest structures: size and alignment as the SDK lays them out
 * -----------------------------------------------------------------------*/
#define CELL_FIBER_PPU_SCHEDULER_ATTRIBUTE_SIZE   256
#define CELL_FIBER_PPU_ATTRIBUTE_SIZE             256
#define CELL_FIBER_PPU_SCHEDULER_SIZE             512
#define CELL_FIBER_PPU_SIZE                       896
#define CELL_FIBER_PPU_SCHEDULER_ALIGN            128
#define CELL_FIBER_PPU_ALIGN                      128

typedef struct CellFiberPpuSchedulerAttribute CellFiberPpuSchedulerAttribute;
typedef struct CellFiberPpuAttribute          CellFiberPpuAttribute;
typedef struct CellFiberPpuScheduler          CellFiberPpuScheduler;
typedef struct CellFiberPpu                   CellFiberPpu;

/* ---------------------------------------------------------------------------
 * API
 * -----------------------------------------------------------------------*/

/* The SDK's inline cellFiberPpu*AttributeInitialize(attr) wrappers call these. */
s32 _cellFiberPpuInitialize(void);
s32 _cellFiberPpuSchedulerAttributeInitialize(CellFiberPpuSchedulerAttribute* attr, u32 sdkVersion);
s32 _cellFiberPpuAttributeInitialize(CellFiberPpuAttribute* attr, u32 sdkVersion);

s32 cellFiberPpuInitializeScheduler(CellFiberPpuScheduler* scheduler,
                                    const CellFiberPpuSchedulerAttribute* attr);
s32 cellFiberPpuFinalizeScheduler(CellFiberPpuScheduler* scheduler);

/* `entry` is the guest address of the fiber function's descriptor (OPD); its return value is the exit status.
 * `eaStack`/`sizeStack` are the fiber's guest stack. A lower `priority` number runs first, creation order breaks ties. */
s32 cellFiberPpuCreateFiber(CellFiberPpuScheduler* scheduler, CellFiberPpu* fiber, u32 entry, u64 arg,
                            u32 priority, void* eaStack, u32 sizeStack, const CellFiberPpuAttribute* attr);

/* Runs the scheduler's fibers on the calling thread until every one has ended. */
s32 cellFiberPpuRunFibers(CellFiberPpuScheduler* scheduler);

/* Called by a fiber. */
s32 cellFiberPpuWaitSignal(void);
s32 cellFiberPpuYield(void);
s32 cellFiberPpuExit(s32 status);
/* Guest address of the calling fiber, 0 outside a fiber. */
CellFiberPpu* cellFiberPpuSelf(void);

/* Callable from any thread. */
s32 cellFiberPpuSendSignal(CellFiberPpu* fiber, u32* numWorker);
s32 cellFiberPpuJoinFiber(CellFiberPpu* fiber, s32* status);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_FIBER_H */
