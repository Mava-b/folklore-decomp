/*
 * spurs_native_job.c -- run one of Folklore's SPURS jobs the way Sony's job
 * manager (jm2) does, without the manager.
 *
 * The SPU half of the libsre replacement (spurs_native.cpp is the PPU half).
 * Everything here comes from traces of the real library
 * (folklore/docs/SPURS.md):
 *
 *   - the job's descriptor (0x80 bytes) is fetched into one of four LS
 *     buffers from 0x3F400 up, used in turn;
 *   - job binaries are packed upward from LS 0x4C00 in the order a chain
 *     first uses them, each rounded up to 1 KB. That is where the lifted
 *     placements (src/spu_gen_lle) were taken from, so the lift for the
 *     binary owns the range as soon as it is fetched there;
 *   - above the binary comes the job's working memory (the descriptor's u16
 *     at +0x1E, in 16-byte units, rounded up to 1 KB), then a 16 KB stack.
 *     The working memory starts above the previous job's binary when that
 *     still fits below 0x3F000, otherwise right after this job's own binary;
 *   - the job context at LS 0x48F0 is zero but for +0x70 (working memory)
 *     and +0x78 (the descriptor's EA); nothing else in LS is cleared;
 *   - the job is called at its first instruction with r3 = context,
 *     r4 = descriptor, r1 = stack top - 0x30 (a four-word frame there), and
 *     r5 = 0xFFFFFFFF, and returns through r0.
 *
 * r0 here is SPU_JOB_RETURN_LS, which spu_indirect_branch treats as "the job
 * is finished" and halts the run on.
 */
#include "spu_context.h"
#include "spu_dma.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spurs_native_job.h"

extern uint8_t* vm_base;
extern int spu_run_with_halt(void (*entry)(spu_context*), spu_context* ctx);
extern void spu_indirect_branch(spu_context* ctx);

#define JOB_CONTEXT_LS          0x48F0u
#define JOB_FIRST_BINARY_LS     0x4C00u
#define JOB_DESCRIPTOR_LS       0x3F400u
#define JOB_DESCRIPTOR_STRIDE   0x100u
#define JOB_DESCRIPTOR_BUFFERS  4u
#define JOB_DESCRIPTOR_SIZE     0x80u
#define JOB_MEMORY_LIMIT_LS     0x3F000u
#define JOB_STACK_SIZE          0x4000u
#define JOB_RETURN_LS           0x3FF00u   /* SPU_JOB_RETURN_LS in spu_channels.c */
#define JOB_MAX_DMA             0x4000u
#define JOB_MAX_BINARIES        16u
#define JOB_RUNNER_IMAGE_ID     90

struct folklore_job_runner
{
    spu_context* spu;
    uint32_t binaryEa[JOB_MAX_BINARIES];
    uint32_t binaryBase[JOB_MAX_BINARIES];
    uint32_t binaryCount;
    uint32_t nextBinaryBase;
    uint32_t previousBinaryEnd;
    uint32_t descriptorBuffer;
    uint64_t jobsRun;
};

static uint32_t readGuest32(uint32_t address)
{
    const uint8_t* bytes = vm_base + address;
    return ((uint32_t)bytes[0] << 24) | ((uint32_t)bytes[1] << 16) | ((uint32_t)bytes[2] << 8) | bytes[3];
}

static uint32_t readLs32(const spu_context* spu, uint32_t address)
{
    return spu_ls_read32(spu, address);
}

static uint16_t readLs16(const spu_context* spu, uint32_t address)
{
    const uint8_t* bytes = spu->ls + (address & SPU_LS_MASK);
    return (uint16_t)(((uint32_t)bytes[0] << 8) | bytes[1]);
}

static uint32_t alignTo1k(uint32_t value)
{
    return (value + 0x3FFu) & ~0x3FFu;
}

/* A GET through the MFC, in pieces of at most 16 KB, so that it is the same
 * transfer the real manager makes: the overlay tracker sees the binary
 * arrive at its LS address and hands the range to the matching lift. */
static int getFromGuest(spu_context* spu, uint32_t lsAddress, uint32_t guestAddress, uint32_t size)
{
    uint32_t done = 0;
    while (done < size)
    {
        uint32_t piece = size - done;
        if (piece > JOB_MAX_DMA)
        {
            piece = JOB_MAX_DMA;
        }
        if (mfc_do_transfer(spu, lsAddress + done, guestAddress + done, piece, MFC_GET_CMD) != 0)
        {
            return -1;
        }
        done += piece;
    }
    return 0;
}

folklore_job_runner* folklore_job_runner_create(void)
{
    folklore_job_runner* runner = (folklore_job_runner*)calloc(1, sizeof(folklore_job_runner));
    if (!runner)
    {
        return NULL;
    }
    runner->spu = (spu_context*)calloc(1, sizeof(spu_context));
    if (!runner->spu)
    {
        free(runner);
        return NULL;
    }
    spu_context_init(runner->spu, 0);
    /* No image of its own: every job's code is found through the resident
     * lift its binary fetch selected. An id nothing registers keeps the
     * fallback lookup (spu_indirect_branch) from landing in an unrelated
     * image -- 0 is src/spu_gen's. */
    runner->spu->image_id = JOB_RUNNER_IMAGE_ID;
    folklore_job_runner_reset(runner);
    return runner;
}

void folklore_job_runner_reset(folklore_job_runner* runner)
{
    runner->binaryCount = 0;
    runner->nextBinaryBase = JOB_FIRST_BINARY_LS;
    runner->previousBinaryEnd = 0;
    runner->descriptorBuffer = 0;
}

uint64_t folklore_job_runner_count(const folklore_job_runner* runner)
{
    return runner->jobsRun;
}

/* Where the chain keeps this binary: the address it was given when the chain
 * first ran it, or the next free one. */
static uint32_t placeBinary(folklore_job_runner* runner, uint32_t binaryEa, uint32_t binarySize)
{
    for (uint32_t index = 0; index < runner->binaryCount; index++)
    {
        if (runner->binaryEa[index] == binaryEa)
        {
            return runner->binaryBase[index];
        }
    }
    const uint32_t base = runner->nextBinaryBase;
    if (runner->binaryCount < JOB_MAX_BINARIES)
    {
        runner->binaryEa[runner->binaryCount] = binaryEa;
        runner->binaryBase[runner->binaryCount] = base;
        runner->binaryCount++;
        runner->nextBinaryBase = base + alignTo1k(binarySize);
    }
    return base;
}

int folklore_job_runner_run(folklore_job_runner* runner, uint32_t descriptorEa)
{
    spu_context* spu = runner->spu;

    /* The descriptor, into the next of the manager's four buffers. */
    const uint32_t descriptorLs = JOB_DESCRIPTOR_LS + runner->descriptorBuffer * JOB_DESCRIPTOR_STRIDE;
    runner->descriptorBuffer = (runner->descriptorBuffer + 1u) % JOB_DESCRIPTOR_BUFFERS;
    if (getFromGuest(spu, descriptorLs, descriptorEa, JOB_DESCRIPTOR_SIZE) != 0)
    {
        fprintf(stderr, "[spurs-native] job 0x%08X: descriptor fetch failed\n", descriptorEa);
        return -1;
    }
    const uint32_t binaryEa = readLs32(spu, descriptorLs + 0x04) & ~0xFu;
    const uint32_t binarySize = (uint32_t)readLs16(spu, descriptorLs + 0x08) << 4;
    const uint32_t dmaListSize = readLs16(spu, descriptorLs + 0x0A);
    const uint32_t workSize = alignTo1k((uint32_t)readLs16(spu, descriptorLs + 0x1E) << 4);
    if (!binaryEa || !binarySize || dmaListSize)
    {
        /* Folklore's jobs fetch their own data; a descriptor asking the
         * manager for input DMA is something this runner does not do. */
        static int s_reported = 0;
        if (s_reported++ < 8)
        {
            fprintf(stderr,
                    "[spurs-native] job 0x%08X: unsupported descriptor (binary 0x%08X size 0x%X, dma list 0x%X)\n",
                    descriptorEa, binaryEa, binarySize, dmaListSize);
        }
        return -1;
    }

    /* The binary, at its place in the chain's layout. */
    const uint32_t binaryBase = placeBinary(runner, binaryEa, binarySize);
    const uint32_t binaryEnd = binaryBase + alignTo1k(binarySize);
    if (binaryEnd > JOB_MEMORY_LIMIT_LS || getFromGuest(spu, binaryBase, binaryEa, binarySize) != 0)
    {
        fprintf(stderr, "[spurs-native] job 0x%08X: binary 0x%08X does not fit at LS 0x%05X\n", descriptorEa,
                binaryEa, binaryBase);
        return -1;
    }

    /* Working memory and stack. */
    uint32_t workBase = binaryEnd;
    if (runner->previousBinaryEnd > workBase &&
        runner->previousBinaryEnd + workSize + JOB_STACK_SIZE <= JOB_MEMORY_LIMIT_LS)
    {
        workBase = runner->previousBinaryEnd;
    }
    const uint32_t stackTop = workBase + workSize + JOB_STACK_SIZE;
    if (stackTop > JOB_MEMORY_LIMIT_LS)
    {
        fprintf(stderr, "[spurs-native] job 0x%08X: memory 0x%05X..0x%05X does not fit\n", descriptorEa,
                binaryBase, stackTop);
        return -1;
    }
    runner->previousBinaryEnd = binaryEnd;
    const uint32_t stackPointer = stackTop - 0x30u;

    /* The job context: zero but for the working memory and the descriptor EA. */
    memset(spu->ls + JOB_CONTEXT_LS, 0, 0x80);
    spu_ls_write32(spu, JOB_CONTEXT_LS + 0x70, workBase);
    spu_ls_write32(spu, JOB_CONTEXT_LS + 0x78, 0);
    spu_ls_write32(spu, JOB_CONTEXT_LS + 0x7C, descriptorEa);

    /* The frame the manager leaves at r1, then zeros up to the stack top. */
    spu_ls_write32(spu, stackPointer + 0x00, stackPointer + 0x20);
    spu_ls_write32(spu, stackPointer + 0x04, JOB_STACK_SIZE + 0x20);
    spu_ls_write32(spu, stackPointer + 0x08, stackTop - workBase + 0x20);
    spu_ls_write32(spu, stackPointer + 0x0C, 0x20);
    memset(spu->ls + stackPointer + 0x10, 0, 0x20);

    /* Registers, then into the job. */
    memset(spu->gpr, 0, sizeof(spu->gpr));
    spu->gpr[0]._u32[0] = JOB_RETURN_LS;
    spu->gpr[1]._u32[0] = stackPointer;
    spu->gpr[3]._u32[0] = JOB_CONTEXT_LS;
    spu->gpr[4]._u32[0] = descriptorLs;
    spu->gpr[5]._u32[0] = 0xFFFFFFFFu;
    spu->pc = binaryBase;
    spu->status = SPU_STATUS_RUNNING;
    spu->stop_code = 0;
    spu_run_with_halt(spu_indirect_branch, spu);
    runner->jobsRun++;

    if ((spu->pc & SPU_LS_MASK) != JOB_RETURN_LS)
    {
        static int s_reported = 0;
        if (s_reported++ < 8)
        {
            fprintf(stderr,
                    "[spurs-native] job 0x%08X (binary 0x%08X at LS 0x%05X) stopped at pc 0x%05X, status 0x%X, "
                    "stop 0x%X -- not through its return\n",
                    descriptorEa, binaryEa, binaryBase, (unsigned)(spu->pc & SPU_LS_MASK), spu->status,
                    spu->stop_code);
        }
        return -2;
    }
    return 0;
}
