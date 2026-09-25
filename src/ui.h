/*
 * ui.h -- the launcher's front end.
 *
 * The UI is immediate-mode: there is no widget tree and no retained
 * layout.  Every frame recomputes its rectangles from the window size and
 * the layout metrics and draws them.  For a screen with one list and one
 * details panel this is less code and less state than any retained scheme,
 * and it makes resizing and DPI changes free -- there is nothing cached to
 * invalidate.
 *
 * The UI never launches a game or resizes a window itself.  It records a
 * request, and app.c polls for it.  That keeps process control and window
 * management out of the drawing code.
 */
#ifndef PH_UI_H
#define PH_UI_H

#include "ph.h"
#include "gfx.h"
#include "text.h"
#include "script.h"

enum ph_req {
	PH_REQ_NONE = 0,
	PH_REQ_LAUNCH,		/* *out is the game to run */
	PH_REQ_QUIT,
	PH_REQ_FULLSCREEN,	/* toggle */
	PH_REQ_RELOAD		/* rescan the library from disk */
};

struct ph_ui;

/*
 * `cfg`, `theme` and `lay` stay owned by the caller and are read every
 * frame; the UI mutates cfg->crt and cfg->theme in place when the user
 * cycles them, and refills *theme from Lua at the same time.
 */
struct ph_ui *ph_ui_create(struct ph_gfx *g, struct ph_script *sc,
    struct ph_lib *lib, struct ph_config *cfg, struct ph_theme *theme,
    const struct ph_layout *lay);
void ph_ui_destroy(struct ph_ui *u);

/* The library changed underneath us (rescan, install, delete). */
void ph_ui_refresh(struct ph_ui *u);

/*
 * Free every loaded cover texture.  MUST be called before the library array
 * is freed or re-scanned: the GL texture name lives in the ph_game record,
 * so discarding the record without this leaks the texture.
 */
void ph_ui_release_covers(struct ph_ui *u);

void ph_ui_action(struct ph_ui *u, enum ph_action a);
void ph_ui_text(struct ph_ui *u, const char *utf8);   /* search typing */
void ph_ui_backspace(struct ph_ui *u);
/* True while the search field has keyboard focus, so app.c knows to
 * route printable keys to typing rather than to bindings. */
int  ph_ui_is_searching(const struct ph_ui *u);
void ph_ui_mouse_move(struct ph_ui *u, int x, int y);
void ph_ui_mouse_click(struct ph_ui *u, int x, int y, int clicks);
void ph_ui_mouse_wheel(struct ph_ui *u, int dy);

void ph_ui_update(struct ph_ui *u, float dt);
void ph_ui_draw(struct ph_ui *u, float time_sec);

/* Non-blocking: returns the pending request and clears it. */
enum ph_req ph_ui_poll(struct ph_ui *u, struct ph_game **out);
/* A transient line in the footer ("installed X", "build failed"). */
void ph_ui_toast(struct ph_ui *u, const char *fmt, ...);

#endif /* PH_UI_H */
