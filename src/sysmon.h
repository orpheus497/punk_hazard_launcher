/*
 * sysmon.h -- the numbers in the bottom panel.
 *
 * Read straight from the kernel through sysctl(3) on FreeBSD.  No libstatgrab,
 * no libgtop, no parsing the output of top(1): those are all wrappers around
 * the same three sysctls this file reads directly, and each one would be a
 * dependency, a fork, or both, sixty times a second.
 *
 * A Linux path exists for development builds only; it reads /proc, which is
 * the equivalent native interface there.  The two are kept in separate
 * #ifdef blocks rather than behind a pretend-portable abstraction, because
 * they genuinely are different interfaces with different semantics.
 */
#ifndef PH_SYSMON_H
#define PH_SYSMON_H

#include "ph.h"

struct ph_sysmon {
	int    ok;                  /* did anything at all read back?      */
	int    ncpu;
	double cpu_pct;             /* 0..100, aggregate across all CPUs   */
	double mem_pct;             /* 0..100                              */
	unsigned long mem_used_mb;
	unsigned long mem_total_mb;
	double load[3];             /* 1, 5, 15 minute                     */
	long   uptime_sec;
	char   os[160];             /* "FreeBSD 14.2-RELEASE"; utsname fields
	                             * are up to 65 bytes each, so 2x65+2  */
	char   host[64];

	/* internals: the previous CPU tick counts, for the delta */
	long   prev[8];
	int    have_prev;
	float  accum;               /* rate limiter                        */
};

void ph_sysmon_init(struct ph_sysmon *s);
/* Cheap to call every frame: it only re-reads a few times a second. */
void ph_sysmon_update(struct ph_sysmon *s, float dt);

#endif /* PH_SYSMON_H */
