/*
 * main.c -- command line.
 *
 * The GUI is one front end, not the only one.  Every operation the
 * launcher performs is reachable from the shell, output is plain text with
 * tab-separated fields, and exit status means what it always means.  That
 * is what lets a library be built by a script, backed up with tar(1) and
 * inspected with grep(1) -- the launcher is not the only way in.
 *
 *     punkhazard                       run the GUI
 *     punkhazard add <path> [options]  import a game
 *     punkhazard ls                    list the library, one game per line
 *     punkhazard info <slug>           print one manifest
 *     punkhazard run <slug>            launch without the GUI
 *     punkhazard rm <slug>             remove a game
 *     punkhazard paths                 print the resolved directories
 */
#include "ph.h"

#include <stdlib.h>
#include <string.h>

static const char usage_text[] =
"usage: " PH_NAME " [-v] [command [arguments]]\n"
"\n"
"commands:\n"
"  ui                     run the graphical launcher (the default)\n"
"  add <path> [options]   import a game: a binary, a directory, a source\n"
"                         tree, or an archive (tar/gz/bz2/xz/zst/zip)\n"
"  ls                     list the library, tab-separated\n"
"  info <slug>            print one game's manifest\n"
"  run <slug>             launch a game directly, without the GUI\n"
"  rm <slug>              remove a game and its files\n"
"  paths                  print the directories in use\n"
"  version                print the version\n"
"  help                   this message\n"
"\n"
"add options:\n"
"  --title <s>      display name        (default: the file or directory name)\n"
"  --exec <path>    entry point, relative to the imported tree\n"
"  --args <s>       arguments passed to the game\n"
"  --genre <s>      --developer <s>     --year <s>\n"
"  --desc <s>       one-line description\n"
"  --cover <file>   box art (png/jpg), copied into the library\n"
"  --no-build       import sources without running a build recipe\n"
"  --link           register the game where it is; copy nothing\n"
"\n"
"environment:\n"
"  PUNKHAZARD_ROOT  library location\n"
"                   (default: $XDG_DATA_HOME/" PH_NAME ",\n"
"                    else ~/.local/share/" PH_NAME ")\n"
"  PUNKHAZARD_LUA   directory holding the Lua policy files\n"
"                   (default: " PH_DATADIR "/lua)\n";

static int
cmd_paths(const struct ph_paths *p)
{
	printf("root\t%s\n",   p->root);
	printf("games\t%s\n",  p->games);
	printf("config\t%s\n", p->config);
	printf("lua\t%s\n",    p->lua);
	return 0;
}

static int
cmd_ls(const struct ph_paths *p)
{
	struct ph_lib lib;
	size_t i;

	ph_lib_init(&lib);
	if (ph_lib_scan(&lib, p) != 0) {
		ph_warn("cannot read %s", p->games);
		return 1;
	}
	ph_lib_sort(&lib, PH_SORT_TITLE);

	/* Tab-separated, one record per line: cut(1) and awk(1) friendly. */
	for (i = 0; i < lib.n; i++) {
		const struct ph_game *g = &lib.v[i];
		char played[32];

		ph_human_time(played, sizeof(played), g->play_seconds);
		printf("%s\t%s\t%s\t%s\t%s\t%lu\t%s\n",
		    g->slug,
		    g->title,
		    g->genre[0] != '\0' ? g->genre : "-",
		    g->year[0]  != '\0' ? g->year  : "-",
		    played,
		    g->play_count,
		    g->favorite ? "fav" : "-");
	}
	ph_lib_free(&lib);
	return 0;
}

static int
cmd_info(const struct ph_paths *p, const char *slug)
{
	struct ph_lib lib;
	struct ph_game *g;
	char buf[PH_PATH_MAX], when[48];
	int rc = 1;

	ph_lib_init(&lib);
	if (ph_lib_scan(&lib, p) != 0)
		return 1;
	if ((g = ph_lib_find(&lib, slug)) == NULL) {
		ph_warn("no such game: %s", slug);
		goto out;
	}
	printf("slug\t%s\n",      g->slug);
	printf("title\t%s\n",     g->title);
	printf("genre\t%s\n",     g->genre);
	printf("developer\t%s\n", g->developer);
	printf("year\t%s\n",      g->year);
	printf("dir\t%s\n",       g->dir);
	if (ph_game_exec_path(g, buf, sizeof(buf)) == 0)
		printf("exec\t%s\n", buf);
	if (ph_game_work_path(g, buf, sizeof(buf)) == 0)
		printf("workdir\t%s\n", buf);
	printf("args\t%s\n",   g->args);
	printf("cover\t%s\n",  g->cover);
	ph_human_date(when, sizeof(when), g->added);
	printf("added\t%s\n", when);
	ph_human_date(when, sizeof(when), g->last_played);
	printf("last_played\t%s\n", when);
	ph_human_time(when, sizeof(when), g->play_seconds);
	printf("playtime\t%s\n", when);
	printf("launches\t%lu\n", g->play_count);
	printf("favorite\t%d\n",  g->favorite);
	if (g->desc[0] != '\0')
		printf("desc\t%s\n", g->desc);
	rc = 0;
out:
	ph_lib_free(&lib);
	return rc;
}

static int
cmd_run(const struct ph_paths *p, const char *slug)
{
	struct ph_lib lib;
	struct ph_game *g;
	struct ph_run_result res;
	int rc = 1;

	ph_lib_init(&lib);
	if (ph_lib_scan(&lib, p) != 0)
		return 1;
	if ((g = ph_lib_find(&lib, slug)) == NULL) {
		ph_warn("no such game: %s", slug);
		goto out;
	}
	/* Pass the game's own exit status straight through, so
	 * `punkhazard run x && echo ok` behaves the way it reads. */
	rc = ph_launch(g, &res);
	if (rc < 0)
		rc = 1;
out:
	ph_lib_free(&lib);
	return rc;
}

static int
cmd_rm(const struct ph_paths *p, const char *slug)
{
	if (ph_uninstall(p, slug) != 0)
		return 1;
	printf("removed %s\n", slug);
	return 0;
}

static int
cmd_add(const struct ph_paths *p, int argc, char **argv)
{
	struct ph_install_opts o;
	char slug[PH_SLUG_MAX];
	const char *src = NULL;
	int i;

	memset(&o, 0, sizeof(o));
	o.build = 1;

	for (i = 0; i < argc; i++) {
		const char *a = argv[i];

#define OPT(name, field)						\
		if (strcmp(a, "--" name) == 0) {			\
			if (++i >= argc) {				\
				ph_warn("--" name " needs a value");	\
				return 2;				\
			}						\
			o.field = argv[i];				\
			continue;					\
		}
		OPT("title",     title)
		OPT("exec",      exec)
		OPT("args",      args)
		OPT("genre",     genre)
		OPT("developer", developer)
		OPT("dev",       developer)
		OPT("year",      year)
		OPT("desc",      desc)
		OPT("cover",     cover)
#undef OPT
		if (strcmp(a, "--no-build") == 0) { o.build = 0; continue; }
		if (strcmp(a, "--link") == 0)     { o.link_only = 1; continue; }
		if (a[0] == '-' && a[1] != '\0') {
			ph_warn("unknown option: %s", a);
			return 2;
		}
		if (src != NULL) {
			ph_warn("more than one path given");
			return 2;
		}
		src = a;
	}
	if (src == NULL) {
		ph_warn("add: no path given");
		fputs(usage_text, stderr);
		return 2;
	}
	if (ph_install(p, src, &o, slug, sizeof(slug)) != 0)
		return 1;

	printf("%s\n", slug);
	fprintf(stderr, PH_NAME ": installed '%s' as %s/%s\n",
	    o.title != NULL ? o.title : src, p->games, slug);
	return 0;
}

int
main(int argc, char **argv)
{
	struct ph_paths paths;
	const char *cmd;
	int i = 1;

	/* A global -v before the command; everything after belongs to it. */
	while (i < argc && strcmp(argv[i], "-v") == 0) {
		ph_verbose_set(1);
		i++;
	}

	ph_paths_init(&paths);
	cmd = (i < argc) ? argv[i++] : "ui";

	if (strcmp(cmd, "help") == 0 || strcmp(cmd, "-h") == 0 ||
	    strcmp(cmd, "--help") == 0) {
		fputs(usage_text, stdout);
		return 0;
	}
	if (strcmp(cmd, "version") == 0 || strcmp(cmd, "--version") == 0) {
		printf("%s %s\n", PH_NAME, PH_VERSION);
		return 0;
	}
	if (strcmp(cmd, "paths") == 0)
		return cmd_paths(&paths);
	if (strcmp(cmd, "ls") == 0 || strcmp(cmd, "list") == 0)
		return cmd_ls(&paths);
	if (strcmp(cmd, "add") == 0 || strcmp(cmd, "install") == 0)
		return cmd_add(&paths, argc - i, argv + i);

	if (strcmp(cmd, "info") == 0 || strcmp(cmd, "run") == 0 ||
	    strcmp(cmd, "rm") == 0 || strcmp(cmd, "remove") == 0) {
		if (i >= argc) {
			ph_warn("%s: needs a game name", cmd);
			return 2;
		}
		if (strcmp(cmd, "info") == 0)
			return cmd_info(&paths, argv[i]);
		if (strcmp(cmd, "run") == 0)
			return cmd_run(&paths, argv[i]);
		return cmd_rm(&paths, argv[i]);
	}

	if (strcmp(cmd, "ui") == 0) {
		/* Create the library directory on first run so the GUI has
		 * somewhere to scan and the user has somewhere to put
		 * things. */
		if (ph_mkdirp(paths.games, 0755) != 0)
			ph_warn("cannot create %s", paths.games);
		return ph_app_run(&paths);
	}

	ph_warn("unknown command: %s", cmd);
	fputs(usage_text, stderr);
	return 2;
}
