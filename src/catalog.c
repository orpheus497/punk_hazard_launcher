/*
 * catalog.c -- the library on disk.
 *
 * LAYOUT
 * ------
 *   <root>/                       PUNKHAZARD_ROOT, else $XDG_DATA_HOME/punkhazard,
 *   |                             else ~/.local/share/punkhazard
 *   +-- config.lua                user overrides, optional
 *   +-- games/
 *       +-- <slug>/
 *           +-- manifest          the record below
 *           +-- cover.png         box art, optional
 *           +-- install.log       transcript of the build/install
 *           +-- root/             the game's own files
 *
 * MANIFEST FORMAT
 * ---------------
 * One "key = value" per line; '#' starts a comment; unknown keys are kept
 * out of the way rather than rejected, so a newer punkhazard writing extra
 * fields does not break an older one reading them.
 *
 * Why a flat text file and not SQLite: the whole point of this layout is
 * that it is inspectable and repairable with the tools already on a FreeBSD
 * base install.  grep(1) finds a game, ed(1) fixes a typo, tar(1) backs the
 * library up, and a half-written record is visible as text rather than as a
 * corrupt page. The catalog is a few hundred records read once at startup;
 * an embedded database would buy nothing and cost a dependency.
 *
 * Newlines inside a value are escaped as \n so that one record is always
 * one line -- that is what keeps the file greppable.
 */
#include "ph.h"

#include <dirent.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>	/* strcasecmp(3) lives here, per POSIX and FreeBSD */
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ *
 * Paths
 * ------------------------------------------------------------------ */
void
ph_paths_init(struct ph_paths *p)
{
	const char *env, *home;

	memset(p, 0, sizeof(*p));

	/*
	 * Root selection, in order of decreasing explicitness.  The XDG Base
	 * Directory spec puts variable application data under XDG_DATA_HOME,
	 * "defaulting to $HOME/.local/share".  A launcher's library is
	 * exactly that: data the user accumulates, not configuration.
	 */
	if ((env = getenv("PUNKHAZARD_ROOT")) != NULL && *env != '\0')
		strlcpy(p->root, env, sizeof(p->root));
	else if ((env = getenv("XDG_DATA_HOME")) != NULL && *env != '\0')
		ph_join(p->root, sizeof(p->root), env, PH_NAME);
	else if ((home = getenv("HOME")) != NULL && *home != '\0')
		ph_join(p->root, sizeof(p->root), home, ".local/share/" PH_NAME);
	else
		strlcpy(p->root, "." PH_NAME, sizeof(p->root));

	ph_join(p->games, sizeof(p->games), p->root, "games");
	ph_join(p->config, sizeof(p->config), p->root, "config.lua");

	/*
	 * Lua policy files.  PUNKHAZARD_LUA lets you run straight out of the
	 * build tree (`make run`); otherwise they come from where `make
	 * install` put them.  PH_DATADIR is baked in by the Makefile.
	 */
	if ((env = getenv("PUNKHAZARD_LUA")) != NULL && *env != '\0')
		strlcpy(p->lua, env, sizeof(p->lua));
	else
		strlcpy(p->lua, PH_DATADIR "/lua", sizeof(p->lua));
}

/* ------------------------------------------------------------------ *
 * Records
 * ------------------------------------------------------------------ */
void
ph_game_init(struct ph_game *g)
{
	memset(g, 0, sizeof(*g));
	strlcpy(g->workdir, ".", sizeof(g->workdir));
	strlcpy(g->icon, "\xef\x84\x9b", sizeof(g->icon));  /* U+F11B gamepad, Nerd Font */
	g->added = time(NULL);
}

/* value -> file: newline and backslash escaped so a record stays one line */
static void
escape_into(FILE *f, const char *s)
{
	for (; *s != '\0'; s++) {
		switch (*s) {
		case '\n': fputs("\\n", f); break;
		case '\r': break;
		case '\\': fputs("\\\\", f); break;
		default:   fputc(*s, f); break;
		}
	}
}

/* file -> value, in place */
static void
unescape(char *s)
{
	char *w = s;

	for (; *s != '\0'; s++) {
		if (*s != '\\') { *w++ = *s; continue; }
		switch (*++s) {
		case 'n':  *w++ = '\n'; break;
		case 't':  *w++ = '\t'; break;
		case '\\': *w++ = '\\'; break;
		case '\0': *w++ = '\\'; s--; break;
		default:   *w++ = '\\'; *w++ = *s; break;
		}
	}
	*w = '\0';
}

static void
set_field(struct ph_game *g, const char *k, char *v)
{
	unescape(v);

	if      (ph_ieq(k, "title"))     strlcpy(g->title, v, sizeof(g->title));
	else if (ph_ieq(k, "genre"))     strlcpy(g->genre, v, sizeof(g->genre));
	else if (ph_ieq(k, "developer")) strlcpy(g->developer, v, sizeof(g->developer));
	else if (ph_ieq(k, "year"))      strlcpy(g->year, v, sizeof(g->year));
	else if (ph_ieq(k, "exec"))      strlcpy(g->exec, v, sizeof(g->exec));
	else if (ph_ieq(k, "args"))      strlcpy(g->args, v, sizeof(g->args));
	else if (ph_ieq(k, "workdir"))   strlcpy(g->workdir, v, sizeof(g->workdir));
	else if (ph_ieq(k, "icon"))      strlcpy(g->icon, v, sizeof(g->icon));
	else if (ph_ieq(k, "desc"))      strlcpy(g->desc, v, sizeof(g->desc));
	else if (ph_ieq(k, "cover"))     strlcpy(g->cover, v, sizeof(g->cover));
	else if (ph_ieq(k, "added"))        g->added        = (time_t)strtoll(v, NULL, 10);
	else if (ph_ieq(k, "last_played"))  g->last_played  = (time_t)strtoll(v, NULL, 10);
	else if (ph_ieq(k, "play_count"))   g->play_count   = strtoul(v, NULL, 10);
	else if (ph_ieq(k, "play_seconds")) g->play_seconds = strtoul(v, NULL, 10);
	else if (ph_ieq(k, "favorite"))     g->favorite     = (int)strtol(v, NULL, 10);
	/* unknown keys: ignored, deliberately */
}

int
ph_manifest_read(struct ph_game *g, const char *path)
{
	char *line = NULL;
	size_t cap = 0;
	ssize_t len;
	FILE *f;

	if ((f = fopen(path, "r")) == NULL)
		return -1;
	/*
	 * getline(3), not fgets(3): fgets with a fixed buffer silently
	 * splits an over-long record, and the tail is then parsed as if it
	 * were the next line -- which can set a field from a fragment.  One
	 * line is one record, whatever its length.
	 */
	while ((len = getline(&line, &cap, f)) > 0) {
		char *eq, *k, *v;

		line[strcspn(line, "\n")] = '\0';
		if ((k = strchr(line, '#')) != NULL && k == line)
			continue;
		if ((eq = strchr(line, '=')) == NULL)
			continue;
		*eq = '\0';
		k = line;
		v = eq + 1;
		ph_trim(k);
		ph_trim(v);
		if (*k != '\0')
			set_field(g, k, v);
	}
	free(line);
	fclose(f);
	return 0;
}

/*
 * Write the manifest.  We write to "<path>.new" and rename(2) over the
 * target: rename is atomic within a filesystem, so a crash mid-write leaves
 * the previous manifest intact rather than a truncated one.  Play statistics
 * are written after every session, so this path runs often enough to matter.
 */
int
ph_manifest_write(const struct ph_game *g, const char *path)
{
	char tmp[PH_PATH_MAX];
	FILE *f;

	if ((size_t)snprintf(tmp, sizeof(tmp), "%s.new", path) >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		return -1;
	}
	if ((f = fopen(tmp, "w")) == NULL)
		return -1;

	fputs("# punkhazard game manifest\n", f);
	fputs("# one 'key = value' per line; '\\n' in a value is a newline\n", f);

#define PUT(k, v) do { fputs(k " = ", f); escape_into(f, (v)); fputc('\n', f); } while (0)
	PUT("title",     g->title);
	PUT("genre",     g->genre);
	PUT("developer", g->developer);
	PUT("year",      g->year);
	PUT("exec",      g->exec);
	PUT("args",      g->args);
	PUT("workdir",   g->workdir);
	PUT("icon",      g->icon);
	PUT("cover",     g->cover);
	PUT("desc",      g->desc);
#undef PUT
	fprintf(f, "added = %lld\n",        (long long)g->added);
	fprintf(f, "last_played = %lld\n",  (long long)g->last_played);
	fprintf(f, "play_count = %lu\n",    g->play_count);
	fprintf(f, "play_seconds = %lu\n",  g->play_seconds);
	fprintf(f, "favorite = %d\n",       g->favorite);

	/*
	 * Close on every path -- the previous `fflush(f) != 0 || fclose(f)`
	 * short-circuited and leaked the stream when the flush failed.
	 *
	 * fsync(2) before rename(2) is what makes the atomicity claim real:
	 * rename is atomic with respect to the directory entry, but it does
	 * not promise the file's *contents* reached the disk first.  Without
	 * the sync a crash can leave the entry pointing at a zero-length
	 * manifest, which is precisely the outcome the temp-file dance is
	 * meant to prevent.
	 */
	if (fflush(f) != 0) {
		fclose(f);
		unlink(tmp);
		return -1;
	}
	if (fsync(fileno(f)) != 0 && errno != EINVAL) {
		fclose(f);
		unlink(tmp);
		return -1;
	}
	if (fclose(f) != 0) {
		unlink(tmp);
		return -1;
	}
	if (rename(tmp, path) != 0) {
		unlink(tmp);
		return -1;
	}
	return 0;
}

int
ph_game_save(const struct ph_game *g)
{
	char mf[PH_PATH_MAX];

	if (ph_join(mf, sizeof(mf), g->dir, "manifest") != 0)
		return -1;
	return ph_manifest_write(g, mf);
}

int
ph_game_exec_path(const struct ph_game *g, char *dst, size_t dstsize)
{
	char base[PH_PATH_MAX];

	if (g->exec[0] == '\0')
		return -1;
	if (g->exec[0] == '/')			/* absolute: use verbatim */
		return strlcpy(dst, g->exec, dstsize) < dstsize ? 0 : -1;
	if (ph_join(base, sizeof(base), g->dir, "root") != 0)
		return -1;
	return ph_join(dst, dstsize, base, g->exec);
}

int
ph_game_work_path(const struct ph_game *g, char *dst, size_t dstsize)
{
	char base[PH_PATH_MAX];

	if (g->workdir[0] == '/')
		return strlcpy(dst, g->workdir, dstsize) < dstsize ? 0 : -1;
	if (ph_join(base, sizeof(base), g->dir, "root") != 0)
		return -1;
	if (g->workdir[0] == '\0' || strcmp(g->workdir, ".") == 0)
		return strlcpy(dst, base, dstsize) < dstsize ? 0 : -1;
	return ph_join(dst, dstsize, base, g->workdir);
}

/* ------------------------------------------------------------------ *
 * The library
 * ------------------------------------------------------------------ */
void
ph_lib_init(struct ph_lib *l)
{
	l->v = NULL;
	l->n = l->cap = 0;
}

void
ph_lib_free(struct ph_lib *l)
{
	free(l->v);
	ph_lib_init(l);
}

static struct ph_game *
lib_push(struct ph_lib *l)
{
	if (l->n == l->cap) {
		l->cap = l->cap ? l->cap * 2 : 32;
		l->v = ph_xrealloc(l->v, l->cap * sizeof(*l->v));
	}
	return &l->v[l->n++];
}

/* Fill in g->cover if the manifest did not name one. */
static void
find_cover(struct ph_game *g)
{
	static const char *const names[] = {
		"cover.png", "cover.jpg", "cover.jpeg", "cover.bmp",
		"box.png", "art.png", NULL
	};
	char abs[PH_PATH_MAX];

	if (g->cover[0] != '\0') {
		/* An absolute path is used only if it is really there; a
		 * stale one now falls through to the search below instead of
		 * leaving the game with a cover that cannot be opened. */
		if (g->cover[0] == '/') {
			if (ph_is_file(g->cover))
				return;
			g->cover[0] = '\0';
		} else if (ph_join(abs, sizeof(abs), g->dir, g->cover) == 0 &&
		    ph_is_file(abs)) {
			strlcpy(g->cover, abs, sizeof(g->cover));
			return;
		}
	}
	if (ph_find_first(g->dir, names, abs, sizeof(abs)) == 0)
		strlcpy(g->cover, abs, sizeof(g->cover));
	else
		g->cover[0] = '\0';
}

int
ph_lib_scan(struct ph_lib *l, const struct ph_paths *p)
{
	struct dirent *de;
	DIR *d;

	ph_lib_free(l);
	ph_lib_init(l);

	if ((d = opendir(p->games)) == NULL)
		return errno == ENOENT ? 0 : -1;   /* empty library is fine */

	while ((de = readdir(d)) != NULL) {
		char dir[PH_PATH_MAX], mf[PH_PATH_MAX];
		struct ph_game g;

		if (de->d_name[0] == '.')
			continue;
		if (ph_join(dir, sizeof(dir), p->games, de->d_name) != 0)
			continue;
		if (!ph_is_dir(dir))
			continue;
		if (ph_join(mf, sizeof(mf), dir, "manifest") != 0 ||
		    !ph_is_file(mf))
			continue;

		ph_game_init(&g);
		strlcpy(g.slug, de->d_name, sizeof(g.slug));
		strlcpy(g.dir, dir, sizeof(g.dir));
		if (ph_manifest_read(&g, mf) != 0) {
			ph_warn("unreadable manifest: %s", mf);
			continue;
		}
		if (g.title[0] == '\0')		/* tolerate a hand-made record */
			strlcpy(g.title, g.slug, sizeof(g.title));
		find_cover(&g);
		*lib_push(l) = g;
	}
	closedir(d);
	return 0;
}

struct ph_game *
ph_lib_find(struct ph_lib *l, const char *slug)
{
	size_t i;

	for (i = 0; i < l->n; i++)
		if (strcmp(l->v[i].slug, slug) == 0)
			return &l->v[i];
	return NULL;
}

int
ph_lib_match(const struct ph_game *g, const char *query)
{
	if (query == NULL || *query == '\0')
		return 1;
	return ph_icontains(g->title, query) ||
	       ph_icontains(g->genre, query) ||
	       ph_icontains(g->developer, query) ||
	       ph_icontains(g->year, query);
}

/* ------------------------------------------------------------------ *
 * Sorting.  Favourites always float to the top; the chosen key orders the
 * rest.  Every comparator falls back to the title so the order is total
 * and therefore stable to look at between runs.
 * ------------------------------------------------------------------ */
static int
by_title(const void *a, const void *b)
{
	const struct ph_game *x = a, *y = b;
	int c;

	if (x->favorite != y->favorite)
		return y->favorite - x->favorite;
	if ((c = strcasecmp(x->title, y->title)) != 0)
		return c;
	return strcmp(x->slug, y->slug);
}

#define CMP_DESC(field)							\
	const struct ph_game *x = a, *y = b;				\
	if (x->favorite != y->favorite)					\
		return y->favorite - x->favorite;			\
	if (x->field != y->field)					\
		return x->field < y->field ? 1 : -1;			\
	return by_title(a, b)

static int by_recent(const void *a, const void *b) { CMP_DESC(last_played); }
static int by_played(const void *a, const void *b) { CMP_DESC(play_seconds); }
static int by_added (const void *a, const void *b) { CMP_DESC(added); }
#undef CMP_DESC

static int
by_year(const void *a, const void *b)
{
	const struct ph_game *x = a, *y = b;
	long xy, yy;

	if (x->favorite != y->favorite)
		return y->favorite - x->favorite;
	xy = strtol(x->year, NULL, 10);
	yy = strtol(y->year, NULL, 10);
	if (xy != yy)
		return xy < yy ? 1 : -1;	/* newest first */
	return by_title(a, b);
}

void
ph_lib_sort(struct ph_lib *l, enum ph_sort how)
{
	int (*cmp)(const void *, const void *);

	switch (how) {
	case PH_SORT_RECENT: cmp = by_recent; break;
	case PH_SORT_PLAYED: cmp = by_played; break;
	case PH_SORT_ADDED:  cmp = by_added;  break;
	case PH_SORT_YEAR:   cmp = by_year;   break;
	case PH_SORT_TITLE:
	default:             cmp = by_title;  break;
	}
	if (l->n > 1)
		qsort(l->v, l->n, sizeof(*l->v), cmp);
}

const char *
ph_sort_name(enum ph_sort s)
{
	switch (s) {
	case PH_SORT_TITLE:  return "TITLE";
	case PH_SORT_RECENT: return "RECENT";
	case PH_SORT_PLAYED: return "PLAYTIME";
	case PH_SORT_ADDED:  return "ADDED";
	case PH_SORT_YEAR:   return "YEAR";
	default:             return "?";
	}
}
