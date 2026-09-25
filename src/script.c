/*
 * script.c -- embedding Lua 5.4.
 *
 * The whole VM lifetime is: open, load five files, pull their tables into C
 * structs, close.  Lua is used as a configuration *language* rather than a
 * runtime: it earns its place because themes and keymaps are data with
 * expressions in them (a theme wants to derive a dim colour from a bright
 * one; a keymap wants a loop), and because it lets a user retheme the
 * launcher without a compiler.
 *
 * Lua's C API is a stack machine.  Every helper here follows the same
 * discipline: push, read, pop, leaving the stack exactly as it was found.
 * lua_getfield pushes one value; every path that calls it also pops it.
 */
#include "ph.h"
#include "script.h"

#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

/* Registry keys.  Using our own addresses as light-userdata keys is the
 * idiomatic way to stash values where user code cannot collide with us. */
static const char K_CONFIG, K_THEME, K_LAYOUT, K_KEYS, K_INSTALL;

struct ph_script {
	lua_State *L;
	char       luadir[PH_PATH_MAX];
};

/* ------------------------------------------------------------------ *
 * Stack helpers.  Each expects the table at stack index `t`.
 * ------------------------------------------------------------------ */
static int
get_int(lua_State *L, int t, const char *k, int dflt)
{
	int v = dflt;

	lua_getfield(L, t, k);
	if (lua_isnumber(L, -1))
		v = (int)lua_tointeger(L, -1);
	else if (lua_isboolean(L, -1))
		v = lua_toboolean(L, -1);
	lua_pop(L, 1);
	return v;
}

static float
get_num(lua_State *L, int t, const char *k, float dflt)
{
	float v = dflt;

	lua_getfield(L, t, k);
	if (lua_isnumber(L, -1))
		v = (float)lua_tonumber(L, -1);
	lua_pop(L, 1);
	return v;
}

/*
 * Colours are written in Lua as 0xRRGGBBAA integers.  Lua 5.4 has real
 * 64-bit integers, so 0xff00ffff survives unmangled -- in 5.1/5.2 it would
 * have arrived as a double and needed rounding.
 */
static ph_rgba
get_rgba(lua_State *L, int t, const char *k, ph_rgba dflt)
{
	ph_rgba v = dflt;

	lua_getfield(L, t, k);
	if (lua_isnumber(L, -1))
		v = (ph_rgba)(lua_Unsigned)lua_tointeger(L, -1);
	lua_pop(L, 1);
	return v;
}

static void
get_str(lua_State *L, int t, const char *k, char *dst, size_t dstsize,
    const char *dflt)
{
	const char *s;

	lua_getfield(L, t, k);
	s = lua_isstring(L, -1) ? lua_tostring(L, -1) : dflt;
	strlcpy(dst, s != NULL ? s : "", dstsize);
	lua_pop(L, 1);
}

/* Push registry[key] onto the stack. Returns 1 if it is a table. */
static int
push_reg(lua_State *L, const char *key)
{
	lua_rawgetp(L, LUA_REGISTRYINDEX, key);
	if (lua_istable(L, -1))
		return 1;
	lua_pop(L, 1);
	return 0;
}

/*
 * Load <dir>/<file>, expect the chunk to return a table, stash it in the
 * registry under `key`.  A missing file is not an error (the caller falls
 * back to compiled-in defaults); a syntax error is reported but also
 * survivable, for the same reason.
 */
static int
load_table(struct ph_script *s, const char *file, const void *key, int required)
{
	char path[PH_PATH_MAX];
	lua_State *L = s->L;

	if (ph_join(path, sizeof(path), s->luadir, file) != 0)
		return -1;
	if (!ph_is_file(path)) {
		if (required)
			ph_warn("missing Lua file: %s", path);
		return -1;
	}
	if (luaL_loadfile(L, path) != LUA_OK) {
		ph_warn("%s: %s", file, lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}
	if (lua_pcall(L, 0, 1, 0) != LUA_OK) {
		ph_warn("%s: %s", file, lua_tostring(L, -1));
		lua_pop(L, 1);
		return -1;
	}
	if (!lua_istable(L, -1)) {
		ph_warn("%s: chunk did not return a table", file);
		lua_pop(L, 1);
		return -1;
	}
	lua_rawsetp(L, LUA_REGISTRYINDEX, key);
	ph_info("loaded %s", path);
	return 0;
}

struct ph_script *
ph_script_open(const struct ph_paths *p)
{
	struct ph_script *s = ph_xcalloc(1, sizeof(*s));

	strlcpy(s->luadir, p->lua, sizeof(s->luadir));
	if ((s->L = luaL_newstate()) == NULL) {
		free(s);
		return NULL;
	}
	luaL_openlibs(s->L);

	/*
	 * So a user's config.lua can `require` helpers from the policy
	 * directory and from their own library root.
	 *
	 * These are *prepended* to the existing package.path rather than
	 * replacing it: overwriting it outright made every module Lua ships
	 * with undiscoverable, so a `require` of anything standard failed.
	 */
	lua_getglobal(s->L, "package");
	if (lua_istable(s->L, -1)) {
		char pat[PH_PATH_MAX * 2 + 64];
		const char *orig;

		lua_getfield(s->L, -1, "path");
		orig = lua_isstring(s->L, -1) ? lua_tostring(s->L, -1) : "";
		snprintf(pat, sizeof(pat), "%s/?.lua;%s/?.lua;%s",
		    s->luadir, p->root, orig);
		lua_pop(s->L, 1);

		lua_pushstring(s->L, pat);
		lua_setfield(s->L, -2, "path");
	}
	lua_pop(s->L, 1);

	load_table(s, "config.lua",  &K_CONFIG,  1);
	load_table(s, "theme.lua",   &K_THEME,   1);
	load_table(s, "layout.lua",  &K_LAYOUT,  1);
	load_table(s, "keys.lua",    &K_KEYS,    1);
	load_table(s, "install.lua", &K_INSTALL, 1);

	/*
	 * User overrides.  <root>/config.lua is loaded last and its keys are
	 * merged over the shipped config table, so a user only has to state
	 * what they want changed.
	 */
	if (ph_is_file(p->config)) {
		if (luaL_loadfile(s->L, p->config) == LUA_OK &&
		    lua_pcall(s->L, 0, 1, 0) == LUA_OK && lua_istable(s->L, -1)) {
			if (push_reg(s->L, (const char *)&K_CONFIG)) {
				/* for k,v in pairs(user) do shipped[k] = v end */
				lua_pushnil(s->L);
				while (lua_next(s->L, -3) != 0) {
					lua_pushvalue(s->L, -2);   /* key   */
					lua_pushvalue(s->L, -2);   /* value */
					lua_settable(s->L, -5);    /* shipped */
					lua_pop(s->L, 1);          /* value */
				}
				lua_pop(s->L, 1);              /* shipped */
			}
			ph_info("merged user config %s", p->config);
		} else {
			ph_warn("config.lua: %s", lua_tostring(s->L, -1));
		}
		lua_pop(s->L, 1);
	}
	return s;
}

void
ph_script_close(struct ph_script *s)
{
	if (s == NULL)
		return;
	if (s->L != NULL)
		lua_close(s->L);
	free(s);
}

/* ------------------------------------------------------------------ *
 * config / layout / theme
 * ------------------------------------------------------------------ */
int
ph_script_config(struct ph_script *s, struct ph_config *c)
{
	lua_State *L = s->L;
	int t;

	if (!push_reg(L, (const char *)&K_CONFIG))
		return -1;
	t = lua_gettop(L);
	c->win_w      = get_int(L, t, "width",      1280);
	c->win_h      = get_int(L, t, "height",      720);
	c->fullscreen = get_int(L, t, "fullscreen",    0);
	c->vsync      = get_int(L, t, "vsync",         1);
	c->crt        = get_int(L, t, "crt",           1);
	c->show_fps   = get_int(L, t, "show_fps",      0);
	get_str(L, t, "theme", c->theme, sizeof(c->theme), "hazard");
	lua_pop(L, 1);
	return 0;
}

int
ph_script_layout(struct ph_script *s, struct ph_layout *l)
{
	lua_State *L = s->L;
	int t;

	if (!push_reg(L, (const char *)&K_LAYOUT))
		return -1;
	t = lua_gettop(L);
	l->pad           = get_int(L, t, "pad",             20);
	l->top_h         = get_int(L, t, "top_height",     116);
	l->bottom_h      = get_int(L, t, "bottom_height",   96);
	l->split         = get_num(L, t, "split",        0.70f);
	l->tile_w        = get_int(L, t, "tile_width",     196);
	l->tile_gap      = get_int(L, t, "tile_gap",        18);
	l->tile_aspect   = get_num(L, t, "tile_aspect",  1.33f);
	l->font_px       = get_int(L, t, "font_size",       20);
	l->font_px_title = get_int(L, t, "font_size_title", 34);
	l->font_px_small = get_int(L, t, "font_size_small", 15);
	l->anim_speed    = get_num(L, t, "anim_speed",   15.0f);
	if (l->split < 0.35f) l->split = 0.35f;
	if (l->split > 0.85f) l->split = 0.85f;
	lua_pop(L, 1);
	return 0;
}

int
ph_script_theme(struct ph_script *s, const char *name, struct ph_theme *t)
{
	lua_State *L = s->L;
	int ti;

	if (!push_reg(L, (const char *)&K_THEME))
		return -1;
	lua_getfield(L, -1, name);
	if (!lua_istable(L, -1)) {		/* unknown name: take the first */
		lua_pop(L, 1);
		lua_pushnil(L);
		if (lua_next(L, -2) == 0) {
			lua_pop(L, 1);
			return -1;
		}
		lua_remove(L, -2);		/* drop the key, keep the value */
	}
	ti = lua_gettop(L);

	strlcpy(t->name, name, sizeof(t->name));
	get_str(L, ti, "name", t->name, sizeof(t->name), name);
	t->bg          = get_rgba(L, ti, "bg",          0x0a0c0fffu);
	t->panel       = get_rgba(L, ti, "panel",       0x12161cffu);
	t->panel_alt   = get_rgba(L, ti, "panel_alt",   0x171c24ffu);
	t->frame       = get_rgba(L, ti, "frame",       0x2a3340ffu);
	t->text        = get_rgba(L, ti, "text",        0xc8d4e0ffu);
	t->text_dim    = get_rgba(L, ti, "text_dim",    0x6b7684ffu);
	t->text_bright = get_rgba(L, ti, "text_bright", 0xf2f6faffu);
	t->accent      = get_rgba(L, ti, "accent",      0x1affaaffu);
	t->accent2     = get_rgba(L, ti, "accent2",     0xff2e88ffu);
	t->danger      = get_rgba(L, ti, "danger",      0xff4444ffu);
	t->ok          = get_rgba(L, ti, "ok",          0x44ff88ffu);
	t->sel_bg      = get_rgba(L, ti, "sel_bg",      0x1affaa28u);
	t->sel_fg      = get_rgba(L, ti, "sel_fg",      0xffffffffu);
	t->shadow      = get_rgba(L, ti, "shadow",      0x00000099u);
	t->scanline    = get_num (L, ti, "scanline",   0.30f);
	t->curvature   = get_num (L, ti, "curvature",  0.03f);
	t->vignette    = get_num (L, ti, "vignette",   0.35f);
	t->aberration  = get_num (L, ti, "aberration", 0.60f);
	t->glow        = get_num (L, ti, "glow",       0.25f);
	t->noise       = get_num (L, ti, "noise",      0.04f);

	lua_pop(L, 2);				/* theme table, themes table */
	return 0;
}

int
ph_script_theme_count(struct ph_script *s)
{
	lua_State *L = s->L;
	int n = 0;

	if (!push_reg(L, (const char *)&K_THEME))
		return 0;
	/* Count only string keys, so this agrees with the list
	 * ph_script_theme_name() builds; anything else is not a theme. */
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		if (lua_type(L, -2) == LUA_TSTRING)
			n++;
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return n;
}

/*
 * Name of the idx'th theme.  Lua table iteration order is unspecified, so
 * the names are collected and sorted to give the UI a stable cycle order.
 */
int
ph_script_theme_name(struct ph_script *s, int idx, char *dst, size_t dstsize)
{
	lua_State *L = s->L;
	char names[32][64];
	int n = 0, i, j;

	if (!push_reg(L, (const char *)&K_THEME))
		return -1;
	/*
	 * The capacity test belongs inside the body, not in the while
	 * condition: lua_next() has already pushed a key and a value by the
	 * time the condition is evaluated, so bailing out there left two
	 * items on the stack and unbalanced it.  Here the traversal always
	 * runs to completion and simply stops storing once full.
	 */
	lua_pushnil(L);
	while (lua_next(L, -2) != 0) {
		if (lua_type(L, -2) == LUA_TSTRING &&
		    n < (int)(sizeof(names) / sizeof(names[0])))
			strlcpy(names[n++], lua_tostring(L, -2), sizeof(names[0]));
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	if (n == 0)
		return -1;

	for (i = 1; i < n; i++) {		/* insertion sort: n is tiny */
		char tmp[64];
		strlcpy(tmp, names[i], sizeof(tmp));
		for (j = i; j > 0 && strcmp(names[j - 1], tmp) > 0; j--)
			strlcpy(names[j], names[j - 1], sizeof(names[0]));
		strlcpy(names[j], tmp, sizeof(names[0]));
	}
	idx = ((idx % n) + n) % n;		/* wrap, including negatives */
	strlcpy(dst, names[idx], dstsize);
	return 0;
}

/* ------------------------------------------------------------------ *
 * Key and pad bindings
 * ------------------------------------------------------------------ */
static const struct {
	const char    *name;
	enum ph_action act;
} action_names[] = {
	{ "up",         PH_ACT_UP        },
	{ "down",       PH_ACT_DOWN      },
	{ "left",       PH_ACT_LEFT      },
	{ "right",      PH_ACT_RIGHT     },
	{ "page_up",    PH_ACT_PAGE_UP   },
	{ "page_down",  PH_ACT_PAGE_DOWN },
	{ "home",       PH_ACT_HOME      },
	{ "end",        PH_ACT_END       },
	{ "launch",     PH_ACT_LAUNCH    },
	{ "back",       PH_ACT_BACK      },
	{ "quit",       PH_ACT_QUIT      },
	{ "fullscreen", PH_ACT_FULLSCREEN},
	{ "search",     PH_ACT_SEARCH    },
	{ "details",    PH_ACT_DETAILS   },
	{ "favorite",   PH_ACT_FAVORITE  },
	{ "sort",       PH_ACT_SORT      },
	{ "crt",        PH_ACT_CRT       },
	{ "theme",      PH_ACT_THEME     },
	{ "reload",     PH_ACT_RELOAD    },
	{ "help",       PH_ACT_HELP      },
	{ NULL,         PH_ACT_NONE      }
};

static enum ph_action
action_by_name(const char *n)
{
	int i;

	if (n == NULL)
		return PH_ACT_NONE;
	for (i = 0; action_names[i].name != NULL; i++)
		if (strcmp(action_names[i].name, n) == 0)
			return action_names[i].act;
	return PH_ACT_NONE;
}

/* keys.lua = { keyboard = {...}, pad = {...} } */
static enum ph_action
lookup_binding(struct ph_script *s, const char *section, const char *key)
{
	lua_State *L = s->L;
	enum ph_action a = PH_ACT_NONE;

	if (key == NULL || *key == '\0')
		return PH_ACT_NONE;
	if (!push_reg(L, (const char *)&K_KEYS))
		return PH_ACT_NONE;
	lua_getfield(L, -1, section);
	if (lua_istable(L, -1)) {
		lua_getfield(L, -1, key);
		if (lua_isstring(L, -1))
			a = action_by_name(lua_tostring(L, -1));
		lua_pop(L, 1);
	}
	lua_pop(L, 2);
	return a;
}

enum ph_action
ph_script_key(struct ph_script *s, const char *keyname)
{
	return lookup_binding(s, "keyboard", keyname);
}

enum ph_action
ph_script_pad(struct ph_script *s, const char *button)
{
	return lookup_binding(s, "pad", button);
}

/* ------------------------------------------------------------------ *
 * Build recipes
 *
 * install.lua returns an array:
 *   { name = "bmake",
 *     detect = { "Makefile", "BSDmakefile" },
 *     steps  = { { "make" } } }
 * ------------------------------------------------------------------ */
static int
read_steps(lua_State *L, int t, struct ph_recipe *r)
{
	int i;

	r->nsteps = 0;
	lua_getfield(L, t, "steps");
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		return 0;
	}
	for (i = 1; i <= PH_RECIPE_MAX_STEPS; i++) {
		int j;

		lua_rawgeti(L, -1, i);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			break;
		}
		{
			struct ph_step *st = &r->steps[r->nsteps];

			st->argc = 0;
			for (j = 1; j <= PH_STEP_MAX_ARGS; j++) {
				lua_rawgeti(L, -1, j);
				if (!lua_isstring(L, -1)) {
					lua_pop(L, 1);
					break;
				}
				strlcpy(st->arg[st->argc++],
				    lua_tostring(L, -1), PH_ARGLEN);
				lua_pop(L, 1);
			}
			if (st->argc > 0)
				r->nsteps++;
		}
		lua_pop(L, 1);
	}
	lua_pop(L, 1);
	return r->nsteps;
}

int
ph_script_recipe(struct ph_script *s, const char *dir, struct ph_recipe *r)
{
	lua_State *L = s->L;
	int i, found = 0;

	memset(r, 0, sizeof(*r));
	if (!push_reg(L, (const char *)&K_INSTALL))
		return 0;

	for (i = 1; !found; i++) {
		int rt, j;

		lua_rawgeti(L, -1, i);
		if (!lua_istable(L, -1)) {
			lua_pop(L, 1);
			break;
		}
		rt = lua_gettop(L);
		get_str(L, rt, "name", r->name, sizeof(r->name), "recipe");

		lua_getfield(L, rt, "detect");
		if (lua_istable(L, -1)) {
			for (j = 1; !found; j++) {
				char probe[PH_PATH_MAX];

				lua_rawgeti(L, -1, j);
				if (!lua_isstring(L, -1)) {
					lua_pop(L, 1);
					break;
				}
				if (ph_join(probe, sizeof(probe), dir,
				    lua_tostring(L, -1)) == 0 &&
				    ph_is_file(probe))
					found = 1;
				lua_pop(L, 1);
			}
		}
		lua_pop(L, 1);			/* detect */

		if (found && read_steps(L, rt, r) == 0)
			found = 0;		/* recipe with no steps: skip */
		lua_pop(L, 1);			/* recipe */
	}
	lua_pop(L, 1);				/* install table */
	return found;
}
