/*
 * embed.c -- X11 window adoption.  See embed.h for what this can and
 * cannot do; the short version is that X11 can adopt another client's
 * window and Wayland cannot.
 */
#include "embed.h"

#include <stdlib.h>
#include <string.h>

#include <SDL_syswm.h>

#if defined(SDL_VIDEO_DRIVER_X11)
#  define PH_HAVE_X11 1
#  include <X11/Xlib.h>
#  include <X11/Xatom.h>
#endif

struct ph_embed {
	const char *why_not;
#ifdef PH_HAVE_X11
	Display *dpy;
	Window   parent;		/* the launcher's own window */
	Window   child;			/* the adopted game window, 0 if none */
	Atom     a_pid;
	Atom     a_state;
	int      px, py, pw, ph_h;	/* last placement */
#endif
};

#ifdef PH_HAVE_X11

/*
 * The game can exit -- and its window disappear -- between the moment we
 * find it and the moment we touch it.  Every such race surfaces as a
 * BadWindow, and Xlib's default handler calls exit(3), which would take
 * the launcher down with it.  So errors are swallowed and noted.
 */
static volatile int x_err;

static int
quiet_handler(Display *d, XErrorEvent *e)
{
	(void)d;
	(void)e;
	x_err = 1;
	return 0;
}

/* _NET_WM_PID for a window, or 0. */
static pid_t
window_pid(struct ph_embed *e, Window w)
{
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, after = 0;
	unsigned char *data = NULL;
	pid_t pid = 0;

	if (XGetWindowProperty(e->dpy, w, e->a_pid, 0, 1, False, XA_CARDINAL,
	    &type, &fmt, &n, &after, &data) != Success)
		return 0;
	if (data != NULL) {
		if (type == XA_CARDINAL && fmt == 32 && n >= 1)
			pid = (pid_t)*(unsigned long *)(void *)data;
		XFree(data);
	}
	return pid;
}

/*
 * Is this a window worth adopting?
 *
 * WM_STATE would be the obvious test -- only a window manager sets it, and
 * only on a managed top-level.  But requiring it means embedding silently
 * fails wherever no window manager is running (a bare startx, a kiosk, a
 * test harness), which is a real configuration and not one worth breaking.
 *
 * So the test is what is true of a game window regardless of who is
 * managing it: it is mapped and visible, and it is not override-redirect
 * (which marks menus, tooltips and splash surfaces that must never be
 * reparented).  WM_STATE is still consulted, but only to prefer a managed
 * window when a process owns several.
 */
static int
is_adoptable(struct ph_embed *e, Window w)
{
	XWindowAttributes a;

	if (!XGetWindowAttributes(e->dpy, w, &a))
		return 0;
	if (a.override_redirect)
		return 0;
	if (a.map_state != IsViewable)
		return 0;
	/* Xlib names this member `class`; it is spelled c_class only under C++. */
	if (a.class != InputOutput)
		return 0;
	/* Ignore the 1x1 helper windows some toolkits keep around. */
	if (a.width < 16 || a.height < 16)
		return 0;
	return 1;
}

static int
has_wm_state(struct ph_embed *e, Window w)
{
	Atom type = None;
	int fmt = 0;
	unsigned long n = 0, after = 0;
	unsigned char *data = NULL;
	int yes = 0;

	if (XGetWindowProperty(e->dpy, w, e->a_state, 0, 1, False,
	    AnyPropertyType, &type, &fmt, &n, &after, &data) != Success)
		return 0;
	if (data != NULL) {
		yes = (type != None);
		XFree(data);
	}
	return yes;
}

/*
 * Depth-first walk for a window owned by `pid`.  `*fallback` collects an
 * adoptable window without WM_STATE, used only if nothing better turns up.
 */
static Window
scan_tree(struct ph_embed *e, Window w, pid_t pid, int depth, Window *fallback)
{
	Window root = 0, parent = 0, *kids = NULL, found = 0;
	unsigned int n = 0, i;

	if (depth > 4)			/* managed windows are shallow */
		return 0;
	if (w != e->parent && window_pid(e, w) == pid && is_adoptable(e, w)) {
		if (has_wm_state(e, w))
			return w;	/* managed: the one we want */
		if (*fallback == 0)
			*fallback = w;
	}
	if (!XQueryTree(e->dpy, w, &root, &parent, &kids, &n))
		return 0;
	for (i = 0; i < n && found == 0; i++) {
		if (kids[i] == e->parent)
			continue;	/* never adopt ourselves */
		found = scan_tree(e, kids[i], pid, depth + 1, fallback);
	}
	if (kids != NULL)
		XFree(kids);
	return found;
}
#endif /* PH_HAVE_X11 */

struct ph_embed *
ph_embed_create(SDL_Window *win)
{
	struct ph_embed *e = ph_xcalloc(1, sizeof(*e));
	SDL_SysWMinfo info;

	SDL_VERSION(&info.version);
	if (win == NULL || !SDL_GetWindowWMInfo(win, &info)) {
		e->why_not = "no window manager information from SDL";
		return e;
	}

#ifdef PH_HAVE_X11
	if (info.subsystem == SDL_SYSWM_X11) {
		e->dpy = info.info.x11.display;
		e->parent = info.info.x11.window;
		e->a_pid = XInternAtom(e->dpy, "_NET_WM_PID", False);
		e->a_state = XInternAtom(e->dpy, "WM_STATE", False);
		XSetErrorHandler(quiet_handler);
		ph_info("embedding available (X11)");
		return e;
	}
#endif
	if (info.subsystem == SDL_SYSWM_WAYLAND) {
		/* Not a shortcoming of this code: see embed.h. */
		e->why_not = "Wayland does not let a client embed another "
		    "client's surface";
	} else {
		e->why_not = "embedding is implemented for X11 only";
	}
	return e;
}

void
ph_embed_destroy(struct ph_embed *e)
{
	if (e == NULL)
		return;
	ph_embed_release(e);
	free(e);
}

const char *
ph_embed_why_not(const struct ph_embed *e)
{
	return e == NULL ? "no embedder" : e->why_not;
}

int
ph_embed_active(const struct ph_embed *e)
{
#ifdef PH_HAVE_X11
	return e != NULL && e->child != 0;
#else
	(void)e;
	return 0;
#endif
}

int
ph_embed_try_capture(struct ph_embed *e, pid_t pid)
{
#ifdef PH_HAVE_X11
	Window w;

	if (e == NULL || e->dpy == NULL || e->child != 0)
		return 0;

	x_err = 0;
	{
		Window fallback = 0;

		w = scan_tree(e, DefaultRootWindow(e->dpy), pid, 0, &fallback);
		if (w == 0)
			w = fallback;	/* no window manager running */
	}
	if (w == 0)
		return 0;

	/*
	 * Adopt it.  Reparenting generates an UnmapNotify/MapNotify pair on
	 * the child; mapping it again afterwards is what makes it appear in
	 * its new home rather than staying withdrawn.
	 */
	XReparentWindow(e->dpy, w, e->parent, e->px, e->py);
	XResizeWindow(e->dpy, w, (unsigned)(e->pw > 0 ? e->pw : 640),
	    (unsigned)(e->ph_h > 0 ? e->ph_h : 480));
	XMapWindow(e->dpy, w);
	XSync(e->dpy, False);

	if (x_err) {			/* it vanished mid-flight */
		ph_info("embed: window went away during capture");
		return 0;
	}
	e->child = w;
	XSetInputFocus(e->dpy, w, RevertToParent, CurrentTime);
	XSync(e->dpy, False);
	ph_info("embed: adopted window 0x%lx for pid %ld",
	    (unsigned long)w, (long)pid);
	return 1;
#else
	(void)e;
	(void)pid;
	return 0;
#endif
}

void
ph_embed_place(struct ph_embed *e, int x, int y, int w, int h)
{
#ifdef PH_HAVE_X11
	if (e == NULL || e->dpy == NULL)
		return;
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	/* Remember it even before a capture: the placement is what the game
	 * window gets sized to the moment it is adopted. */
	if (x == e->px && y == e->py && w == e->pw && h == e->ph_h)
		return;
	e->px = x; e->py = y; e->pw = w; e->ph_h = h;
	if (e->child == 0)
		return;
	x_err = 0;
	XMoveResizeWindow(e->dpy, e->child, x, y, (unsigned)w, (unsigned)h);
	XSync(e->dpy, False);
#else
	(void)e; (void)x; (void)y; (void)w; (void)h;
#endif
}

void
ph_embed_release(struct ph_embed *e)
{
#ifdef PH_HAVE_X11
	if (e == NULL || e->dpy == NULL || e->child == 0)
		return;
	x_err = 0;
	/* Hand it back to the root so a still-living game keeps its window
	 * instead of being destroyed along with our container. */
	XReparentWindow(e->dpy, e->child, DefaultRootWindow(e->dpy), 0, 0);
	XSync(e->dpy, False);
	e->child = 0;
#else
	(void)e;
#endif
}
