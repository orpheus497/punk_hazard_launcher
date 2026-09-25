/*
 * script.h -- the Lua policy layer.
 *
 * DIVISION OF LABOUR
 * ------------------
 * C owns mechanism: pixels, processes, files, the event loop.  Lua owns
 * policy: which colours, which metrics, which key does what, which command
 * builds a source tree.  Nothing in Lua can crash the renderer and nothing
 * in C needs recompiling to restyle the launcher.
 *
 * Every Lua file is a plain chunk that returns a table.  There is no
 * framework, no callback registry and no sandbox ceremony -- the files are
 * read once at startup, converted into the C structs below, and the tables
 * are then irrelevant to the hot path.  A frame never enters the Lua VM.
 */
#ifndef PH_SCRIPT_H
#define PH_SCRIPT_H

#include "ph.h"

#define PH_STEP_MAX_ARGS   12
#define PH_RECIPE_MAX_STEPS 4
#define PH_ARGLEN         160

struct ph_step {
	int  argc;
	char arg[PH_STEP_MAX_ARGS][PH_ARGLEN];
};

struct ph_recipe {
	char           name[64];
	int            nsteps;
	struct ph_step steps[PH_RECIPE_MAX_STEPS];
};

struct ph_script;	/* opaque; see script.c */

struct ph_script *ph_script_open(const struct ph_paths *p);
void  ph_script_close(struct ph_script *s);

int   ph_script_config(struct ph_script *s, struct ph_config *c);
int   ph_script_layout(struct ph_script *s, struct ph_layout *l);
int   ph_script_theme(struct ph_script *s, const char *name, struct ph_theme *t);
int   ph_script_theme_count(struct ph_script *s);
int   ph_script_theme_name(struct ph_script *s, int idx, char *dst, size_t dstsize);

/* Key name as SDL spells it (SDL_GetKeyName) -> semantic action. */
enum ph_action ph_script_key(struct ph_script *s, const char *keyname);
/* SDL game controller button name -> semantic action. */
enum ph_action ph_script_pad(struct ph_script *s, const char *button);

/*
 * Choose a build recipe for a source directory: the first recipe in
 * install.lua whose `detect` list names a file that exists there.
 * Returns 1 and fills *r on a match, 0 if the tree needs no building.
 */
int   ph_script_recipe(struct ph_script *s, const char *dir, struct ph_recipe *r);

#endif /* PH_SCRIPT_H */
