/*
 * gfx.h -- the 2D renderer.
 *
 * Deliberately free of any SDL reference.  This layer speaks OpenGL ES 2.0
 * and nothing else, which means it can be driven by any context provider:
 * the SDL window in app.c, or the headless EGL pbuffer in tools/phshot.c
 * that renders the UI to a PNG without a display server.  A renderer you
 * can run without a screen is a renderer you can actually test.
 *
 * WHY OpenGL ES 2.0 AND NOT DESKTOP GL
 * ------------------------------------
 * GLX is an X11 protocol extension and does not exist on Wayland; EGL is
 * the one context/surface API that spans both.  Asking SDL for an ES
 * profile is what pushes it onto EGL on X11 as well as Wayland, so both
 * display servers end up on exactly the same code path rather than one
 * tested path and one hoped-for path.  ES 2.0 is also the widest possible
 * driver baseline, and a flat 2D launcher needs nothing beyond it.
 *
 * EVERYTHING IS A TEXTURED QUAD
 * -----------------------------
 * Solid rectangles are drawn with a 1x1 opaque white texture rather than
 * with a second shader.  One pipeline, one shader, no branch in the
 * fragment path, and the batcher only ever breaks on a genuine texture
 * change.
 */
#ifndef PH_GFX_H
#define PH_GFX_H

#include "ph.h"

struct ph_gfx;

struct ph_gfx *ph_gfx_create(int w, int h);
void  ph_gfx_destroy(struct ph_gfx *g);
void  ph_gfx_resize(struct ph_gfx *g, int w, int h);
int   ph_gfx_w(const struct ph_gfx *g);
int   ph_gfx_h(const struct ph_gfx *g);

/* A frame is: begin -> draw calls -> end.  Between them everything lands
 * in an offscreen framebuffer; ph_gfx_frame_end resolves it to the screen,
 * through the CRT shader when `crt` is set. */
void  ph_gfx_frame_begin(struct ph_gfx *g, ph_rgba clear);
void  ph_gfx_frame_end(struct ph_gfx *g, const struct ph_theme *t,
          float time_sec, int crt);

void  ph_gfx_rect(struct ph_gfx *g, float x, float y, float w, float h, ph_rgba c);
void  ph_gfx_rect_vgrad(struct ph_gfx *g, float x, float y, float w, float h,
          ph_rgba top, ph_rgba bottom);
void  ph_gfx_border(struct ph_gfx *g, float x, float y, float w, float h,
          float thickness, ph_rgba c);
void  ph_gfx_tex(struct ph_gfx *g, unsigned tex, float x, float y, float w, float h,
          float u0, float v0, float u1, float v1, ph_rgba tint);

/* Pixel-space clipping, top-left origin (GL's scissor is bottom-left; the
 * flip happens inside). */
void  ph_gfx_clip(struct ph_gfx *g, int x, int y, int w, int h);
void  ph_gfx_clip_off(struct ph_gfx *g);

/* Textures */
unsigned ph_gfx_tex_rgba(const void *pixels, int w, int h);
unsigned ph_gfx_tex_blank(int w, int h);
void     ph_gfx_tex_sub(unsigned tex, int x, int y, int w, int h, const void *px);
void     ph_gfx_tex_free(unsigned tex);
unsigned ph_gfx_tex_file(const char *path, int *w, int *h);

/* For the offscreen screenshot tool: RGBA, top-left origin, caller frees. */
unsigned char *ph_gfx_capture(struct ph_gfx *g, int *w, int *h);

/* stb.c -- image decoding, kept out of this file so the vendored headers
 * compile in their own translation unit with our warnings switched off. */
unsigned char *ph_image_load(const char *path, int *w, int *h);
void           ph_image_free(unsigned char *px);
int            ph_image_write_png(const char *path, int w, int h, const void *px);

#endif /* PH_GFX_H */
