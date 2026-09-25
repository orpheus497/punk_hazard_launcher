/*
 * launch.c -- running a game and accounting for the time it took.
 *
 * The launcher is the parent process.  It forks, the child becomes the
 * game, and the launcher blocks in waitpid(2) until the game exits.  It
 * does not daemonise the game or hand it off to a session manager: if you
 * kill the launcher, the thing you launched from it goes too, which is the
 * behaviour you want from a console front-end.
 *
 * The caller is expected to hide or minimise its window first -- see
 * app.c, which drops out of fullscreen so the game gets the display.
 */
#include "ph.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Elapsed time comes from CLOCK_MONOTONIC, not from time(2).
 *
 * CLOCK_MONOTONIC counts from an arbitrary fixed point and is unaffected
 * by the administrator setting the clock or by NTP stepping it.  Using
 * wall-clock time here would let a clock adjustment during a long session
 * record a negative or wildly inflated playtime.  The wall clock is still
 * the right source for "when did I last play this", which is a date, so
 * last_played uses time(2) and only the duration uses the monotonic clock.
 */
static unsigned long
elapsed_seconds(const struct timespec *a, const struct timespec *b)
{
	double d;

	d = (double)(b->tv_sec - a->tv_sec) +
	    (double)(b->tv_nsec - a->tv_nsec) / 1e9;
	return d > 0.0 ? (unsigned long)d : 0ul;
}

int
ph_launch(struct ph_game *g, struct ph_run_result *r)
{
	struct timespec t0, t1;
	char exec[PH_PATH_MAX], work[PH_PATH_MAX];
	char **words = NULL, *store = NULL, **argv = NULL;
	int nwords, i, status;

	if (r != NULL) {
		r->status = -1;
		r->seconds = 0;
	}
	if (g->exec[0] == '\0') {
		ph_warn("%s: no exec set in the manifest", g->slug);
		return -1;
	}
	if (ph_game_exec_path(g, exec, sizeof(exec)) != 0) {
		ph_warn("%s: exec path too long", g->slug);
		return -1;
	}
	if (!ph_is_exec(exec)) {
		ph_warn("%s: not executable: %s", g->slug, exec);
		return -1;
	}
	if (ph_game_work_path(g, work, sizeof(work)) != 0 || !ph_is_dir(work)) {
		ph_warn("%s: bad working directory", g->slug);
		return -1;
	}

	/*
	 * argv[0] is the full path we exec.  Some games look at argv[0] to
	 * find their own data directory, so giving them the real path is
	 * more useful than giving them a bare name.
	 */
	nwords = ph_argsplit(g->args, &words, &store);
	argv = ph_xcalloc((size_t)nwords + 2, sizeof(*argv));
	argv[0] = exec;
	for (i = 0; i < nwords; i++)
		argv[i + 1] = words[i];
	argv[nwords + 1] = NULL;

	ph_info("launching %s (cwd %s)", exec, work);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	status = ph_spawn(work, argv, -1);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	free(argv);
	free(words);
	free(store);

	if (status < 0) {
		ph_warn("%s: could not run", g->slug);
		return -1;
	}
	if (status == 127)
		ph_warn("%s: exec failed (command not found)", g->slug);
	else if (status != 0)
		ph_info("%s exited with status %d", g->slug, status);

	/* Book-keeping. A session shorter than 5s is almost always a crash
	 * or a mis-set exec line, so it counts as a launch but adds no
	 * playtime -- otherwise a broken entry silently accumulates hours. */
	{
		unsigned long secs = elapsed_seconds(&t0, &t1);

		g->play_count++;
		g->last_played = time(NULL);
		if (secs >= 5)
			g->play_seconds += secs;
		if (r != NULL) {
			r->status = status;
			r->seconds = secs;
		}
	}
	if (ph_game_save(g) != 0)
		ph_warn("%s: could not update manifest: %s", g->slug,
		    strerror(errno));
	return status;
}
