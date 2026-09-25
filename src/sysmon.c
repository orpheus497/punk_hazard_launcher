/*
 * sysmon.c -- CPU, memory, load and uptime.
 */
#include "sysmon.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/utsname.h>
#include <unistd.h>

#ifdef __FreeBSD__
#  include <sys/types.h>
#  include <sys/sysctl.h>
#  include <sys/time.h>
#  include <sys/resource.h>		/* CPUSTATES, CP_IDLE */
#endif

/* How often the kernel is actually asked. Twice a second is well past what
 * a human reads off a panel, and it keeps the cost invisible next to a
 * frame. */
#define SAMPLE_INTERVAL 0.5f

void
ph_sysmon_init(struct ph_sysmon *s)
{
	struct utsname u;

	memset(s, 0, sizeof(*s));
	s->ncpu = 1;

	/* uname(3) is POSIX and says the same thing on both platforms, so it
	 * is used for the text even where sysctl would also work. */
	if (uname(&u) == 0) {
		snprintf(s->os, sizeof(s->os), "%s %s", u.sysname, u.release);
		strlcpy(s->host, u.nodename, sizeof(s->host));
	} else {
		strlcpy(s->os, "unknown", sizeof(s->os));
	}
	s->accum = SAMPLE_INTERVAL;	/* sample immediately on first update */
}

#ifdef __FreeBSD__
/* ------------------------------------------------------------------ *
 * FreeBSD
 * ------------------------------------------------------------------ */
static int
sysctl_int(const char *name, int *out)
{
	size_t len = sizeof(*out);

	return sysctlbyname(name, out, &len, NULL, 0);
}

static int
sysctl_u32(const char *name, uint32_t *out)
{
	size_t len = sizeof(*out);

	return sysctlbyname(name, out, &len, NULL, 0);
}

static void
sample(struct ph_sysmon *s)
{
	long cp[CPUSTATES];
	size_t len;
	int pagesize = 4096, n;

	s->ok = 1;

	if (sysctl_int("hw.ncpu", &n) == 0 && n > 0)
		s->ncpu = n;
	if (sysctl_int("hw.pagesize", &n) == 0 && n > 0)
		pagesize = n;

	/*
	 * CPU.  kern.cp_time is an array of CPUSTATES longs holding *ticks
	 * since boot* in each state (user, nice, sys, intr, idle), so a
	 * single read says nothing -- utilisation is the difference between
	 * two reads:
	 *
	 *     busy% = 100 * (1 - idle_delta / total_delta)
	 */
	len = sizeof(cp);
	if (sysctlbyname("kern.cp_time", cp, &len, NULL, 0) == 0) {
		long total = 0, idle, i;

		for (i = 0; i < CPUSTATES; i++)
			total += cp[i];
		idle = cp[CP_IDLE];

		if (s->have_prev) {
			long dtotal = total, didle = idle, j;

			for (j = 0; j < CPUSTATES; j++)
				dtotal -= s->prev[j];
			didle -= s->prev[CP_IDLE];
			if (dtotal > 0) {
				s->cpu_pct = 100.0 * (1.0 -
				    (double)didle / (double)dtotal);
				if (s->cpu_pct < 0.0)   s->cpu_pct = 0.0;
				if (s->cpu_pct > 100.0) s->cpu_pct = 100.0;
			}
		}
		for (i = 0; i < CPUSTATES; i++)
			s->prev[i] = cp[i];
		s->have_prev = 1;
	}

	/*
	 * Memory.  Page counts come from the vm.stats.vm node.  "Used" is
	 * taken as active + wired + laundry: inactive pages are still
	 * reclaimable on demand, so counting them as used would report a
	 * healthy FreeBSD box as permanently full -- which is the classic
	 * way of misreading this interface.
	 */
	{
		uint32_t total_pg = 0, act = 0, wire = 0, laundry = 0;

		if (sysctl_u32("vm.stats.vm.v_page_count", &total_pg) == 0 &&
		    total_pg > 0) {
			uint64_t used_pg;

			sysctl_u32("vm.stats.vm.v_active_count", &act);
			sysctl_u32("vm.stats.vm.v_wire_count", &wire);
			sysctl_u32("vm.stats.vm.v_laundry_count", &laundry);

			used_pg = (uint64_t)act + wire + laundry;
			if (used_pg > total_pg)
				used_pg = total_pg;
			s->mem_total_mb = (unsigned long)
			    ((uint64_t)total_pg * pagesize / (1024 * 1024));
			s->mem_used_mb = (unsigned long)
			    (used_pg * pagesize / (1024 * 1024));
			s->mem_pct = 100.0 * (double)used_pg / (double)total_pg;
		}
	}

	/* Uptime: kern.boottime is a struct timeval of the wall-clock moment
	 * the kernel started. */
	{
		struct timeval bt;

		len = sizeof(bt);
		if (sysctlbyname("kern.boottime", &bt, &len, NULL, 0) == 0 &&
		    bt.tv_sec > 0)
			s->uptime_sec = (long)(time(NULL) - bt.tv_sec);
	}
}

#else
/* ------------------------------------------------------------------ *
 * Linux (development builds): /proc
 * ------------------------------------------------------------------ */
static void
sample(struct ph_sysmon *s)
{
	FILE *f;
	char line[256];

	s->ok = 1;
	s->ncpu = (int)sysconf(_SC_NPROCESSORS_ONLN);
	if (s->ncpu < 1)
		s->ncpu = 1;

	/* /proc/stat "cpu" line: the same cumulative-ticks model as
	 * kern.cp_time, with more states. */
	if ((f = fopen("/proc/stat", "r")) != NULL) {
		long v[8];
		int n;

		if (fgets(line, (int)sizeof(line), f) != NULL &&
		    strncmp(line, "cpu ", 4) == 0) {
			n = sscanf(line + 4, "%ld %ld %ld %ld %ld %ld %ld %ld",
			    &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
			if (n >= 4) {
				long total = 0, idle = v[3], i;

				for (i = 0; i < n; i++)
					total += v[i];
				if (s->have_prev) {
					long dtotal = total, didle = idle;

					for (i = 0; i < 8; i++)
						dtotal -= s->prev[i];
					didle -= s->prev[3];
					if (dtotal > 0) {
						s->cpu_pct = 100.0 * (1.0 -
						    (double)didle / (double)dtotal);
						if (s->cpu_pct < 0.0)   s->cpu_pct = 0.0;
						if (s->cpu_pct > 100.0) s->cpu_pct = 100.0;
					}
				}
				for (i = 0; i < 8; i++)
					s->prev[i] = (i < n) ? v[i] : 0;
				s->have_prev = 1;
			}
		}
		fclose(f);
	}

	if ((f = fopen("/proc/meminfo", "r")) != NULL) {
		unsigned long total = 0, avail = 0;

		while (fgets(line, (int)sizeof(line), f) != NULL) {
			if (sscanf(line, "MemTotal: %lu kB", &total) == 1)
				continue;
			if (sscanf(line, "MemAvailable: %lu kB", &avail) == 1)
				break;
		}
		fclose(f);
		if (total > 0) {
			unsigned long used = (avail < total) ? total - avail : 0;

			s->mem_total_mb = total / 1024;
			s->mem_used_mb = used / 1024;
			s->mem_pct = 100.0 * (double)used / (double)total;
		}
	}

	if ((f = fopen("/proc/uptime", "r")) != NULL) {
		double up = 0.0;

		if (fscanf(f, "%lf", &up) == 1)
			s->uptime_sec = (long)up;
		fclose(f);
	}
}
#endif

void
ph_sysmon_update(struct ph_sysmon *s, float dt)
{
	if ((s->accum += dt) < SAMPLE_INTERVAL)
		return;
	s->accum = 0.0f;

	sample(s);

	/* getloadavg(3) is in libc on FreeBSD and on glibc, and returns the
	 * same three figures uptime(1) prints. */
	if (getloadavg(s->load, 3) != 3)
		s->load[0] = s->load[1] = s->load[2] = 0.0;
}
