/*
 * embed.h -- run a game inside the launcher's own window.
 *
 * WHAT THIS CAN AND CANNOT DO
 * ---------------------------
 * On X11 this works: X has always allowed a client to adopt another
 * client's window with XReparentWindow(3), which is the same mechanism
 * behind every window manager's title bars and behind XEmbed.  The game's
 * top-level window is located by its process id, reparented into the
 * launcher's window, and sized to the grid area.  The X server then
 * composites it above our GL surface, so the launcher's own panels stay
 * visible around it.
 *
 * On Wayland this is IMPOSSIBLE for an ordinary client, and no amount of
 * effort here changes that.  Wayland object ids are scoped to a single
 * client connection: a client cannot name -- let alone adopt -- a surface
 * belonging to another process.  wl_subsurface only composes surfaces the
 * *same* client created.  Embedding another application is a compositor's
 * job, and a launcher is not a compositor.  So under Wayland
 * ph_embed_create() returns NULL and app.c falls back to hiding the
 * launcher while the game runs, which is the honest behaviour rather than
 * a broken imitation of the X11 one.
 *
 * It is best-effort even on X11.  A game that maps no top-level window,
 * that reports no _NET_WM_PID, that forks a wrapper the launcher cannot
 * follow, or that sets its own fullscreen will simply not be captured;
 * the launcher then behaves exactly as it does on Wayland.
 */
#ifndef PH_EMBED_H
#define PH_EMBED_H

#include "ph.h"

#include <SDL.h>

struct ph_embed;

/* NULL when the session is not X11, or X11 support was not compiled in. */
struct ph_embed *ph_embed_create(SDL_Window *win);
void ph_embed_destroy(struct ph_embed *e);

/*
 * Look once for a top-level window owned by `pid`.  Call it each frame while
 * a game is starting; returns 1 the first time it captures one.
 */
int  ph_embed_try_capture(struct ph_embed *e, pid_t pid);
int  ph_embed_active(const struct ph_embed *e);

/* Position the embedded window, in launcher-window coordinates. */
void ph_embed_place(struct ph_embed *e, int x, int y, int w, int h);

/* Hand the window back to the root, e.g. when the game has exited. */
void ph_embed_release(struct ph_embed *e);

/* Human-readable reason embedding is unavailable, or NULL if it is. */
const char *ph_embed_why_not(const struct ph_embed *e);

#endif /* PH_EMBED_H */
