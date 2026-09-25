/*
 * ph.h -- punkhazard: shared types and the core C interface.
 *
 * One header for the whole program.  A game launcher is not big enough to
 * justify a web of private headers; one contract everybody reads is easier
 * to hold in your head than twelve.
 */
#ifndef PH_H
#define PH_H

/*
 * Feature-test namespace.  This must precede every system header, which is
 * why ph.h is the first include in every translation unit.
 *
 * FreeBSD (the target): deliberately define NOTHING.  <sys/cdefs.h> sets
 * __BSD_VISIBLE to 1 by default, which is what exposes strlcpy(3) and
 * strlcat(3) from libc.  Defining _POSIX_C_SOURCE here would *hide* them
 * and force us onto strncpy(3), which does not NUL-terminate on truncation.
 *
 * Linux (host builds for development): glibc keeps strlcpy/strlcat behind
 * _DEFAULT_SOURCE, and only provides them at all from glibc 2.38.  util.c
 * carries a fallback for older libcs.
 */
#ifdef __linux__
#  ifndef _DEFAULT_SOURCE
#    define _DEFAULT_SOURCE 1
#  endif
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <sys/types.h>
#include <string.h>

/*
 * strlcpy(3)/strlcat(3) compatibility.
 *
 * These are BSD-native: on FreeBSD they are in libc and declared by
 * <string.h> (strlcpy(3): "LIBRARY: libc", "SYNOPSIS: #include <string.h>").
 * They are the correct tool here because, unlike strncpy(3), they always
 * NUL-terminate and they report the length they *wanted* to write, which is
 * exactly the truncation check we need.
 *
 * glibc only adopted them in 2.38.  On an older glibc we supply our own;
 * everywhere BSD-derived we use libc's.
 */
#if !defined(__FreeBSD__) && !defined(__OpenBSD__) && !defined(__NetBSD__) && \
    !defined(__DragonFly__) && !defined(__APPLE__)
#  if defined(__GLIBC__) && (__GLIBC__ < 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ < 38))
#    define PH_PROVIDE_STRL 1
size_t strlcpy(char *dst, const char *src, size_t dstsize);
size_t strlcat(char *dst, const char *src, size_t dstsize);
#  endif
#endif

#define PH_NAME     "punkhazard"
#define PH_VERSION  "0.1.0"

/*
 * Fixed-size records.  A launcher holds a few hundred games; chasing a
 * malloc per string would cost more in allocator traffic and cache misses
 * than the slack costs in RAM, and every field below has a natural bound.
 * sizeof(struct ph_game) is ~6KB, so 1000 games is 6MB -- irrelevant.
 */
#define PH_PATH_MAX   1024
#define PH_TITLE_MAX   160
#define PH_DESC_MAX   2048
#define PH_ARGS_MAX    512
#define PH_SLUG_MAX     96

/* ------------------------------------------------------------------ *
 * Colour
 * ------------------------------------------------------------------ */
/*
 * 0xRRGGBBAA.  Stored packed because that is how themes are written in Lua
 * (0x1affaaff reads as a hex colour) and how they are handed to the GPU.
 */
typedef uint32_t ph_rgba;

#define PH_R(c) ((float)(((c) >> 24) & 0xffu) / 255.0f)
#define PH_G(c) ((float)(((c) >> 16) & 0xffu) / 255.0f)
#define PH_B(c) ((float)(((c) >>  8) & 0xffu) / 255.0f)
#define PH_A(c) ((float)(((c)      ) & 0xffu) / 255.0f)

/* ------------------------------------------------------------------ *
 * A game
 * ------------------------------------------------------------------ */
struct ph_game {
	char slug[PH_SLUG_MAX];        /* filesystem-safe id, also the dirname */
	char title[PH_TITLE_MAX];
	char genre[64];
	char developer[96];
	char year[8];

	char exec[PH_PATH_MAX];        /* relative to <dir>/root, or absolute */
	char args[PH_ARGS_MAX];        /* shell-style word list, see ph_argsplit */
	char workdir[PH_PATH_MAX];     /* relative to <dir>/root; "." by default */
	char icon[8];                  /* one UTF-8 glyph, usually a Nerd Font icon */

	char desc[PH_DESC_MAX];
	char dir[PH_PATH_MAX];         /* <library>/games/<slug> */
	char cover[PH_PATH_MAX];       /* absolute path to cover art, or "" */

	time_t        added;
	time_t        last_played;
	unsigned long play_count;
	unsigned long play_seconds;
	int           favorite;

	/* Runtime-only, never written to the manifest. */
	unsigned cover_tex;            /* GL texture name, 0 = not loaded */
	int      cover_w, cover_h;
	int      cover_tried;          /* so a missing cover is not retried each frame */
};

struct ph_lib {
	struct ph_game *v;
	size_t          n, cap;
};

/* ------------------------------------------------------------------ *
 * Where things live
 * ------------------------------------------------------------------ */
struct ph_paths {
	char root[PH_PATH_MAX];    /* the library root */
	char games[PH_PATH_MAX];   /* <root>/games */
	char lua[PH_PATH_MAX];     /* directory the Lua policy files came from */
	char config[PH_PATH_MAX];  /* <root>/config.lua, user overrides */
};

/* ------------------------------------------------------------------ *
 * Theme and layout: the data Lua owns, C only consumes
 * ------------------------------------------------------------------ */
struct ph_theme {
	char    name[64];
	ph_rgba bg, panel, panel_alt, frame;
	ph_rgba text, text_dim, text_bright;
	ph_rgba accent, accent2, danger, ok;
	ph_rgba sel_bg, sel_fg, shadow;
	/* CRT post-process parameters, all 0..1 unless noted */
	float   scanline;      /* scanline darkening              */
	float   curvature;     /* barrel distortion               */
	float   vignette;      /* corner falloff                  */
	float   aberration;    /* chromatic fringing, in pixels   */
	float   glow;          /* phosphor bloom                  */
	float   noise;         /* animated grain                  */
};

struct ph_layout {
	int   pad;
	int   top_h;           /* top panel: wordmark + selected-game detail */
	int   bottom_h;        /* bottom panel: system monitor               */
	float split;           /* left fraction of the working area, 0..1    */
	int   tile_w;          /* target tile width; the column count is
	                        * derived from it so the grid reflows        */
	int   tile_gap;
	float tile_aspect;     /* cover height / width, 1.333 = 3:4 box art  */
	int   font_px;         /* body text size   */
	int   font_px_title;   /* header text size */
	int   font_px_small;
	float anim_speed;      /* selection easing, higher = snappier */
};

struct ph_config {
	int  win_w, win_h;
	int  fullscreen;
	int  vsync;
	int  crt;              /* CRT post-process on/off */
	int  show_fps;
	char theme[64];
};

/* ------------------------------------------------------------------ *
 * Semantic input actions (what the UI reacts to, not what was pressed)
 * ------------------------------------------------------------------ */
enum ph_action {
	PH_ACT_NONE = 0,
	PH_ACT_UP, PH_ACT_DOWN, PH_ACT_LEFT, PH_ACT_RIGHT,
	PH_ACT_PAGE_UP, PH_ACT_PAGE_DOWN, PH_ACT_HOME, PH_ACT_END,
	PH_ACT_LAUNCH, PH_ACT_BACK, PH_ACT_QUIT,
	PH_ACT_FULLSCREEN, PH_ACT_SEARCH, PH_ACT_DETAILS,
	PH_ACT_FAVORITE, PH_ACT_SORT, PH_ACT_CRT, PH_ACT_THEME,
	PH_ACT_RELOAD, PH_ACT_HELP,
	PH_ACT__COUNT
};

/* ------------------------------------------------------------------ *
 * util.c -- errors
 * ------------------------------------------------------------------ */
void  ph_fatal(const char *fmt, ...);
void  ph_warn(const char *fmt, ...);
void  ph_info(const char *fmt, ...);
void  ph_verbose_set(int on);

/* util.c -- memory (these abort rather than return NULL) */
void *ph_xmalloc(size_t n);
void *ph_xcalloc(size_t n, size_t sz);
void *ph_xrealloc(void *p, size_t n);
char *ph_xstrdup(const char *s);

/* util.c -- strings */
void  ph_slug(char *dst, size_t dstsize, const char *src);
void  ph_trim(char *s);
int   ph_ieq(const char *a, const char *b);
int   ph_icontains(const char *hay, const char *needle);
void  ph_human_time(char *dst, size_t dstsize, unsigned long seconds);
void  ph_human_date(char *dst, size_t dstsize, time_t t);
/* Split a shell-ish word list. Returns count; fills argv with pointers into
 * a scratch copy stored in *store, which the caller frees. */
int   ph_argsplit(const char *s, char ***argv_out, char **store);

/* util.c -- UTF-8. Returns the code point and advances *s. */
uint32_t ph_utf8_next(const char **s);
size_t   ph_utf8_len(const char *s);

/* util.c -- filesystem */
int   ph_join(char *dst, size_t dstsize, const char *a, const char *b);
int   ph_mkdirp(const char *path, mode_t mode);
int   ph_is_dir(const char *path);
int   ph_is_file(const char *path);
int   ph_is_exec(const char *path);
int   ph_copy_file(const char *src, const char *dst);
int   ph_copy_tree(const char *src, const char *dst);
int   ph_rmtree(const char *path);
char *ph_read_file(const char *path, size_t *len_out);
int   ph_find_first(const char *dir, const char *const *names, char *out, size_t outsize);

/* util.c -- processes.  fork(2)+execvp(3), never system(3). */
int   ph_spawn(const char *cwd, char *const argv[], int logfd);
int   ph_have_cmd(const char *name);

/* ------------------------------------------------------------------ *
 * catalog.c -- the library on disk
 * ------------------------------------------------------------------ */
enum ph_sort {
	PH_SORT_TITLE = 0,
	PH_SORT_RECENT,
	PH_SORT_PLAYED,
	PH_SORT_ADDED,
	PH_SORT_YEAR,
	PH_SORT__COUNT
};
const char *ph_sort_name(enum ph_sort s);

void  ph_paths_init(struct ph_paths *p);
void  ph_game_init(struct ph_game *g);
int   ph_manifest_read(struct ph_game *g, const char *path);
int   ph_manifest_write(const struct ph_game *g, const char *path);
int   ph_game_save(const struct ph_game *g);

void  ph_lib_init(struct ph_lib *l);
void  ph_lib_free(struct ph_lib *l);
int   ph_lib_scan(struct ph_lib *l, const struct ph_paths *p);
void  ph_lib_sort(struct ph_lib *l, enum ph_sort how);
struct ph_game *ph_lib_find(struct ph_lib *l, const char *slug);
int   ph_lib_match(const struct ph_game *g, const char *query);
/* Absolute path to the executable this game should run. */
int   ph_game_exec_path(const struct ph_game *g, char *dst, size_t dstsize);
int   ph_game_work_path(const struct ph_game *g, char *dst, size_t dstsize);

/* ------------------------------------------------------------------ *
 * install.c -- putting a game into the library
 * ------------------------------------------------------------------ */
struct ph_install_opts {
	const char *title;
	const char *exec;
	const char *genre;
	const char *developer;
	const char *year;
	const char *desc;
	const char *cover;
	const char *args;
	int         build;      /* run the build step if sources are detected */
	int         link_only;  /* register in place instead of copying       */
};
int ph_install(const struct ph_paths *p, const char *src,
    const struct ph_install_opts *o, char *slug_out, size_t slug_size);
int ph_uninstall(const struct ph_paths *p, const char *slug);

/* ------------------------------------------------------------------ *
 * launch.c -- running a game
 * ------------------------------------------------------------------ */
struct ph_run_result {
	int           status;      /* exit status, or 128+signo */
	unsigned long seconds;     /* measured on CLOCK_MONOTONIC */
};
int ph_launch(struct ph_game *g, struct ph_run_result *r);

/* ------------------------------------------------------------------ *
 * app.c -- the SDL window and the main loop
 * ------------------------------------------------------------------ */
int ph_app_run(const struct ph_paths *p);

#endif /* PH_H */
