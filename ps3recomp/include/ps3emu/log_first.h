/*
 * ps3recomp - log a diagnostic only the first few times
 */
#ifndef PS3RECOMP_LOG_FIRST_H
#define PS3RECOMP_LOG_FIRST_H

/* True for the first `limit` evaluations of the counter `ctr` (a static variable of the call site), then false:
 *     static int _n = 0; if (PS3_LOG_FIRST(_n, 8)) fprintf(stderr, ...);
 * The counter stops at the limit instead of counting on, so a hot path cannot wrap it past 2^31 and start
 * printing on every call again. */
#define PS3_LOG_FIRST(ctr, limit) ((ctr) < (limit) && ++(ctr))

#endif /* PS3RECOMP_LOG_FIRST_H */
