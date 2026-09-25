/*
 * embedtest -- prove that ph_embed actually adopts another process's
 * window.  A test harness, never installed.
 *
 * Reparenting is the riskiest thing punkhazard does: it reaches into
 * another client's window through the X server.  Asserting the result
 * with XQueryTree is the only way to know it worked rather than silently
 * doing nothing.
 *
 * Run under any X server, including Xvfb:
 *     Xvfb :99 -screen 0 1280x800x24 &
 *     DISPLAY=:99 ./tools/embedtest /path/to/some-x-program
 */
#include "ph.h"
#include "embed.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include <SDL_syswm.h>
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#include <signal.h>

static Window
parent_of(Display *d, Window w)
{
	Window root = 0, parent = 0, *kids = NULL;
	unsigned int n = 0;

	if (!XQueryTree(d, w, &root, &parent, &kids, &n))
		return 0;
	if (kids != NULL)
		XFree(kids);
	return parent;
}

/* Find the test child's window again, wherever it now lives. */
static Window
find_pid_window(Display *d, Window w, pid_t pid, Atom a_pid, int depth)
{
	Window root = 0, parent = 0, *kids = NULL, found = 0;
	unsigned int n = 0, i;
	Atom type = None;
	int fmt = 0;
	unsigned long items = 0, after = 0;
	unsigned char *data = NULL;

	if (depth > 5)
		return 0;
	if (XGetWindowProperty(d, w, a_pid, 0, 1, False, XA_CARDINAL, &type,
	    &fmt, &items, &after, &data) == Success && data != NULL) {
		pid_t got = (fmt == 32 && items >= 1)
		    ? (pid_t)*(unsigned long *)(void *)data : 0;
		XFree(data);
		if (got == pid) {
			XWindowAttributes a;

			if (XGetWindowAttributes(d, w, &a) &&
			    a.map_state == IsViewable && a.width >= 16)
				return w;
		}
	}
	if (!XQueryTree(d, w, &root, &parent, &kids, &n))
		return 0;
	for (i = 0; i < n && found == 0; i++)
		found = find_pid_window(d, kids[i], pid, a_pid, depth + 1);
	if (kids != NULL)
		XFree(kids);
	return found;
}

int
main(int argc, char **argv)
{
	SDL_Window *win;
	SDL_SysWMinfo info;
	struct ph_embed *e;
	Display *d;
	Window self, gw = 0;
	Atom a_pid;
	pid_t pid;
	int i, rc = 1, captured = 0;
	char *game_argv[2];

	if (argc < 2) {
		fprintf(stderr, "usage: embedtest <program-that-opens-a-window>\n");
		return 2;
	}
	ph_verbose_set(1);

	if (SDL_Init(SDL_INIT_VIDEO) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	win = SDL_CreateWindow("embedtest-parent", 0, 0, 900, 700,
	    SDL_WINDOW_SHOWN);
	if (win == NULL) {
		fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
		return 1;
	}
	SDL_VERSION(&info.version);
	if (!SDL_GetWindowWMInfo(win, &info) || info.subsystem != SDL_SYSWM_X11) {
		fprintf(stderr, "not an X11 session; nothing to test\n");
		return 77;			/* skip */
	}
	d = info.info.x11.display;
	self = info.info.x11.window;
	a_pid = XInternAtom(d, "_NET_WM_PID", False);
	printf("launcher window = 0x%lx\n", (unsigned long)self);

	e = ph_embed_create(win);
	if (ph_embed_why_not(e) != NULL) {
		fprintf(stderr, "FAIL: embedding unavailable: %s\n",
		    ph_embed_why_not(e));
		return 1;
	}
	ph_embed_place(e, 40, 60, 500, 380);

	game_argv[0] = argv[1];
	game_argv[1] = NULL;
	if ((pid = fork()) == 0) {
		execvp(game_argv[0], game_argv);
		_exit(127);
	}
	printf("child pid = %ld\n", (long)pid);

	for (i = 0; i < 100 && !captured; i++) {
		SDL_PumpEvents();
		captured = ph_embed_try_capture(e, pid);
		if (!captured)
			SDL_Delay(50);
	}
	if (!captured) {
		fprintf(stderr, "FAIL: never captured a window for pid %ld\n",
		    (long)pid);
		goto out;
	}
	printf("PASS: captured after %d polls\n", i);

	/* The assertion that matters: the child's window is now OUR child. */
	gw = find_pid_window(d, DefaultRootWindow(d), pid, a_pid, 0);
	if (gw == 0) {
		fprintf(stderr, "FAIL: lost the child window after capture\n");
		goto out;
	}
	printf("game window  = 0x%lx, parent = 0x%lx\n",
	    (unsigned long)gw, (unsigned long)parent_of(d, gw));
	if (parent_of(d, gw) != self) {
		fprintf(stderr, "FAIL: parent is not the launcher window\n");
		goto out;
	}
	printf("PASS: reparented into the launcher window\n");

	/* And that placement really moves and sizes it. */
	ph_embed_place(e, 100, 120, 320, 240);
	XSync(d, False);
	{
		XWindowAttributes a;

		if (XGetWindowAttributes(d, gw, &a) &&
		    a.x == 100 && a.y == 120 && a.width == 320 && a.height == 240)
			printf("PASS: placed at %d,%d %dx%d\n",
			    a.x, a.y, a.width, a.height);
		else {
			fprintf(stderr, "FAIL: placement not applied\n");
			goto out;
		}
	}

	ph_embed_release(e);
	XSync(d, False);
	if (parent_of(d, gw) != DefaultRootWindow(d)) {
		fprintf(stderr, "FAIL: release did not return it to the root\n");
		goto out;
	}
	printf("PASS: released back to the root window\n");
	rc = 0;
out:
	kill(pid, SIGTERM);
	waitpid(pid, NULL, 0);
	ph_embed_destroy(e);
	SDL_DestroyWindow(win);
	SDL_Quit();
	return rc;
}
