/*
 * ui.c -- the launcher front end.
 *
 * LAYOUT
 *
 *   +------------------------------------------------------+  top panel
 *   |  PUNK HAZARD        selected game, in detail         |
 *   +---------------------------------+--------------------+
 *   |                                 |  DETAILS           |
 *   |   game icon grid                |  cover/meta/desc   |
 *   |   (layout.split of the width)   |  ----------------  |
 *   |                                 |  OPTIONS           |
 *   +---------------------------------+--------------------+
 *   |  cpu   memory   load   uptime   host                 |  bottom panel
 *   +------------------------------------------------------+
 *
 * Immediate mode: every frame recomputes its rectangles from the window
 * size and the layout metrics and draws them.  The only persistent state is
 * what the user chose -- selection, focus, filter, sort, mode -- plus one
 * eased scroll value.  Resizing and DPI changes are therefore free: there
 * is no retained layout to invalidate.
 */
#include "ui.h"
#include "sysmon.h"

#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

/*
 * Nerd Font icons, in the Private Use Area.  These are why the typeface is
 * the patched Hurmit rather than plain Hermit: an unpatched font has
 * nothing at these code points and every one would come out blank.
 *
 * Every code point here was checked against the baked face with
 * stbtt_FindGlyphIndex before use.  The plain Unicode arrows U+2191/U+2193
 * and U+23CE RETURN SYMBOL are *not* in Hurmit, which is why the Font
 * Awesome equivalents are used instead.
 */
#define IC_PAD      "\xef\x84\x9b"	/* U+F11B gamepad     */
#define IC_STAR     "\xef\x80\x85"	/* U+F005 star        */
#define IC_SEARCH   "\xef\x80\x82"	/* U+F002 magnifier   */
#define IC_CLOCK    "\xef\x80\x97"	/* U+F017 clock       */
#define IC_TERM     "\xef\x84\xa0"	/* U+F120 terminal    */
#define IC_PLAY     "\xef\x81\x8b"	/* U+F04B play        */
#define IC_SORT     "\xef\x83\x9c"	/* U+F0DC sort        */
#define IC_REFRESH  "\xef\x80\xa1"	/* U+F021 refresh     */
#define IC_THEME    "\xef\x87\xbc"	/* U+F1FC paint-brush */
#define IC_CPU      "\xef\x8b\x9b"	/* U+F2DB microchip   */
#define IC_MEM      "\xef\x87\x80"	/* U+F1C0 database    */
#define IC_LOAD     "\xef\x83\xa7"	/* U+F0E7 bolt        */
#define IC_HOST     "\xef\x84\x89"	/* U+F109 laptop      */
#define IC_TAG      "\xef\x80\xab"	/* U+F02B tag         */
#define IC_TV       "\xef\x89\xac"	/* U+F26C television  */
#define IC_SCREEN   "\xef\x84\x88"	/* U+F108 desktop     */
#define IC_POWER    "\xef\x80\x91"	/* U+F011 power-off   */
#define IC_UP       "\xef\x81\xa2"	/* U+F062 arrow-up    */
#define IC_DOWN     "\xef\x81\xa3"	/* U+F063 arrow-down  */
#define IC_ENTER    "\xef\x85\x89"	/* U+F149 level-down  */
#define IC_TASKS    "\xef\x82\xae"	/* U+F0AE tasks       */

enum { MODE_LIBRARY = 0, MODE_SEARCH, MODE_HELP };
enum { FOCUS_GRID = 0, FOCUS_PANEL };

/*
 * The options panel.  A table rather than a pile of draw calls, so the
 * panel, the key help and the keyboard shortcuts can never disagree about
 * what exists -- they are all generated from this one list.
 */
static const struct {
	const char    *icon;
	const char    *label;
	const char    *key;
	enum ph_action act;
	int            needs_game;
} options[] = {
	{ IC_PLAY,    "LAUNCH",     "Enter", PH_ACT_LAUNCH,     1 },
	{ IC_STAR,    "FAVOURITE",  "B",     PH_ACT_FAVORITE,   1 },
	{ IC_SEARCH,  "SEARCH",     "/",     PH_ACT_SEARCH,     0 },
	{ IC_SORT,    "SORT",       "S",     PH_ACT_SORT,       0 },
	{ IC_THEME,   "THEME",      "T",     PH_ACT_THEME,      0 },
	{ IC_TV,      "CRT",        "C",     PH_ACT_CRT,        0 },
	{ IC_SCREEN,  "FULLSCREEN", "F11",   PH_ACT_FULLSCREEN, 0 },
	{ IC_REFRESH, "RESCAN",     "R",     PH_ACT_RELOAD,     0 },
	{ IC_POWER,   "QUIT",       "Q",     PH_ACT_QUIT,       0 },
};
#define NOPTIONS ((int)(sizeof(options) / sizeof(options[0])))

struct ph_ui {
	struct ph_gfx    *g;
	struct ph_script *sc;
	struct ph_lib    *lib;
	struct ph_config *cfg;
	struct ph_theme  *theme;
	struct ph_layout  lay;

	struct ph_font *f_body, *f_title, *f_small, *f_huge;
	struct ph_sysmon mon;

	size_t *view;			/* indices into lib->v after filtering */
	size_t  nview, capview;

	int    sel;
	int    focus;
	int    opt_sel;
	float  scroll, scroll_target;

	int    mode;
	char   query[96];
	enum ph_sort sort;
	int    theme_idx;

	enum ph_req     req;
	struct ph_game *req_game;

	char   toast[192];
	float  toast_t;

	int    mouse_x, mouse_y, hover, hover_opt;

	/* Grid geometry, recomputed each frame and cached for hit testing
	 * because the mouse handlers run between frames. */
	float  gx, gy, gw, gh;
	float  tile_w, tile_h;
	int    cols, rows_visible;
	float  px, py, pw, ph_;		/* the right-hand panel */
};

/* ------------------------------------------------------------------ *
 * Small helpers
 * ------------------------------------------------------------------ */
static ph_rgba
fade(ph_rgba c, float a)
{
	float base = PH_A(c) * a;
	unsigned v;

	if (base < 0.0f) base = 0.0f;
	if (base > 1.0f) base = 1.0f;
	v = (unsigned)(base * 255.0f + 0.5f);
	return (c & 0xffffff00u) | v;
}

/*
 * Frame-rate independent exponential approach.  Lerping by a constant
 * factor per frame would run at different speeds on a 60Hz and a 144Hz
 * display; 1-exp(-k*dt) is the closed form of the same easing sampled over
 * dt, so it does not.
 */
static float
approach(float cur, float target, float k, float dt)
{
	if (k <= 0.0f)
		return target;
	return cur + (target - cur) * (1.0f - expf(-k * dt));
}

static struct ph_game *
sel_game(struct ph_ui *u)
{
	if (u->nview == 0 || u->sel < 0 || (size_t)u->sel >= u->nview)
		return NULL;
	return &u->lib->v[u->view[u->sel]];
}

static float
row_pitch(const struct ph_ui *u)
{
	return u->tile_h + (float)u->lay.tile_gap;
}

/* ------------------------------------------------------------------ *
 * The filtered view
 * ------------------------------------------------------------------ */
static void
rebuild_view(struct ph_ui *u)
{
	struct ph_game *keep = sel_game(u);
	size_t i;

	if (u->lib->n > u->capview) {
		u->capview = u->lib->n + 16;
		u->view = ph_xrealloc(u->view, u->capview * sizeof(*u->view));
	}
	u->nview = 0;
	for (i = 0; i < u->lib->n; i++)
		if (ph_lib_match(&u->lib->v[i], u->query))
			u->view[u->nview++] = i;

	/* Keep the cursor on the same game across a re-filter or a re-sort
	 * where possible; nothing is more disorienting than a grid that
	 * jumps to the top every time you type a character. */
	u->sel = 0;
	if (keep != NULL) {
		for (i = 0; i < u->nview; i++) {
			if (&u->lib->v[u->view[i]] == keep) {
				u->sel = (int)i;
				break;
			}
		}
	}
	if (u->nview == 0)
		u->sel = 0;
	else if ((size_t)u->sel >= u->nview)
		u->sel = (int)u->nview - 1;
}

static void
ensure_visible(struct ph_ui *u)
{
	float pitch, top, bot, maxs;
	int row;

	if (u->gh <= 0.0f || u->cols < 1)
		return;
	pitch = row_pitch(u);
	row = u->sel / u->cols;
	top = (float)row * pitch;
	bot = top + u->tile_h;

	if (top < u->scroll_target)
		u->scroll_target = top;
	else if (bot > u->scroll_target + u->gh)
		u->scroll_target = bot - u->gh;

	{
		int nrows = ((int)u->nview + u->cols - 1) / u->cols;

		maxs = (float)nrows * pitch - (float)u->lay.tile_gap - u->gh;
	}
	if (maxs < 0.0f)
		maxs = 0.0f;
	if (u->scroll_target > maxs)
		u->scroll_target = maxs;
	if (u->scroll_target < 0.0f)
		u->scroll_target = 0.0f;
}

static void
move_sel(struct ph_ui *u, int delta)
{
	if (u->nview == 0)
		return;
	u->sel += delta;
	if (u->sel < 0)
		u->sel = 0;
	if ((size_t)u->sel >= u->nview)
		u->sel = (int)u->nview - 1;
	ensure_visible(u);
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */
struct ph_ui *
ph_ui_create(struct ph_gfx *g, struct ph_script *sc, struct ph_lib *lib,
    struct ph_config *cfg, struct ph_theme *theme, const struct ph_layout *lay)
{
	struct ph_ui *u = ph_xcalloc(1, sizeof(*u));

	u->g = g;
	u->sc = sc;
	u->lib = lib;
	u->cfg = cfg;
	u->theme = theme;
	u->lay = *lay;
	u->sort = PH_SORT_TITLE;
	u->hover = -1;
	u->hover_opt = -1;
	u->cols = 1;

	u->f_body  = ph_font_create(ph_font_regular, ph_font_regular_len, lay->font_px);
	u->f_small = ph_font_create(ph_font_regular, ph_font_regular_len, lay->font_px_small);
	u->f_title = ph_font_create(ph_font_bold,    ph_font_bold_len,    lay->font_px_title);
	/* The Mono variant of a Nerd Font scales its icons to a single
	 * character cell, so a "huge" icon needs a genuinely huge face to
	 * fill a tile. */
	u->f_huge  = ph_font_create(ph_font_bold,    ph_font_bold_len,    lay->font_px_title * 3);
	if (u->f_body == NULL || u->f_small == NULL || u->f_title == NULL) {
		ph_warn("ui: could not build the typeface");
		ph_ui_destroy(u);
		return NULL;
	}
	ph_sysmon_init(&u->mon);
	ph_lib_sort(u->lib, u->sort);
	rebuild_view(u);
	return u;
}

void
ph_ui_destroy(struct ph_ui *u)
{
	if (u == NULL)
		return;
	ph_font_destroy(u->f_body);
	ph_font_destroy(u->f_small);
	ph_font_destroy(u->f_title);
	ph_font_destroy(u->f_huge);
	free(u->view);
	free(u);
}

void
ph_ui_refresh(struct ph_ui *u)
{
	ph_lib_sort(u->lib, u->sort);
	rebuild_view(u);
	ensure_visible(u);
}

void
ph_ui_toast(struct ph_ui *u, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(u->toast, sizeof(u->toast), fmt, ap);
	va_end(ap);
	u->toast_t = 4.0f;
}

enum ph_req
ph_ui_poll(struct ph_ui *u, struct ph_game **out)
{
	enum ph_req r = u->req;

	if (out != NULL)
		*out = u->req_game;
	u->req = PH_REQ_NONE;
	u->req_game = NULL;
	return r;
}

int
ph_ui_is_searching(const struct ph_ui *u)
{
	return u != NULL && u->mode == MODE_SEARCH;
}

/* ------------------------------------------------------------------ *
 * Input
 * ------------------------------------------------------------------ */
static void
cycle_theme(struct ph_ui *u, int delta)
{
	char name[64];

	u->theme_idx += delta;
	if (ph_script_theme_name(u->sc, u->theme_idx, name, sizeof(name)) != 0)
		return;
	if (ph_script_theme(u->sc, name, u->theme) == 0) {
		strlcpy(u->cfg->theme, name, sizeof(u->cfg->theme));
		ph_ui_toast(u, "theme: %s", u->theme->name);
	}
}

void
ph_ui_action(struct ph_ui *u, enum ph_action a)
{
	int page = (u->cols > 0 && u->rows_visible > 0)
	    ? u->cols * u->rows_visible : 4;

	if (u->mode == MODE_HELP && a != PH_ACT_NONE) {
		if (a == PH_ACT_BACK || a == PH_ACT_HELP || a == PH_ACT_QUIT) {
			u->mode = MODE_LIBRARY;
			return;
		}
	}

	/*
	 * When the options panel has focus, the directional keys walk the
	 * option list and Enter activates the highlighted one.  Everything
	 * else keeps its usual meaning, so a shortcut still works from
	 * either side of the screen.
	 */
	if (u->focus == FOCUS_PANEL && u->mode != MODE_SEARCH) {
		switch (a) {
		case PH_ACT_UP:
			u->opt_sel = (u->opt_sel + NOPTIONS - 1) % NOPTIONS;
			return;
		case PH_ACT_DOWN:
			u->opt_sel = (u->opt_sel + 1) % NOPTIONS;
			return;
		case PH_ACT_LEFT:
		case PH_ACT_BACK:
			u->focus = FOCUS_GRID;
			return;
		case PH_ACT_LAUNCH:
			/* Activate the highlighted option instead of
			 * launching, unless it *is* launch. */
			if (options[u->opt_sel].act != PH_ACT_LAUNCH) {
				enum ph_action act = options[u->opt_sel].act;

				ph_ui_action(u, act);
				return;
			}
			break;
		case PH_ACT_DETAILS:
			u->focus = FOCUS_GRID;
			return;
		default:
			break;
		}
	}

	switch (a) {
	case PH_ACT_LEFT:      move_sel(u, -1); break;
	case PH_ACT_RIGHT:     move_sel(u, +1); break;
	case PH_ACT_UP:        move_sel(u, -u->cols); break;
	case PH_ACT_DOWN:      move_sel(u, +u->cols); break;
	case PH_ACT_PAGE_UP:   move_sel(u, -page); break;
	case PH_ACT_PAGE_DOWN: move_sel(u, +page); break;
	case PH_ACT_HOME:      u->sel = 0; ensure_visible(u); break;
	case PH_ACT_END:
		if (u->nview > 0)
			u->sel = (int)u->nview - 1;
		ensure_visible(u);
		break;

	case PH_ACT_DETAILS:	/* Tab moves focus between grid and panel */
		u->focus = (u->focus == FOCUS_GRID) ? FOCUS_PANEL : FOCUS_GRID;
		break;

	case PH_ACT_LAUNCH: {
		struct ph_game *g = sel_game(u);

		if (g == NULL) {
			ph_ui_toast(u, "library is empty");
			break;
		}
		if (g->exec[0] == '\0') {
			ph_ui_toast(u, "%s has no exec set in its manifest",
			    g->slug);
			break;
		}
		u->req = PH_REQ_LAUNCH;
		u->req_game = g;
		break;
	}

	case PH_ACT_BACK:
		if (u->mode == MODE_SEARCH || u->mode == MODE_HELP) {
			if (u->mode == MODE_SEARCH && u->query[0] != '\0') {
				u->query[0] = '\0';
				rebuild_view(u);
			}
			u->mode = MODE_LIBRARY;
		} else if (u->query[0] != '\0') {
			u->query[0] = '\0';
			rebuild_view(u);
		}
		break;

	case PH_ACT_QUIT:
		if (u->mode != MODE_LIBRARY)
			u->mode = MODE_LIBRARY;
		else
			u->req = PH_REQ_QUIT;
		break;

	case PH_ACT_SEARCH:
		u->mode = (u->mode == MODE_SEARCH) ? MODE_LIBRARY : MODE_SEARCH;
		if (u->mode == MODE_SEARCH)
			u->focus = FOCUS_GRID;
		break;

	case PH_ACT_HELP:
		u->mode = (u->mode == MODE_HELP) ? MODE_LIBRARY : MODE_HELP;
		break;

	case PH_ACT_FULLSCREEN: u->req = PH_REQ_FULLSCREEN; break;
	case PH_ACT_RELOAD:     u->req = PH_REQ_RELOAD; break;

	case PH_ACT_CRT:
		u->cfg->crt = !u->cfg->crt;
		ph_ui_toast(u, "CRT %s", u->cfg->crt ? "on" : "off");
		break;

	case PH_ACT_THEME: cycle_theme(u, +1); break;

	case PH_ACT_SORT:
		u->sort = (enum ph_sort)((u->sort + 1) % PH_SORT__COUNT);
		ph_lib_sort(u->lib, u->sort);
		rebuild_view(u);
		ensure_visible(u);
		ph_ui_toast(u, "sort: %s", ph_sort_name(u->sort));
		break;

	case PH_ACT_FAVORITE: {
		struct ph_game *g = sel_game(u);

		if (g == NULL)
			break;
		g->favorite = !g->favorite;
		if (ph_game_save(g) != 0)
			ph_ui_toast(u, "could not save %s", g->slug);
		else
			ph_ui_toast(u, "%s %s", g->title,
			    g->favorite ? "favourited" : "unfavourited");
		ph_lib_sort(u->lib, u->sort);
		rebuild_view(u);
		ensure_visible(u);
		break;
	}

	case PH_ACT_NONE:
	default:
		break;
	}
}

void
ph_ui_text(struct ph_ui *u, const char *utf8)
{
	size_t have, add;

	if (u->mode != MODE_SEARCH || utf8 == NULL)
		return;
	have = strlen(u->query);
	add = strlen(utf8);
	if (have + add + 1 >= sizeof(u->query))
		return;
	memcpy(u->query + have, utf8, add + 1);
	rebuild_view(u);
	ensure_visible(u);
}

/* Backspace, routed here by app.c because it is editing, not navigation. */
void
ph_ui_backspace(struct ph_ui *u)
{
	size_t n = strlen(u->query);

	if (u->mode != MODE_SEARCH || n == 0)
		return;
	/* Step back over a whole UTF-8 sequence, not one byte. */
	while (n > 0 && ((unsigned char)u->query[n - 1] & 0xc0) == 0x80)
		n--;
	if (n > 0)
		n--;
	u->query[n] = '\0';
	rebuild_view(u);
}

/* Which tile is under (x,y)?  -1 for none. */
static int
tile_at(struct ph_ui *u, int x, int y)
{
	float fx = (float)x, fy = (float)y, pitch;
	int col, row, idx;

	if (u->cols < 1 || u->tile_w <= 0.0f)
		return -1;
	if (fx < u->gx || fx > u->gx + u->gw || fy < u->gy || fy > u->gy + u->gh)
		return -1;
	pitch = row_pitch(u);
	col = (int)((fx - u->gx) / (u->tile_w + (float)u->lay.tile_gap));
	row = (int)((fy - u->gy + u->scroll) / pitch);
	if (col < 0 || col >= u->cols || row < 0)
		return -1;
	idx = row * u->cols + col;
	if (idx < 0 || (size_t)idx >= u->nview)
		return -1;
	return idx;
}

/* Which option row is under (x,y)?  -1 for none. */
static int
option_at(struct ph_ui *u, int x, int y)
{
	float rowh = ph_font_height(u->f_body) * 1.75f;
	float top = u->py + u->ph_ - (float)u->lay.pad - rowh * (float)NOPTIONS;
	int idx;

	if ((float)x < u->px || (float)x > u->px + u->pw)
		return -1;
	if ((float)y < top || (float)y > u->py + u->ph_)
		return -1;
	idx = (int)(((float)y - top) / rowh);
	return (idx >= 0 && idx < NOPTIONS) ? idx : -1;
}

void
ph_ui_mouse_move(struct ph_ui *u, int x, int y)
{
	u->mouse_x = x;
	u->mouse_y = y;
	u->hover = tile_at(u, x, y);
	u->hover_opt = option_at(u, x, y);
}

void
ph_ui_mouse_click(struct ph_ui *u, int x, int y, int clicks)
{
	int idx;

	if ((idx = option_at(u, x, y)) >= 0) {
		u->focus = FOCUS_PANEL;
		u->opt_sel = idx;
		ph_ui_action(u, options[idx].act);
		return;
	}
	if ((idx = tile_at(u, x, y)) < 0)
		return;
	u->focus = FOCUS_GRID;
	u->sel = idx;
	ensure_visible(u);
	if (clicks >= 2)		/* double-click launches, like a desktop */
		ph_ui_action(u, PH_ACT_LAUNCH);
}

void
ph_ui_mouse_wheel(struct ph_ui *u, int dy)
{
	float maxs, pitch = row_pitch(u);
	int nrows;

	if (u->cols < 1)
		return;
	u->scroll_target -= (float)dy * pitch * 0.5f;
	nrows = ((int)u->nview + u->cols - 1) / u->cols;
	maxs = (float)nrows * pitch - (float)u->lay.tile_gap - u->gh;
	if (maxs < 0.0f)
		maxs = 0.0f;
	if (u->scroll_target > maxs)
		u->scroll_target = maxs;
	if (u->scroll_target < 0.0f)
		u->scroll_target = 0.0f;
}

void
ph_ui_update(struct ph_ui *u, float dt)
{
	u->scroll = approach(u->scroll, u->scroll_target, u->lay.anim_speed, dt);
	if (fabsf(u->scroll - u->scroll_target) < 0.5f)
		u->scroll = u->scroll_target;
	if (u->toast_t > 0.0f)
		u->toast_t -= dt;
	ph_sysmon_update(&u->mon, dt);
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */

/*
 * A segmented bar meter.  Discrete cells rather than a smooth fill: it
 * reads at a glance from across a room, which is the whole point of a
 * console front end, and it is unmistakably of the era the theme is
 * imitating.
 */
static void
meter(struct ph_ui *u, float x, float y, float w, float h, double pct,
    ph_rgba on)
{
	const int cells = 14;
	float cw = w / (float)cells;
	int lit, i;

	if (pct < 0.0) pct = 0.0;
	if (pct > 100.0) pct = 100.0;
	lit = (int)(pct / 100.0 * cells + 0.5);

	for (i = 0; i < cells; i++) {
		ph_rgba c = (i < lit) ? on : fade(u->theme->frame, 0.75f);

		/* A hot meter earns a hotter colour; the eye catches that
		 * before it reads the number. */
		if (i < lit && pct > 85.0)
			c = u->theme->danger;
		else if (i < lit && pct > 65.0)
			c = u->theme->accent2;
		ph_gfx_rect(u->g, x + (float)i * cw, y, cw - 2.0f, h, c);
	}
}

static void
uptime_str(char *dst, size_t n, long secs)
{
	long d = secs / 86400, h = (secs % 86400) / 3600, m = (secs % 3600) / 60;

	if (secs <= 0)       snprintf(dst, n, "-");
	else if (d > 0)      snprintf(dst, n, "%ldd %ldh", d, h);
	else if (h > 0)      snprintf(dst, n, "%ldh %ldm", h, m);
	else                 snprintf(dst, n, "%ldm", m);
}

/* ---------------------------- top panel --------------------------- */
static void
draw_top(struct ph_ui *u, float W, float hh)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	struct ph_game *gm = sel_game(u);
	float pad = (float)u->lay.pad;
	float x, ty;
	/* Sized for the worst case: developer(96) + genre(64) + year(8) +
	 * two formatted dates and their labels. */
	char buf[512];

	ph_gfx_rect_vgrad(g, 0, 0, W, hh, t->panel_alt, t->panel);
	ph_gfx_rect(g, 0, hh - 2.0f, W, 2.0f, fade(t->accent, 0.55f));

	/* Wordmark: two words, two accents -- the identity of the launcher
	 * in one line. */
	ty = pad * 0.6f;
	x = pad;
	x = ph_text_draw(g, u->f_title, x, ty, "PUNK", t->accent);
	x = ph_text_draw(g, u->f_title, x + 8.0f, ty, "HAZARD", t->accent2);
	{
		static const char *const sub =
		    "v" PH_VERSION " \xc2\xb7 freebsd game launcher";
		float subend;

		ph_text_draw(g, u->f_small, pad + 2.0f,
		    ty + ph_font_height(u->f_title) * 0.92f, sub,
		    fade(t->text_dim, 0.85f));
		/* The subtitle is wider than the wordmark, so the column
		 * boundary has to clear the wider of the two -- otherwise
		 * the game detail line overlaps it. */
		subend = pad + 2.0f + ph_text_width(u->f_small, sub);
		if (subend > x)
			x = subend;
	}

	/* A vertical rule separates identity from content. */
	ph_gfx_rect(g, x + pad, pad * 0.5f, 1.0f, hh - pad, fade(t->frame, 0.9f));

	/* The selected game, in more detail than the tile can show. */
	{
		float dx = x + pad * 2.0f;
		float dw = W - dx - pad;
		float rx = W - pad;

		/* Right-hand status first, so the title knows how much room
		 * it really has. */
		snprintf(buf, sizeof(buf), "%s %zu/%zu   %s %s   %s %s",
		    IC_TASKS, u->nview, u->lib->n,
		    IC_SORT, ph_sort_name(u->sort),
		    IC_THEME, t->name);
		{
			float bw = ph_text_width(u->f_small, buf);

			ph_text_draw(g, u->f_small, rx - bw, ty + 2.0f, buf,
			    fade(t->text_dim, 0.9f));
			dw = (rx - bw - 16.0f) - dx;
		}

		if (gm == NULL) {
			ph_text_draw(g, u->f_title, dx, ty,
			    u->lib->n == 0 ? "NO GAMES INSTALLED" : "NO MATCH",
			    fade(t->text_dim, 0.8f));
			return;
		}

		ph_text_draw_clip(g, u->f_title, dx, ty, dw, gm->title,
		    t->text_bright);

		/* Second line: everything worth knowing without looking right. */
		{
			char played[32], when[48];
			float ly = ty + ph_font_height(u->f_title) * 0.92f;

			ph_human_time(played, sizeof(played), gm->play_seconds);
			ph_human_date(when, sizeof(when), gm->last_played);
			snprintf(buf, sizeof(buf),
			    "%s%s%s%s%s   %s %s   %s %lu launches   %s last %s",
			    gm->genre[0] ? gm->genre : "",
			    gm->genre[0] && gm->year[0] ? "  \xc2\xb7  " : "",
			    gm->year[0] ? gm->year : "",
			    (gm->genre[0] || gm->year[0]) && gm->developer[0]
			        ? "  \xc2\xb7  " : "",
			    gm->developer[0] ? gm->developer : "",
			    IC_CLOCK, played, IC_PLAY, gm->play_count,
			    IC_TAG, when);
			ph_text_draw_clip(g, u->f_small, dx, ly, dw, buf,
			    fade(t->text, 0.75f));
		}
	}
}

/* ------------------------------ grid ------------------------------ */
static void
draw_tile(struct ph_ui *u, struct ph_game *gm, float x, float y, int is_sel,
    int is_hov)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	float cw = u->tile_w;
	float ch = u->tile_w * u->lay.tile_aspect;
	float lab_y = y + ch + 7.0f;
	ph_rgba border;

	/* --- cover ------------------------------------------------- */
	if (!gm->cover_tried) {
		/* Load once, on first sight, and remember the failure too so
		 * a missing file is not re-opened sixty times a second. */
		gm->cover_tried = 1;
		if (gm->cover[0] != '\0')
			gm->cover_tex = ph_gfx_tex_file(gm->cover,
			    &gm->cover_w, &gm->cover_h);
	}

	ph_gfx_rect(g, x, y, cw, ch, fade(t->bg, 0.85f));

	if (gm->cover_tex != 0 && gm->cover_w > 0 && gm->cover_h > 0) {
		float ar = (float)gm->cover_w / (float)gm->cover_h;
		float dw = cw, dh = ch;

		if (ar > cw / ch)		/* letterbox, never distort */
			dh = cw / ar;
		else
			dw = ch * ar;
		ph_gfx_tex(g, gm->cover_tex, x + (cw - dw) * 0.5f,
		    y + (ch - dh) * 0.5f, dw, dh, 0, 0, 1, 1,
		    is_sel ? 0xffffffffu : 0xd8d8d8ffu);
	} else {
		const char *ic = gm->icon[0] != '\0' ? gm->icon : IC_PAD;
		float iw = ph_text_width(u->f_huge, ic);

		ph_gfx_rect_vgrad(g, x, y, cw, ch,
		    fade(t->panel_alt, 0.95f), fade(t->panel, 0.95f));
		ph_text_draw(g, u->f_huge, x + (cw - iw) * 0.5f,
		    y + (ch - ph_font_height(u->f_huge)) * 0.5f, ic,
		    fade(t->accent, is_sel ? 0.55f : 0.28f));
	}

	/* --- badges over the cover --------------------------------- */
	if (gm->favorite) {
		float sw = ph_text_width(u->f_small, IC_STAR);

		ph_gfx_rect(g, x + cw - sw - 14.0f, y + 6.0f, sw + 10.0f,
		    ph_font_height(u->f_small) + 6.0f, fade(t->shadow, 0.8f));
		ph_text_draw(g, u->f_small, x + cw - sw - 9.0f, y + 9.0f,
		    IC_STAR, t->accent2);
	}
	if (gm->play_seconds > 0) {
		char played[32], badge[48];
		float bw;

		ph_human_time(played, sizeof(played), gm->play_seconds);
		snprintf(badge, sizeof(badge), "%s %s", IC_CLOCK, played);
		bw = ph_text_width(u->f_small, badge);
		ph_gfx_rect(g, x + 6.0f, y + ch - ph_font_height(u->f_small) - 12.0f,
		    bw + 12.0f, ph_font_height(u->f_small) + 6.0f,
		    fade(t->shadow, 0.82f));
		ph_text_draw(g, u->f_small, x + 12.0f,
		    y + ch - ph_font_height(u->f_small) - 9.0f, badge,
		    fade(t->text, 0.9f));
	}

	/* --- frame -------------------------------------------------- */
	border = is_sel ? t->accent
	    : (is_hov ? fade(t->accent, 0.5f) : fade(t->frame, 0.9f));
	ph_gfx_border(g, x, y, cw, ch, is_sel ? 2.0f : 1.0f, border);
	if (is_sel) {
		/* A selection you can find instantly from the far side of a
		 * room: a solid bar under the art plus a tinted wash. */
		ph_gfx_rect(g, x, y, cw, ch, fade(t->accent, 0.10f));
		ph_gfx_rect(g, x, y + ch - 3.0f, cw, 3.0f, t->accent);
	}

	/* --- label --------------------------------------------------- */
	ph_text_draw_clip(g, u->f_body, x, lab_y, cw, gm->title,
	    is_sel ? t->text_bright : t->text);
	{
		char meta[160];

		meta[0] = '\0';
		if (gm->genre[0] != '\0')
			strlcat(meta, gm->genre, sizeof(meta));
		if (gm->year[0] != '\0') {
			if (meta[0] != '\0')
				strlcat(meta, "  \xc2\xb7  ", sizeof(meta));
			strlcat(meta, gm->year, sizeof(meta));
		}
		if (meta[0] == '\0')
			strlcpy(meta, gm->slug, sizeof(meta));
		ph_text_draw_clip(g, u->f_small, x,
		    lab_y + ph_font_height(u->f_body) * 0.98f, cw, meta,
		    fade(t->text_dim, is_sel ? 1.0f : 0.8f));
	}
}

static void
draw_empty(struct ph_ui *u, float x, float y, float w, float h)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	static const char *const lines[] = {
		"NO GAMES INSTALLED",
		"",
		"Add one from the shell:",
		"    punkhazard add /path/to/game",
		"    punkhazard add ./source-tree --title \"My Game\"",
		"    punkhazard add game.tar.gz",
		"",
		"Then press  R  to rescan.",
		NULL
	};
	float cy = y + h * 0.22f;
	int i;

	{
		float iw = ph_text_width(u->f_huge, IC_PAD);

		ph_text_draw(g, u->f_huge, x + (w - iw) * 0.5f, cy, IC_PAD,
		    fade(t->accent, 0.30f));
		cy += ph_font_height(u->f_huge) + 10.0f;
	}
	for (i = 0; lines[i] != NULL; i++) {
		struct ph_font *f = (i == 0) ? u->f_title : u->f_body;
		ph_rgba c = (i == 0) ? t->accent
		    : (lines[i][0] == ' ' ? t->accent2 : t->text_dim);
		float lw = ph_text_width(f, lines[i]);

		if (lines[i][0] != '\0')
			ph_text_draw(g, f, x + (w - lw) * 0.5f, cy, lines[i], c);
		cy += ph_font_height(f) * 1.18f;
	}
}

static void
draw_grid(struct ph_ui *u)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	float pitch = row_pitch(u);
	int first_row, last_row, i;

	ph_gfx_rect(g, u->gx, u->gy, u->gw, u->gh, fade(t->panel, 0.55f));

	if (u->nview == 0) {
		draw_empty(u, u->gx, u->gy, u->gw, u->gh);
		ph_gfx_border(g, u->gx, u->gy, u->gw, u->gh, 1.0f, t->frame);
		return;
	}

	/* Clip to the viewport and draw only the rows that intersect it, so
	 * a library of a thousand games costs the same as one of ten. */
	ph_gfx_clip(g, (int)u->gx, (int)u->gy, (int)u->gw, (int)u->gh);
	first_row = (int)(u->scroll / pitch);
	if (first_row < 0)
		first_row = 0;
	last_row = first_row + u->rows_visible + 2;

	for (i = first_row * u->cols;
	     i < last_row * u->cols && (size_t)i < u->nview; i++) {
		int col = i % u->cols, row = i / u->cols;
		float tx = u->gx + (float)col * (u->tile_w + (float)u->lay.tile_gap);
		float ty = u->gy + (float)row * pitch - u->scroll;

		draw_tile(u, &u->lib->v[u->view[i]], tx, ty,
		    i == u->sel && u->focus == FOCUS_GRID,
		    i == u->hover);
		/* A selection that has lost focus still has to be findable. */
		if (i == u->sel && u->focus != FOCUS_GRID)
			ph_gfx_border(g, tx, ty, u->tile_w,
			    u->tile_w * u->lay.tile_aspect, 2.0f,
			    fade(t->accent, 0.45f));
	}
	ph_gfx_clip_off(g);
	ph_gfx_border(g, u->gx, u->gy, u->gw, u->gh, 1.0f, t->frame);

	/* Scrollbar, only when there is something to scroll. */
	{
		int nrows = ((int)u->nview + u->cols - 1) / u->cols;
		float total = (float)nrows * pitch;

		if (total > u->gh) {
			float th = u->gh * (u->gh / total);
			float ty;

			if (th < 26.0f)
				th = 26.0f;
			ty = u->gy + (u->scroll / (total - u->gh)) * (u->gh - th);
			ph_gfx_rect(g, u->gx + u->gw - 5.0f, u->gy, 3.0f, u->gh,
			    fade(t->frame, 0.6f));
			ph_gfx_rect(g, u->gx + u->gw - 5.0f, ty, 3.0f, th,
			    fade(t->accent, 0.85f));
		}
	}
}

/* -------------------------- right panel --------------------------- */
/*
 * One label/value row.  `limit` is the floor the details block may not
 * cross -- on a short window there is simply not room for every field, and
 * silently dropping the ones that do not fit is far better than drawing
 * them on top of the options list.
 */
static float
meta_row(struct ph_ui *u, float x, float y, float w, float limit,
    const char *k, const char *v)
{
	const struct ph_theme *t = u->theme;
	float kw = w * 0.42f;

	if (v == NULL || *v == '\0')
		return y;
	if (y + ph_font_height(u->f_small) > limit)
		return y;
	if (kw > 130.0f)
		kw = 130.0f;
	ph_text_draw(u->g, u->f_small, x, y, k, fade(t->text_dim, 0.65f));
	ph_text_draw_clip(u->g, u->f_small, x + kw, y, w - kw, v, t->text);
	return y + ph_font_height(u->f_small) * 1.5f;
}

static void
draw_panel(struct ph_ui *u)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	struct ph_game *gm = sel_game(u);
	float pad = (float)u->lay.pad;
	float x = u->px, y = u->py, w = u->pw, h = u->ph_;
	float inner = w - pad * 2.0f;
	float ty, opt_rowh, opt_top, opt_head_y, exec_y;
	int i;

	ph_gfx_rect(g, x, y, w, h, t->panel);
	ph_gfx_border(g, x, y, w, h, 1.0f,
	    u->focus == FOCUS_PANEL ? fade(t->accent, 0.8f) : t->frame);

	/*
	 * The panel is laid out from the bottom up: the options block is
	 * anchored to the floor, the OPTIONS heading and rule sit above it,
	 * and the exec line above that.  Whatever vertical space is left is
	 * what the description may use.  Computing these first is what keeps
	 * the blocks from overlapping when the window is short.
	 */
	opt_rowh   = ph_font_height(u->f_body) * 1.75f;
	opt_top    = y + h - pad - opt_rowh * (float)NOPTIONS;
	opt_head_y = opt_top - ph_font_height(u->f_small) * 1.7f;
	exec_y     = opt_head_y - ph_font_height(u->f_small) * 1.9f;

	/* ---- details ---------------------------------------------- */
	ty = y + pad;
	ph_text_draw(g, u->f_small, x + pad, ty, "DETAILS",
	    fade(t->accent, 0.9f));
	ty += ph_font_height(u->f_small) * 1.2f;
	ph_gfx_rect(g, x + pad, ty, inner, 1.0f, fade(t->accent, 0.35f));
	ty += pad * 0.5f;

	if (gm == NULL) {
		const char *msg = u->lib->n == 0 ? "library is empty"
		                                 : "nothing matches the filter";
		ph_text_draw_clip(g, u->f_small, x + pad, ty, inner, msg,
		    fade(t->text_dim, 0.8f));
	} else {
		char buf[256], tmp[64];

		float lim = exec_y - 8.0f;

		ph_text_draw_clip(g, u->f_body, x + pad, ty, inner, gm->title,
		    t->text_bright);
		ty += ph_font_height(u->f_body) * 1.4f;

		ty = meta_row(u, x + pad, ty, inner, lim, "DEVELOPER", gm->developer);
		ty = meta_row(u, x + pad, ty, inner, lim, "GENRE",     gm->genre);
		ty = meta_row(u, x + pad, ty, inner, lim, "YEAR",      gm->year);
		ph_human_date(tmp, sizeof(tmp), gm->last_played);
		ty = meta_row(u, x + pad, ty, inner, lim, "LAST PLAYED", tmp);
		ph_human_time(tmp, sizeof(tmp), gm->play_seconds);
		snprintf(buf, sizeof(buf), "%s  (%lu)", tmp, gm->play_count);
		ty = meta_row(u, x + pad, ty, inner, lim, "PLAYTIME", buf);
		ph_human_date(tmp, sizeof(tmp), gm->added);
		ty = meta_row(u, x + pad, ty, inner, lim, "ADDED", tmp);

		/* Description, wrapped into whatever room is left above the
		 * options block. */
		if (gm->desc[0] != '\0') {
			struct ph_line lines[12];
			int n;

			ty += pad * 0.3f;
			n = ph_text_wrap(u->f_small, gm->desc, inner, lines,
			    (int)(sizeof(lines) / sizeof(lines[0])));
			for (i = 0; i < n; i++) {
				if (ty + ph_font_height(u->f_small) > exec_y - 6.0f)
					break;
				ph_text_draw_n(g, u->f_small, x + pad, ty,
				    lines[i].p, lines[i].len,
				    fade(t->text, 0.82f));
				ty += ph_font_height(u->f_small) * 1.28f;
			}
		}

		/* What will actually be executed, pinned above the options. */
		{
			char exec[PH_PATH_MAX];
			char line[PH_PATH_MAX + 16];
			float by = exec_y;

			if (by > ty - ph_font_height(u->f_small) &&
			    ph_game_exec_path(gm, exec, sizeof(exec)) == 0) {
				snprintf(line, sizeof(line), "%s %s", IC_TERM,
				    exec);
				ph_gfx_rect(g, x + pad, by - 5.0f, inner,
				    ph_font_height(u->f_small) + 10.0f,
				    fade(t->bg, 0.65f));
				ph_text_draw_clip(g, u->f_small, x + pad + 6.0f,
				    by, inner - 12.0f, line,
				    fade(t->text_dim, 0.9f));
			}
		}
	}

	/* ---- options ----------------------------------------------- */
	ph_text_draw(g, u->f_small, x + pad, opt_head_y, "OPTIONS",
	    fade(t->accent, 0.9f));
	ph_gfx_rect(g, x + pad, opt_head_y + ph_font_height(u->f_small) * 1.25f,
	    inner, 1.0f, fade(t->accent, 0.35f));

	for (i = 0; i < NOPTIONS; i++) {
		float ry = opt_top + (float)i * opt_rowh;
		int active = (u->focus == FOCUS_PANEL && i == u->opt_sel);
		int dimmed = options[i].needs_game && gm == NULL;
		ph_rgba lc, kc;
		float tx;

		if (active)
			ph_gfx_rect(g, x + pad * 0.5f, ry, w - pad, opt_rowh,
			    t->sel_bg);
		if (i == u->hover_opt && !active)
			ph_gfx_rect(g, x + pad * 0.5f, ry, w - pad, opt_rowh,
			    fade(t->accent, 0.07f));
		if (active)
			ph_gfx_rect(g, x + pad * 0.5f, ry, 3.0f, opt_rowh,
			    t->accent);

		lc = dimmed ? fade(t->text_dim, 0.45f)
		            : (active ? t->sel_fg : t->text);
		kc = dimmed ? fade(t->text_dim, 0.35f) : fade(t->text_dim, 0.8f);

		tx = x + pad;
		ph_text_draw(g, u->f_body, tx,
		    ry + (opt_rowh - ph_font_height(u->f_body)) * 0.5f,
		    options[i].icon, dimmed ? kc : t->accent);
		ph_text_draw(g, u->f_body, tx + 30.0f,
		    ry + (opt_rowh - ph_font_height(u->f_body)) * 0.5f,
		    options[i].label, lc);
		{
			float kw = ph_text_width(u->f_small, options[i].key);

			ph_text_draw(g, u->f_small, x + w - pad - kw,
			    ry + (opt_rowh - ph_font_height(u->f_small)) * 0.5f,
			    options[i].key, kc);
		}
	}
}

/* -------------------------- bottom panel -------------------------- */
static void
draw_bottom(struct ph_ui *u, float y, float W, float h)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	const struct ph_sysmon *m = &u->mon;
	float pad = (float)u->lay.pad;
	float x = pad, row1, row2, colw;
	char buf[128];

	ph_gfx_rect(g, 0, y, W, h, t->panel);
	ph_gfx_rect(g, 0, y, W, 1.0f, fade(t->accent, 0.35f));

	row1 = y + 12.0f;
	row2 = row1 + ph_font_height(u->f_small) * 1.35f;

	/* Five columns of gauges, sized to the window so they never collide. */
	colw = (W - pad * 2.0f) / 5.0f;
	if (colw > 300.0f)
		colw = 300.0f;

#define LABEL(ic, txt) \
	snprintf(buf, sizeof(buf), "%s %s", (ic), (txt))

	/* CPU */
	LABEL(IC_CPU, "CPU");
	ph_text_draw(g, u->f_small, x, row1, buf, fade(t->text_dim, 0.9f));
	snprintf(buf, sizeof(buf), "%3.0f%%  %d\xc3\x97", m->cpu_pct, m->ncpu);
	ph_text_draw(g, u->f_small, x + colw - 78.0f, row1, buf,
	    fade(t->text, 0.95f));
	meter(u, x, row2, colw - 26.0f, 9.0f, m->cpu_pct, t->accent);

	/* MEMORY */
	x += colw;
	LABEL(IC_MEM, "MEMORY");
	ph_text_draw(g, u->f_small, x, row1, buf, fade(t->text_dim, 0.9f));
	if (m->mem_total_mb > 0)
		snprintf(buf, sizeof(buf), "%lu/%lu MB",
		    m->mem_used_mb, m->mem_total_mb);
	else
		snprintf(buf, sizeof(buf), "-");
	ph_text_draw(g, u->f_small, x + colw - 118.0f, row1, buf,
	    fade(t->text, 0.95f));
	meter(u, x, row2, colw - 26.0f, 9.0f, m->mem_pct, t->accent);

	/* LOAD */
	x += colw;
	LABEL(IC_LOAD, "LOAD");
	ph_text_draw(g, u->f_small, x, row1, buf, fade(t->text_dim, 0.9f));
	snprintf(buf, sizeof(buf), "%.2f  %.2f  %.2f",
	    m->load[0], m->load[1], m->load[2]);
	ph_text_draw(g, u->f_small, x, row2 - 2.0f, buf, fade(t->text, 0.95f));

	/* UPTIME */
	x += colw;
	LABEL(IC_CLOCK, "UPTIME");
	ph_text_draw(g, u->f_small, x, row1, buf, fade(t->text_dim, 0.9f));
	uptime_str(buf, sizeof(buf), m->uptime_sec);
	ph_text_draw(g, u->f_small, x, row2 - 2.0f, buf, fade(t->text, 0.95f));

	/* HOST */
	x += colw;
	LABEL(IC_HOST, "HOST");
	ph_text_draw(g, u->f_small, x, row1, buf, fade(t->text_dim, 0.9f));
	ph_text_draw_clip(g, u->f_small, x, row2 - 2.0f,
	    W - x - pad, m->os, fade(t->text, 0.95f));
#undef LABEL

	/* A toast replaces the hint line; both live on the far right so they
	 * never overlap a gauge. */
	if (u->toast_t > 0.0f) {
		float a = u->toast_t > 1.0f ? 1.0f : u->toast_t;
		float tw = ph_text_width(u->f_small, u->toast);

		if (tw < W - pad * 2.0f) {
			ph_gfx_rect(g, W - pad - tw - 14.0f, y + h * 0.5f - 13.0f,
			    tw + 20.0f, 26.0f, fade(t->bg, 0.9f));
			ph_text_draw(g, u->f_small, W - pad - tw - 4.0f,
			    y + h * 0.5f - ph_font_height(u->f_small) * 0.5f,
			    u->toast, fade(t->accent2, a));
		}
	}
}

/* ---------------------------- overlays ---------------------------- */
static void
draw_searchbar(struct ph_ui *u, float x, float y, float w, float h,
    float time_sec)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	float tx, ty;

	ph_gfx_rect(g, x, y, w, h, t->panel_alt);
	ph_gfx_border(g, x, y, w, h, 1.0f, t->accent);

	ty = y + (h - ph_font_height(u->f_body)) * 0.5f;
	tx = ph_text_draw(g, u->f_body, x + 12.0f, ty, IC_SEARCH, t->accent);
	tx = ph_text_draw(g, u->f_body, tx + 10.0f, ty, u->query,
	    t->text_bright);

	/* Blinking block cursor, because this is a console launcher. */
	if (fmodf(time_sec, 1.0f) < 0.6f)
		ph_gfx_rect(g, tx + 2.0f, y + h * 0.25f, 9.0f, h * 0.5f,
		    t->accent);
	if (u->query[0] == '\0')
		ph_text_draw(g, u->f_small, tx + 18.0f,
		    y + (h - ph_font_height(u->f_small)) * 0.5f,
		    "type to filter   ESC to clear", fade(t->text_dim, 0.7f));
}

static void
draw_help(struct ph_ui *u, float W, float H)
{
	const struct ph_theme *t = u->theme;
	struct ph_gfx *g = u->g;
	static const char *const rows[] = {
		"GRID",
		"  arrows / h j k l      move around the grid",
		"  PageUp / PageDown     move a screenful",
		"  Home / End            first / last game",
		"  Tab                   move focus: grid <-> options panel",
		"  mouse wheel           scroll    |    double-click    launch",
		"",
		"ACTIONS",
		"  Return / Space        launch the selected game",
		"  /                     filter the library",
		"  B or *                toggle favourite (favourites sort first)",
		"  S                     cycle sort: title, recent, playtime, added, year",
		"  R                     rescan the library from disk",
		"",
		"DISPLAY",
		"  F11 or F              toggle fullscreen",
		"  C                     toggle the CRT post-process",
		"  T                     cycle theme",
		"",
		"GAMEPAD",
		"  d-pad / left stick    move       A  launch       B  back",
		"  X                     focus panel   Y  favourite   Back  sort",
		"",
		"Esc closes this.  Q quits.",
		NULL
	};
	float pw = W * 0.74f, phh = H * 0.84f, px, py, ty;
	int i;

	if (pw > 920.0f)
		pw = 920.0f;
	px = (W - pw) * 0.5f;
	py = (H - phh) * 0.5f;

	ph_gfx_rect(g, 0, 0, W, H, fade(t->shadow, 0.88f));
	ph_gfx_rect(g, px, py, pw, phh, t->panel);
	ph_gfx_border(g, px, py, pw, phh, 2.0f, t->accent);

	ty = py + (float)u->lay.pad;
	ph_text_draw(g, u->f_title, px + (float)u->lay.pad, ty, "KEYS",
	    t->accent);
	ty += ph_font_height(u->f_title) * 1.2f;

	for (i = 0; rows[i] != NULL; i++) {
		int is_head = (rows[i][0] != '\0' && rows[i][0] != ' ');

		if (rows[i][0] != '\0')
			ph_text_draw(g, u->f_small, px + (float)u->lay.pad, ty,
			    rows[i], is_head ? t->accent2 : fade(t->text, 0.9f));
		ty += ph_font_height(u->f_small) * 1.34f;
	}
}

/* ---------------------------- the frame --------------------------- */
void
ph_ui_draw(struct ph_ui *u, float time_sec)
{
	float W = (float)ph_gfx_w(u->g);
	float H = (float)ph_gfx_h(u->g);
	float pad = (float)u->lay.pad;
	float top_h = (float)u->lay.top_h;
	float bot_h = (float)u->lay.bottom_h;
	float work_y, work_h, work_w, sbar = 0.0f;
	int show_panel;

	/* The details/options panel earns its 30% only on a wide enough
	 * window; below that the grid takes everything and the top panel
	 * still carries the selected game's details. */
	show_panel = (W >= 960.0f);

	work_y = top_h + pad;
	work_h = H - top_h - bot_h - pad * 2.0f;
	work_w = W - pad * 2.0f;
	if (work_h < 80.0f)
		work_h = 80.0f;

	u->gx = pad;
	u->gy = work_y;
	u->gh = work_h;
	if (show_panel) {
		u->gw = (work_w - pad) * u->lay.split;
		u->pw = (work_w - pad) - u->gw;
		u->px = u->gx + u->gw + pad;
		u->py = work_y;
		u->ph_ = work_h;
	} else {
		u->gw = work_w;
		u->pw = u->ph_ = 0.0f;
		u->focus = FOCUS_GRID;
	}
	if (u->gw < 120.0f)
		u->gw = 120.0f;

	if (u->mode == MODE_SEARCH) {
		sbar = 46.0f;
		u->gy += sbar;
		u->gh -= sbar;
	}

	/*
	 * Columns are derived from the available width, not fixed, so the
	 * grid reflows on resize instead of clipping.  The tile is then
	 * widened to divide the row exactly, which keeps the right edge
	 * flush with the panel.
	 */
	{
		float gap = (float)u->lay.tile_gap;
		float want = (float)u->lay.tile_w;

		u->cols = (int)((u->gw - pad + gap) / (want + gap));
		if (u->cols < 1)
			u->cols = 1;
		u->tile_w = (u->gw - pad - (float)(u->cols - 1) * gap)
		    / (float)u->cols;
		if (u->tile_w < 48.0f)
			u->tile_w = 48.0f;
		u->tile_h = u->tile_w * u->lay.tile_aspect
		    + ph_font_height(u->f_body) + ph_font_height(u->f_small)
		    + 12.0f;
		u->rows_visible = (int)(u->gh / row_pitch(u));
		if (u->rows_visible < 1)
			u->rows_visible = 1;
	}
	/* Inset the grid content so tiles are not flush against the border. */
	u->gx += pad * 0.5f;
	u->gy += pad * 0.5f;
	u->gh -= pad;

	draw_top(u, W, top_h);
	if (u->mode == MODE_SEARCH)
		draw_searchbar(u, pad, top_h + pad, u->gw, sbar - 8.0f, time_sec);
	draw_grid(u);
	if (show_panel)
		draw_panel(u);
	draw_bottom(u, H - bot_h, W, bot_h);

	if (u->mode == MODE_HELP)
		draw_help(u, W, H);
}
