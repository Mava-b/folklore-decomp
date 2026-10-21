/*
 * spurs_native_job.h -- run Folklore's SPURS jobs without Sony's job manager
 * (spurs_native_job.c). One runner per job chain; not thread-safe, so a
 * runner belongs to the one thread that walks its chain.
 */
#ifndef FOLKLORE_SPURS_NATIVE_JOB_H
#define FOLKLORE_SPURS_NATIVE_JOB_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct folklore_job_runner folklore_job_runner;

folklore_job_runner* folklore_job_runner_create(void);

/* A new chain: forget where its binaries went. */
void folklore_job_runner_reset(folklore_job_runner* runner);

/* Fetch the descriptor at descriptorEa, place and fetch its binary, and run
 * the lifted job to completion. 0 on success, negative when the job could not
 * be staged (-1) or did not return normally (-2). */
int folklore_job_runner_run(folklore_job_runner* runner, uint32_t descriptorEa);

/* The chain waited (at a GUARD): the previous job's binary no longer counts
 * when placing the next job's working memory. */
void folklore_job_runner_idle(folklore_job_runner* runner);

uint64_t folklore_job_runner_count(const folklore_job_runner* runner);

#ifdef __cplusplus
}
#endif

#endif
