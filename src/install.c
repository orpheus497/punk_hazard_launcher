/*
 * install.c -- putting a game into the library.
 *
 * The user hands us one of four things and we normalise all of them into
 * the same on-disk shape (<games>/<slug>/root/ plus a manifest):
 *
 *   1. an archive        -- extracted with tar(1), then treated as (2)
 *   2. a source tree     -- built with a recipe from lua/install.lua,
 *                           then treated as (3)
 *   3. a directory       -- copied wholesale, then scanned for an entry point
 *   4. a single binary   -- copied as the entry point
 *
 * Nothing here goes through a shell.  Archives are extracted and builds are
 * run with fork(2)+execvp(3) via ph_spawn(), so a directory called
 * `; rm -rf ~` is just an awkward directory name.
 */
#include "ph.h"
#include "script.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */
static const char *
base_name(const char *p)
{
	const char *s = strrchr(p, '/');

	return (s != NULL && s[1] != '\0') ? s + 1 : p;
}

static int
has_suffix(const char *s, const char *suf)
{
	size_t ls = strlen(s), lf = strlen(suf);

	return ls >= lf && ph_ieq(s + ls - lf, suf);
}

/* Recognised archive extensions.  FreeBSD's tar(1) is bsdtar, built on
 * libarchive, so it reads every one of these including .zip; GNU tar does
 * not read zip, hence the unzip fallback in extract(). */
static int
is_archive(const char *path)
{
	static const char *const ext[] = {
		".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz", ".tbz2",
		".tar.xz", ".txz", ".tar.zst", ".tzst", ".zip", NULL
	};
	int i;

	for (i = 0; ext[i] != NULL; i++)
		if (has_suffix(path, ext[i]))
			return 1;
	return 0;
}

/* Strip one known archive extension to get a default title. */
static void
strip_archive_ext(char *s)
{
	static const char *const ext[] = {
		".tar.gz", ".tar.bz2", ".tar.xz", ".tar.zst", ".tar",
		".tgz", ".tbz2", ".tbz", ".txz", ".tzst", ".zip", NULL
	};
	int i;

	for (i = 0; ext[i] != NULL; i++) {
		size_t ls = strlen(s), lf = strlen(ext[i]);

		if (ls > lf && ph_ieq(s + ls - lf, ext[i])) {
			s[ls - lf] = '\0';
			return;
		}
	}
}

static int
open_log(const char *dir)
{
	char p[PH_PATH_MAX];

	if (ph_join(p, sizeof(p), dir, "install.log") != 0)
		return -1;
	return open(p, O_WRONLY | O_CREAT | O_APPEND, 0644);
}

/* ------------------------------------------------------------------ *
 * Archive extraction
 * ------------------------------------------------------------------ */
static int
extract(const char *archive, const char *dest)
{
	char *argv_tar[]   = { "tar", "-x", "-f", NULL, "-C", NULL, NULL };
	char *argv_bsd[]   = { "bsdtar", "-x", "-f", NULL, "-C", NULL, NULL };
	char *argv_unzip[] = { "unzip", "-q", "-o", NULL, "-d", NULL, NULL };
	char **argv;
	char arch[PH_PATH_MAX], dst[PH_PATH_MAX];

	if (ph_mkdirp(dest, 0755) != 0)
		return -1;
	/* execvp(3) wants char *const argv[]; copy rather than cast away
	 * const, so the compiler keeps checking us. */
	if (strlcpy(arch, archive, sizeof(arch)) >= sizeof(arch) ||
	    strlcpy(dst, dest, sizeof(dst)) >= sizeof(dst))
		return -1;

	if (has_suffix(archive, ".zip")) {
		if (ph_have_cmd("bsdtar"))
			argv = argv_bsd;
		else if (ph_have_cmd("unzip"))
			argv = argv_unzip;
		else
			argv = argv_tar;   /* FreeBSD's tar is bsdtar anyway */
	} else {
		argv = ph_have_cmd("tar") ? argv_tar : argv_bsd;
	}
	argv[3] = arch;
	argv[5] = dst;

	ph_info("extracting with %s", argv[0]);
	if (ph_spawn(NULL, argv, -1) != 0) {
		ph_warn("%s failed on %s", argv[0], archive);
		return -1;
	}
	return 0;
}

/*
 * If the extracted tree is a single directory (the polite tarball shape),
 * descend into it so the library does not end up with
 * root/foo-1.2.3/foo-1.2.3/bin/foo.
 */
static void
descend_single_dir(char *dir, size_t dirsize)
{
	char only[PH_PATH_MAX], probe[PH_PATH_MAX];
	struct dirent *de;
	DIR *d;
	int count = 0;

	if ((d = opendir(dir)) == NULL)
		return;
	only[0] = '\0';
	while ((de = readdir(d)) != NULL) {
		if (de->d_name[0] == '.')
			continue;
		if (++count > 1)
			break;
		strlcpy(only, de->d_name, sizeof(only));
	}
	closedir(d);

	if (count == 1 && only[0] != '\0' &&
	    ph_join(probe, sizeof(probe), dir, only) == 0 && ph_is_dir(probe))
		strlcpy(dir, probe, dirsize);
}

/* ------------------------------------------------------------------ *
 * Entry-point discovery
 *
 * After a copy or a build we have a tree and need to guess which file the
 * user actually wants to run.  Scoring beats a fixed rule because real
 * source trees disagree wildly about where the binary lands.
 * ------------------------------------------------------------------ */
struct cand {
	char rel[PH_PATH_MAX];
	int  score;
};

static int
skip_dir(const char *n)
{
	static const char *const skip[] = {
		".git", ".svn", ".hg", "CMakeFiles", ".deps", ".libs",
		"node_modules", "__pycache__", NULL
	};
	int i;

	for (i = 0; skip[i] != NULL; i++)
		if (strcmp(n, skip[i]) == 0)
			return 1;
	return 0;
}

static int
looks_unrunnable(const char *n)
{
	static const char *const bad[] = {
		".so", ".o", ".a", ".la", ".h", ".c", ".txt", ".md",
		".png", ".wad", ".cfg", NULL
	};
	static const char *const names[] = {
		"configure", "config.status", "libtool", "config.guess",
		"config.sub", "install-sh", "compile", "depcomp", NULL
	};
	int i;

	for (i = 0; bad[i] != NULL; i++)
		if (has_suffix(n, bad[i]))
			return 1;
	for (i = 0; names[i] != NULL; i++)
		if (strcmp(n, names[i]) == 0)
			return 1;
	return 0;
}

static void
scan_exec(const char *root, const char *rel, int depth, const char *slug,
    struct cand *best)
{
	char abs[PH_PATH_MAX];
	struct dirent *de;
	DIR *d;

	if (depth > 6)
		return;
	if (ph_join(abs, sizeof(abs), root, rel) != 0)
		return;
	if ((d = opendir(abs)) == NULL)
		return;

	while ((de = readdir(d)) != NULL) {
		char childrel[PH_PATH_MAX], childabs[PH_PATH_MAX];
		struct stat st;

		if (de->d_name[0] == '.')
			continue;
		if (ph_join(childrel, sizeof(childrel), rel, de->d_name) != 0)
			continue;
		if (ph_join(childabs, sizeof(childabs), root, childrel) != 0)
			continue;
		if (lstat(childabs, &st) != 0)
			continue;

		if (S_ISDIR(st.st_mode)) {
			if (!skip_dir(de->d_name))
				scan_exec(root, childrel, depth + 1, slug, best);
			continue;
		}
		if (!S_ISREG(st.st_mode) || access(childabs, X_OK) != 0)
			continue;
		if (looks_unrunnable(de->d_name))
			continue;

		{
			char nameslug[PH_SLUG_MAX];
			const char *parent = base_name(rel);
			int score = 50 - depth * 6;

			ph_slug(nameslug, sizeof(nameslug), de->d_name);
			if (strcmp(nameslug, slug) == 0)
				score += 120;
			else if (ph_icontains(de->d_name, slug) ||
			         ph_icontains(slug, nameslug))
				score += 55;
			if (strcmp(parent, "bin") == 0)
				score += 45;
			if (strchr(de->d_name, '.') == NULL)
				score += 15;	/* classic UNIX binary name */
			if (st.st_size > 65536)
				score += 10;	/* not a two-line wrapper */

			if (score > best->score) {
				best->score = score;
				strlcpy(best->rel, childrel, sizeof(best->rel));
			}
		}
	}
	closedir(d);
}

static int
find_exec(const char *root, const char *slug, char *out, size_t outsize)
{
	struct cand best;

	best.score = -1;
	best.rel[0] = '\0';
	scan_exec(root, "", 0, slug, &best);
	if (best.rel[0] == '\0')
		return -1;
	/* ph_join(x, "") yields a leading "./" style path; normalise. */
	strlcpy(out, best.rel[0] == '/' ? best.rel + 1 : best.rel, outsize);
	return 0;
}

/* ------------------------------------------------------------------ *
 * Building
 * ------------------------------------------------------------------ */
static int
run_recipe(const char *dir, struct ph_recipe *r, int logfd)
{
	int i;

	for (i = 0; i < r->nsteps; i++) {
		char *argv[PH_STEP_MAX_ARGS + 1];
		int j;

		for (j = 0; j < r->steps[i].argc; j++)
			argv[j] = r->steps[i].arg[j];
		argv[j] = NULL;

		ph_info("build step: %s", argv[0]);
		if (logfd >= 0)
			dprintf(logfd, "\n=== %s: %s ===\n", r->name, argv[0]);
		if (ph_spawn(dir, argv, logfd) != 0) {
			ph_warn("build step '%s' failed (see install.log)",
			    argv[0]);
			return -1;
		}
	}
	return 0;
}

/* ------------------------------------------------------------------ *
 * Slug allocation
 * ------------------------------------------------------------------ */
static int
unique_slug(const struct ph_paths *p, const char *want, char *out, size_t outsize)
{
	char probe[PH_PATH_MAX];
	int n;

	if (strlcpy(out, want, outsize) >= outsize)
		return -1;
	for (n = 2; n < 1000; n++) {
		if (ph_join(probe, sizeof(probe), p->games, out) != 0)
			return -1;
		if (!ph_is_dir(probe))
			return 0;
		if ((size_t)snprintf(out, outsize, "%s-%d", want, n) >= outsize)
			return -1;
	}
	return -1;
}

/* ------------------------------------------------------------------ *
 * ph_install
 * ------------------------------------------------------------------ */
int
ph_install(const struct ph_paths *p, const char *src,
    const struct ph_install_opts *o, char *slug_out, size_t slug_size)
{
	struct ph_install_opts none;
	struct ph_script *sc = NULL;
	struct ph_game g;
	char abssrc[PH_PATH_MAX];
	char staging[PH_PATH_MAX] = "";
	char payload[PH_PATH_MAX];
	char gamedir[PH_PATH_MAX], gameroot[PH_PATH_MAX];
	char slug[PH_SLUG_MAX], title[PH_TITLE_MAX];
	int  logfd = -1, rc = -1, src_is_file;

	if (o == NULL) {
		memset(&none, 0, sizeof(none));
		none.build = 1;
		o = &none;
	}

	/*
	 * realpath(3) is explicit about its buffer: "The resolved_path
	 * argument must point to a buffer capable of storing at least
	 * PATH_MAX characters, or be NULL."
	 *
	 * PH_PATH_MAX is our own bound and is NOT PATH_MAX -- it is 1024,
	 * while PATH_MAX is 4096 on Linux -- so handing realpath one of our
	 * buffers is a real overflow, not a theoretical one.  Passing NULL
	 * instead makes realpath allocate exactly what the resolved path
	 * needs; the man page notes the result "must be freed by the
	 * caller".  That also sidesteps PATH_MAX differing per platform.
	 */
	{
		char *rp;

		if ((rp = realpath(src, NULL)) == NULL) {
			ph_warn("%s: %s", src, strerror(errno));
			return -1;
		}
		if (strlcpy(abssrc, rp, sizeof(abssrc)) >= sizeof(abssrc)) {
			ph_warn("%s: resolved path is too long", src);
			free(rp);
			return -1;
		}
		free(rp);
	}
	src_is_file = ph_is_file(abssrc);
	if (!src_is_file && !ph_is_dir(abssrc)) {
		ph_warn("%s: not a file or directory", abssrc);
		return -1;
	}

	/* --- title and slug ------------------------------------------- */
	if (o->title != NULL && *o->title != '\0') {
		strlcpy(title, o->title, sizeof(title));
	} else {
		strlcpy(title, base_name(abssrc), sizeof(title));
		strip_archive_ext(title);
	}
	{
		char want[PH_SLUG_MAX];

		ph_slug(want, sizeof(want), title);
		if (ph_mkdirp(p->games, 0755) != 0) {
			ph_warn("cannot create %s: %s", p->games, strerror(errno));
			return -1;
		}
		if (unique_slug(p, want, slug, sizeof(slug)) != 0) {
			ph_warn("cannot allocate a slug for '%s'", title);
			return -1;
		}
	}
	if (ph_join(gamedir, sizeof(gamedir), p->games, slug) != 0 ||
	    ph_join(gameroot, sizeof(gameroot), gamedir, "root") != 0) {
		ph_warn("path too long for '%s'", slug);
		return -1;
	}
	if (ph_mkdirp(gamedir, 0755) != 0) {
		ph_warn("cannot create %s: %s", gamedir, strerror(errno));
		return -1;
	}
	logfd = open_log(gamedir);

	/* --- 1. archive -> staging ------------------------------------ */
	strlcpy(payload, abssrc, sizeof(payload));
	if (src_is_file && is_archive(abssrc)) {
		/*
		 * --link records absolute paths and copies nothing, but an
		 * archive has to be unpacked somewhere first, and that staging
		 * directory is removed when this function returns.  The two
		 * together would leave a manifest pointing into a deleted
		 * tree, so refuse rather than produce a broken entry.
		 */
		if (o->link_only) {
			ph_warn("--link cannot be used with an archive: "
			    "extract %s yourself, then --link the directory",
			    abssrc);
			goto out;
		}
		if (snprintf(staging, sizeof(staging), "%s/.staging-%ld",
		    p->root, (long)getpid()) >= (int)sizeof(staging))
			goto out;
		ph_info("staging archive in %s", staging);
		if (extract(abssrc, staging) != 0)
			goto out;
		strlcpy(payload, staging, sizeof(payload));
		descend_single_dir(payload, sizeof(payload));
		src_is_file = 0;
	}

	ph_game_init(&g);
	strlcpy(g.slug, slug, sizeof(g.slug));
	strlcpy(g.dir, gamedir, sizeof(g.dir));
	strlcpy(g.title, title, sizeof(g.title));

	/* --- 2. place the payload ------------------------------------- */
	if (o->link_only) {
		/*
		 * Register in place: nothing is copied, and exec/workdir are
		 * absolute.  For a game that is already installed elsewhere
		 * (a port, a Steam depot, a shared NFS mount) copying it
		 * would only waste the disk.
		 */
		if (src_is_file) {
			char *cut;

			strlcpy(g.exec, payload, sizeof(g.exec));
			strlcpy(g.workdir, payload, sizeof(g.workdir));
			if ((cut = strrchr(g.workdir, '/')) != NULL)
				*cut = '\0';
			else
				strlcpy(g.workdir, ".", sizeof(g.workdir));
		} else {
			char rel[PH_PATH_MAX];

			if (o->exec != NULL && *o->exec != '\0')
				strlcpy(rel, o->exec, sizeof(rel));
			else if (find_exec(payload, slug, rel, sizeof(rel)) != 0) {
				ph_warn("no executable found under %s", payload);
				goto out;
			}
			if (ph_join(g.exec, sizeof(g.exec), payload, rel) != 0)
				goto out;
			strlcpy(g.workdir, payload, sizeof(g.workdir));
		}
	} else {
		if (ph_mkdirp(gameroot, 0755) != 0)
			goto out;
		if (src_is_file) {
			char dst[PH_PATH_MAX];

			if (ph_join(dst, sizeof(dst), gameroot,
			    base_name(payload)) != 0)
				goto out;
			ph_info("copying %s", payload);
			if (ph_copy_file(payload, dst) != 0) {
				ph_warn("copy failed: %s", strerror(errno));
				goto out;
			}
			chmod(dst, 0755);
			strlcpy(g.exec, base_name(payload), sizeof(g.exec));
		} else {
			ph_info("copying tree %s -> %s", payload, gameroot);
			if (ph_copy_tree(payload, gameroot) != 0)
				ph_warn("some files could not be copied");
		}

		/* --- 3. build, if this looks like source ------------------ */
		if (!src_is_file && o->build) {
			struct ph_recipe r;

			sc = ph_script_open(p);
			if (sc != NULL && ph_script_recipe(sc, gameroot, &r)) {
				ph_info("building with recipe '%s'", r.name);
				if (run_recipe(gameroot, &r, logfd) != 0)
					ph_warn("build failed; the tree was "
					    "still imported, set exec= by hand");
			}
		}

		/* --- 4. find the entry point ----------------------------- */
		if (o->exec != NULL && *o->exec != '\0') {
			strlcpy(g.exec, o->exec, sizeof(g.exec));
		} else if (g.exec[0] == '\0') {
			char rel[PH_PATH_MAX];

			if (find_exec(gameroot, slug, rel, sizeof(rel)) != 0) {
				ph_warn("no executable found under %s -- "
				    "edit %s/manifest and set exec=",
				    gameroot, gamedir);
			} else {
				strlcpy(g.exec, rel, sizeof(g.exec));
				ph_info("entry point: %s", rel);
			}
		}
	}

	/* --- 5. metadata ---------------------------------------------- */
	if (o->genre != NULL)     strlcpy(g.genre, o->genre, sizeof(g.genre));
	if (o->developer != NULL) strlcpy(g.developer, o->developer, sizeof(g.developer));
	if (o->year != NULL)      strlcpy(g.year, o->year, sizeof(g.year));
	if (o->desc != NULL)      strlcpy(g.desc, o->desc, sizeof(g.desc));
	if (o->args != NULL)      strlcpy(g.args, o->args, sizeof(g.args));

	if (o->cover != NULL && *o->cover != '\0' && ph_is_file(o->cover)) {
		char dst[PH_PATH_MAX];
		const char *e = strrchr(o->cover, '.');

		if (ph_join(dst, sizeof(dst), gamedir,
		    (e != NULL && ph_ieq(e, ".jpg")) ? "cover.jpg" : "cover.png") == 0 &&
		    ph_copy_file(o->cover, dst) == 0)
			strlcpy(g.cover, dst, sizeof(g.cover));
	}

	if (ph_game_save(&g) != 0) {
		ph_warn("cannot write manifest: %s", strerror(errno));
		goto out;
	}
	if (slug_out != NULL)
		strlcpy(slug_out, slug, slug_size);
	rc = 0;

out:
	if (sc != NULL)
		ph_script_close(sc);
	if (logfd >= 0)
		close(logfd);
	if (staging[0] != '\0')
		ph_rmtree(staging);
	if (rc != 0)
		ph_rmtree(gamedir);	/* leave no half-installed entry */
	return rc;
}

int
ph_uninstall(const struct ph_paths *p, const char *slug)
{
	char dir[PH_PATH_MAX];
	char clean[PH_SLUG_MAX];

	/*
	 * Re-slug what we were given before touching the filesystem.  The
	 * slug becomes a path component, and ph_slug() cannot produce "..",
	 * a '/' or an empty string -- so there is no input to this function
	 * that can make it delete something outside <games>/.
	 */
	ph_slug(clean, sizeof(clean), slug);
	if (strcmp(clean, slug) != 0) {
		ph_warn("no such game: %s", slug);
		return -1;
	}
	if (ph_join(dir, sizeof(dir), p->games, clean) != 0)
		return -1;
	if (!ph_is_dir(dir)) {
		ph_warn("no such game: %s", slug);
		return -1;
	}
	if (ph_rmtree(dir) != 0) {
		ph_warn("could not fully remove %s", dir);
		return -1;
	}
	return 0;
}
