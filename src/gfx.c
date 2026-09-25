/*
 * gfx.c -- OpenGL ES 2.0 sprite batcher with a CRT post-process.
 *
 * PIPELINE
 *   frame_begin  bind offscreen FBO, clear
 *   rect/tex/... append quads to a CPU vertex buffer; flush on a texture
 *                change, a clip change, or when the buffer fills
 *   frame_end    flush, unbind FBO, draw one fullscreen triangle sampling
 *                the FBO through either a pass-through or the CRT shader
 *
 * The offscreen pass is not optional even with the CRT off: rendering to a
 * texture and resolving in one pass keeps the UI code free of any
 * knowledge about whether post-processing is on.
 */
#include "gfx.h"

#include <stdlib.h>
#include <string.h>
#include <GLES2/gl2.h>

#define MAX_QUADS 8192
#define VERTS_PER_QUAD 6

struct vtx {
	float         x, y;
	float         u, v;
	unsigned char r, g, b, a;	/* 20 bytes; matches the attrib layout */
};

struct ph_gfx {
	int w, h;

	GLuint prog_ui, prog_blit, prog_crt;
	GLint  ui_res, ui_tex;
	GLint  blit_tex;
	GLint  crt_tex, crt_res, crt_time;
	GLint  crt_curve, crt_scan, crt_vig, crt_ab, crt_glow, crt_noise;

	GLuint vbo;
	GLuint white;			/* 1x1 opaque white */
	GLuint fbo, fbo_tex;
	int    fbo_w, fbo_h;

	struct vtx *buf;
	int         nvtx;
	GLuint      cur_tex;
	int         clip_on, clip_x, clip_y, clip_w, clip_h;
};

/* ------------------------------------------------------------------ *
 * Shaders
 * ------------------------------------------------------------------ */

/*
 * The vertex shader does the pixel->clip mapping itself instead of taking a
 * matrix: the transform is a fixed 2D orthographic flip, so a 4x4 matrix
 * would be 14 multiplications by zero. u_res is the render target size.
 * y is flipped here so the whole UI can use screen coordinates with the
 * origin at the top left, which is how every layout calculation reads.
 */
static const char *VS_UI =
"#version 100\n"
"attribute vec2 a_pos;\n"
"attribute vec2 a_uv;\n"
"attribute vec4 a_col;\n"
"uniform   vec2 u_res;\n"
"varying   vec2 v_uv;\n"
"varying   vec4 v_col;\n"
"void main() {\n"
"    vec2 p = a_pos / u_res;\n"
"    gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
"    v_uv  = a_uv;\n"
"    v_col = a_col;\n"
"}\n";

/*
 * GLSL ES 2.0 only *guarantees* mediump in fragment shaders; highp is
 * optional and the compiler advertises it with GL_FRAGMENT_PRECISION_HIGH.
 * Ask for highp where it exists and fall back where it does not -- and,
 * separately, keep every intermediate small enough that the mediump path
 * is still correct (see the hash in FS_CRT).
 */
#define PH_PRECISION							\
"#ifdef GL_FRAGMENT_PRECISION_HIGH\n"					\
"precision highp float;\n"						\
"#else\n"								\
"precision mediump float;\n"						\
"#endif\n"

static const char *FS_UI =
"#version 100\n"
PH_PRECISION
"uniform sampler2D u_tex;\n"
"varying vec2 v_uv;\n"
"varying vec4 v_col;\n"
"void main() {\n"
"    gl_FragColor = texture2D(u_tex, v_uv) * v_col;\n"
"}\n";

/* Fullscreen pass: a_pos arrives already in normalised device coordinates,
 * so uv is just pos*0.5+0.5 and the y flip resolves itself. */
static const char *VS_FULL =
"#version 100\n"
"attribute vec2 a_pos;\n"
"varying   vec2 v_uv;\n"
"void main() {\n"
"    v_uv = a_pos * 0.5 + 0.5;\n"
"    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
"}\n";

static const char *FS_BLIT =
"#version 100\n"
PH_PRECISION
"uniform sampler2D u_tex;\n"
"varying vec2 v_uv;\n"
"void main() { gl_FragColor = texture2D(u_tex, v_uv); }\n";

/*
 * The CRT pass. Six effects, each independently scalable from Lua so the
 * look can be dialled from "subtle" to "broken arcade monitor":
 *
 *   curvature   barrel-distort the sample coordinate, black outside
 *   aberration  sample R and B a fraction of a pixel apart
 *   glow        four-tap blur added back, i.e. phosphor bleed
 *   scanline    darken alternate display rows
 *   grille      RGB triad mask, the aperture grille of a Trinitron
 *   vignette    corner falloff
 *   noise       animated grain, the only use of u_time
 */
static const char *FS_CRT =
"#version 100\n"
PH_PRECISION
"uniform sampler2D u_tex;\n"
"uniform vec2  u_res;\n"
"uniform float u_time;\n"
"uniform float u_curve;\n"
"uniform float u_scan;\n"
"uniform float u_vig;\n"
"uniform float u_ab;\n"
"uniform float u_glow;\n"
"uniform float u_noise;\n"
"varying vec2  v_uv;\n"
"\n"
"vec2 curve(vec2 uv) {\n"
"    uv = uv * 2.0 - 1.0;\n"
"    vec2 off = abs(uv.yx) / vec2(5.0, 4.0);\n"
"    uv += uv * off * off * u_curve * 3.0;\n"
"    return uv * 0.5 + 0.5;\n"
"}\n"
"\n"
/*
 * Value hash WITHOUT sin().
 *
 * The usual one-liner is fract(sin(dot(p, vec2(12.9898, 78.233))) *
 * 43758.5453).  Fed screen-pixel coordinates it computes a dot product in
 * the tens of thousands, and mediump is a 16-bit float on a great deal of
 * hardware -- its maximum finite value is 65504.  Past that the dot
 * overflows to inf, sin(inf) is NaN, and the NaN propagates into the
 * colour, so everything beyond the line dot(p,k) == 65504 renders black.
 * That is a straight diagonal across the screen, and it is exactly what
 * this shader did before.
 *
 * This version keeps every intermediate under about 1000, so it is
 * correct in mediump as well as highp, and it is cheaper than a sine.
 */
"float hash(vec2 p) {\n"
"    p = fract(p * vec2(443.8975, 441.4234));\n"
"    p += dot(p, p.yx + 19.19);\n"
"    return fract((p.x + p.y) * p.x);\n"
"}\n"
"\n"
"void main() {\n"
"    vec2 uv = curve(v_uv);\n"
"    if (uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0) {\n"
"        gl_FragColor = vec4(0.0, 0.0, 0.0, 1.0);\n"
"        return;\n"
"    }\n"
"    vec2 px = 1.0 / u_res;\n"
"    float ab = u_ab * px.x;\n"
"\n"
"    vec3 col;\n"
"    col.r = texture2D(u_tex, uv + vec2(ab, 0.0)).r;\n"
"    col.g = texture2D(u_tex, uv).g;\n"
"    col.b = texture2D(u_tex, uv - vec2(ab, 0.0)).b;\n"
"\n"
"    if (u_glow > 0.0) {\n"
"        vec3 bl = texture2D(u_tex, uv + vec2( 2.0 * px.x, 0.0)).rgb\n"
"                + texture2D(u_tex, uv - vec2( 2.0 * px.x, 0.0)).rgb\n"
"                + texture2D(u_tex, uv + vec2(0.0,  2.0 * px.y)).rgb\n"
"                + texture2D(u_tex, uv - vec2(0.0,  2.0 * px.y)).rgb;\n"
"        col += bl * 0.25 * u_glow;\n"
"    }\n"
"\n"
    /* One full sine period every two display rows.  Written as
     * fract(y/2)*2pi rather than y*pi so the argument stays in [0,2pi)
     * instead of growing to the height of the display -- the same
     * mediump concern as the hash above. */
"    float s = sin(fract(uv.y * u_res.y * 0.5) * 6.2831853);\n"
"    col *= 1.0 - u_scan * 0.5 * s * s;\n"
"\n"
"    float m = mod(floor(uv.x * u_res.x), 3.0);\n"
"    vec3 grille = vec3(m == 0.0 ? 1.0 : 0.85,\n"
"                       m == 1.0 ? 1.0 : 0.85,\n"
"                       m == 2.0 ? 1.0 : 0.85);\n"
"    col *= mix(vec3(1.0), grille, u_scan);\n"
"\n"
"    vec2 vg = uv * (1.0 - uv);\n"
"    col *= mix(1.0, pow(clamp(vg.x * vg.y * 16.0, 0.0, 1.0), 0.3), u_vig);\n"
"\n"
"    if (u_noise > 0.0)\n"
"        col += (hash(uv + fract(u_time)) - 0.5) * u_noise;\n"
"\n"
"    gl_FragColor = vec4(col, 1.0);\n"
"}\n";

/* ------------------------------------------------------------------ *
 * Shader plumbing
 * ------------------------------------------------------------------ */
static GLuint
compile(GLenum type, const char *src, const char *what)
{
	GLuint s = glCreateShader(type);
	GLint ok = 0;

	glShaderSource(s, 1, &src, NULL);
	glCompileShader(s);
	glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";

		glGetShaderInfoLog(s, (GLsizei)sizeof(log), NULL, log);
		ph_warn("%s shader: %s", what, log);
		glDeleteShader(s);
		return 0;
	}
	return s;
}

static GLuint
link_prog(const char *vs_src, const char *fs_src, const char *what)
{
	GLuint vs, fs, p;
	GLint ok = 0;

	if ((vs = compile(GL_VERTEX_SHADER, vs_src, what)) == 0)
		return 0;
	if ((fs = compile(GL_FRAGMENT_SHADER, fs_src, what)) == 0) {
		glDeleteShader(vs);
		return 0;
	}
	p = glCreateProgram();
	glAttachShader(p, vs);
	glAttachShader(p, fs);
	/* Bind locations explicitly so the batcher can hard-code 0/1/2
	 * instead of querying them per program. */
	glBindAttribLocation(p, 0, "a_pos");
	glBindAttribLocation(p, 1, "a_uv");
	glBindAttribLocation(p, 2, "a_col");
	glLinkProgram(p);
	glGetProgramiv(p, GL_LINK_STATUS, &ok);
	if (!ok) {
		char log[1024] = "";

		glGetProgramInfoLog(p, (GLsizei)sizeof(log), NULL, log);
		ph_warn("%s link: %s", what, log);
		glDeleteProgram(p);
		p = 0;
	}
	glDeleteShader(vs);		/* the program holds its own reference */
	glDeleteShader(fs);
	return p;
}

/* ------------------------------------------------------------------ *
 * Framebuffer object
 * ------------------------------------------------------------------ */
static void
fbo_release(struct ph_gfx *g)
{
	if (g->fbo != 0)     { glDeleteFramebuffers(1, &g->fbo); g->fbo = 0; }
	if (g->fbo_tex != 0) { glDeleteTextures(1, &g->fbo_tex); g->fbo_tex = 0; }
}

static int
fbo_build(struct ph_gfx *g, int w, int h)
{
	GLenum st;

	fbo_release(g);
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	glGenTextures(1, &g->fbo_tex);
	glBindTexture(GL_TEXTURE_2D, g->fbo_tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
	    GL_UNSIGNED_BYTE, NULL);
	/* GL ES 2.0 permits non-power-of-two textures only with CLAMP_TO_EDGE
	 * and without mipmaps, which is exactly what a screen-sized render
	 * target wants anyway. */
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

	glGenFramebuffers(1, &g->fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
	    GL_TEXTURE_2D, g->fbo_tex, 0);
	st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);

	if (st != GL_FRAMEBUFFER_COMPLETE) {
		ph_warn("framebuffer incomplete (0x%04x)", (unsigned)st);
		fbo_release(g);
		return -1;
	}
	g->fbo_w = w;
	g->fbo_h = h;
	return 0;
}

/* ------------------------------------------------------------------ *
 * Lifecycle
 * ------------------------------------------------------------------ */
struct ph_gfx *
ph_gfx_create(int w, int h)
{
	struct ph_gfx *g = ph_xcalloc(1, sizeof(*g));
	const unsigned char white[4] = { 255, 255, 255, 255 };

	g->w = w > 0 ? w : 1;
	g->h = h > 0 ? h : 1;
	g->buf = ph_xmalloc((size_t)MAX_QUADS * VERTS_PER_QUAD * sizeof(struct vtx));

	g->prog_ui   = link_prog(VS_UI,   FS_UI,   "ui");
	g->prog_blit = link_prog(VS_FULL, FS_BLIT, "blit");
	g->prog_crt  = link_prog(VS_FULL, FS_CRT,  "crt");
	if (g->prog_ui == 0 || g->prog_blit == 0) {
		ph_warn("renderer: essential shaders failed to build");
		ph_gfx_destroy(g);
		return NULL;
	}
	g->ui_res   = glGetUniformLocation(g->prog_ui,   "u_res");
	g->ui_tex   = glGetUniformLocation(g->prog_ui,   "u_tex");
	g->blit_tex = glGetUniformLocation(g->prog_blit, "u_tex");
	if (g->prog_crt != 0) {
		g->crt_tex   = glGetUniformLocation(g->prog_crt, "u_tex");
		g->crt_res   = glGetUniformLocation(g->prog_crt, "u_res");
		g->crt_time  = glGetUniformLocation(g->prog_crt, "u_time");
		g->crt_curve = glGetUniformLocation(g->prog_crt, "u_curve");
		g->crt_scan  = glGetUniformLocation(g->prog_crt, "u_scan");
		g->crt_vig   = glGetUniformLocation(g->prog_crt, "u_vig");
		g->crt_ab    = glGetUniformLocation(g->prog_crt, "u_ab");
		g->crt_glow  = glGetUniformLocation(g->prog_crt, "u_glow");
		g->crt_noise = glGetUniformLocation(g->prog_crt, "u_noise");
	}

	glGenBuffers(1, &g->vbo);
	g->white = ph_gfx_tex_rgba(white, 1, 1);

	if (fbo_build(g, g->w, g->h) != 0) {
		ph_gfx_destroy(g);
		return NULL;
	}

	glDisable(GL_DEPTH_TEST);
	glDisable(GL_CULL_FACE);
	glEnable(GL_BLEND);
	/* Straight (non-premultiplied) alpha for colour; the separate alpha
	 * term keeps the render target's own alpha sane when quads overlap. */
	glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
	    GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
	return g;
}

void
ph_gfx_destroy(struct ph_gfx *g)
{
	if (g == NULL)
		return;
	fbo_release(g);
	if (g->vbo != 0)       glDeleteBuffers(1, &g->vbo);
	if (g->white != 0)     glDeleteTextures(1, &g->white);
	if (g->prog_ui != 0)   glDeleteProgram(g->prog_ui);
	if (g->prog_blit != 0) glDeleteProgram(g->prog_blit);
	if (g->prog_crt != 0)  glDeleteProgram(g->prog_crt);
	free(g->buf);
	free(g);
}

void
ph_gfx_resize(struct ph_gfx *g, int w, int h)
{
	if (w < 1) w = 1;
	if (h < 1) h = 1;
	if (w == g->w && h == g->h)
		return;
	g->w = w;
	g->h = h;
	fbo_build(g, w, h);
}

int ph_gfx_w(const struct ph_gfx *g) { return g->w; }
int ph_gfx_h(const struct ph_gfx *g) { return g->h; }

/* ------------------------------------------------------------------ *
 * Batching
 * ------------------------------------------------------------------ */
static void
flush(struct ph_gfx *g)
{
	if (g->nvtx == 0)
		return;

	glUseProgram(g->prog_ui);
	glUniform2f(g->ui_res, (float)g->w, (float)g->h);
	glUniform1i(g->ui_tex, 0);
	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g->cur_tex);

	glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
	glBufferData(GL_ARRAY_BUFFER,
	    (GLsizeiptr)((size_t)g->nvtx * sizeof(struct vtx)), g->buf,
	    GL_STREAM_DRAW);

	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glEnableVertexAttribArray(2);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(struct vtx),
	    (const void *)0);
	glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(struct vtx),
	    (const void *)(sizeof(float) * 2));
	glVertexAttribPointer(2, 4, GL_UNSIGNED_BYTE, GL_TRUE, sizeof(struct vtx),
	    (const void *)(sizeof(float) * 4));

	glDrawArrays(GL_TRIANGLES, 0, g->nvtx);

	glDisableVertexAttribArray(0);
	glDisableVertexAttribArray(1);
	glDisableVertexAttribArray(2);
	g->nvtx = 0;
}

static void
push_quad(struct ph_gfx *g, GLuint tex,
    float x, float y, float w, float h,
    float u0, float v0, float u1, float v1,
    ph_rgba c0, ph_rgba c1)		/* c0 top, c1 bottom */
{
	struct vtx *p;
	unsigned char r0, g0, b0, a0, r1, g1, b1, a1;

	if (w <= 0.0f || h <= 0.0f)
		return;
	if (tex != g->cur_tex || g->nvtx + VERTS_PER_QUAD > MAX_QUADS * VERTS_PER_QUAD) {
		flush(g);
		g->cur_tex = tex;
	}
	r0 = (unsigned char)((c0 >> 24) & 0xff); g0 = (unsigned char)((c0 >> 16) & 0xff);
	b0 = (unsigned char)((c0 >>  8) & 0xff); a0 = (unsigned char)( c0        & 0xff);
	r1 = (unsigned char)((c1 >> 24) & 0xff); g1 = (unsigned char)((c1 >> 16) & 0xff);
	b1 = (unsigned char)((c1 >>  8) & 0xff); a1 = (unsigned char)( c1        & 0xff);

	p = &g->buf[g->nvtx];
#define V(ix, iy, iu, iv, R, G, B, A) do {				\
		p->x = (ix); p->y = (iy); p->u = (iu); p->v = (iv);	\
		p->r = (R); p->g = (G); p->b = (B); p->a = (A);		\
		p++;							\
	} while (0)
	V(x,     y,     u0, v0, r0, g0, b0, a0);
	V(x + w, y,     u1, v0, r0, g0, b0, a0);
	V(x + w, y + h, u1, v1, r1, g1, b1, a1);
	V(x,     y,     u0, v0, r0, g0, b0, a0);
	V(x + w, y + h, u1, v1, r1, g1, b1, a1);
	V(x,     y + h, u0, v1, r1, g1, b1, a1);
#undef V
	g->nvtx += VERTS_PER_QUAD;
}

void
ph_gfx_rect(struct ph_gfx *g, float x, float y, float w, float h, ph_rgba c)
{
	push_quad(g, g->white, x, y, w, h, 0, 0, 1, 1, c, c);
}

void
ph_gfx_rect_vgrad(struct ph_gfx *g, float x, float y, float w, float h,
    ph_rgba top, ph_rgba bottom)
{
	push_quad(g, g->white, x, y, w, h, 0, 0, 1, 1, top, bottom);
}

void
ph_gfx_border(struct ph_gfx *g, float x, float y, float w, float h,
    float t, ph_rgba c)
{
	if (t <= 0.0f || w <= 0.0f || h <= 0.0f)
		return;
	ph_gfx_rect(g, x,         y,         w, t,         c);	/* top    */
	ph_gfx_rect(g, x,         y + h - t, w, t,         c);	/* bottom */
	ph_gfx_rect(g, x,         y + t,     t, h - 2 * t, c);	/* left   */
	ph_gfx_rect(g, x + w - t, y + t,     t, h - 2 * t, c);	/* right  */
}

void
ph_gfx_tex(struct ph_gfx *g, unsigned tex, float x, float y, float w, float h,
    float u0, float v0, float u1, float v1, ph_rgba tint)
{
	push_quad(g, tex != 0 ? (GLuint)tex : g->white,
	    x, y, w, h, u0, v0, u1, v1, tint, tint);
}

/*
 * Clipping.  glScissor's origin is the bottom-left of the render target,
 * while every layout coordinate in this program is top-left -- hence the
 * flip.  A clip change must flush, because scissor state applies at draw
 * time and the batch may already hold quads meant for the old region.
 */
void
ph_gfx_clip(struct ph_gfx *g, int x, int y, int w, int h)
{
	flush(g);
	if (w < 0) w = 0;
	if (h < 0) h = 0;
	glEnable(GL_SCISSOR_TEST);
	glScissor(x, g->h - (y + h), w, h);
	g->clip_on = 1;
	g->clip_x = x; g->clip_y = y; g->clip_w = w; g->clip_h = h;
}

void
ph_gfx_clip_off(struct ph_gfx *g)
{
	flush(g);
	glDisable(GL_SCISSOR_TEST);
	g->clip_on = 0;
}

/* ------------------------------------------------------------------ *
 * Frames
 * ------------------------------------------------------------------ */
void
ph_gfx_frame_begin(struct ph_gfx *g, ph_rgba clear)
{
	if (g->fbo_w != g->w || g->fbo_h != g->h)
		fbo_build(g, g->w, g->h);

	glBindFramebuffer(GL_FRAMEBUFFER, g->fbo);
	glViewport(0, 0, g->w, g->h);
	glDisable(GL_SCISSOR_TEST);
	g->clip_on = 0;
	glClearColor(PH_R(clear), PH_G(clear), PH_B(clear), PH_A(clear));
	glClear(GL_COLOR_BUFFER_BIT);
	g->nvtx = 0;
	g->cur_tex = g->white;
}

/* One triangle covering the screen: cheaper than two and with no seam
 * along the diagonal where the interpolators would otherwise meet. */
static void
fullscreen_tri(struct ph_gfx *g)
{
	static const float tri[6] = { -1.0f, -1.0f, 3.0f, -1.0f, -1.0f, 3.0f };

	glBindBuffer(GL_ARRAY_BUFFER, g->vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(tri), tri, GL_STREAM_DRAW);
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 2 * sizeof(float),
	    (const void *)0);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glDisableVertexAttribArray(0);
}

void
ph_gfx_frame_end(struct ph_gfx *g, const struct ph_theme *t, float time_sec,
    int crt)
{
	flush(g);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glViewport(0, 0, g->w, g->h);
	glDisable(GL_SCISSOR_TEST);
	glDisable(GL_BLEND);

	glActiveTexture(GL_TEXTURE0);
	glBindTexture(GL_TEXTURE_2D, g->fbo_tex);

	if (crt && g->prog_crt != 0 && t != NULL) {
		glUseProgram(g->prog_crt);
		glUniform1i(g->crt_tex,   0);
		glUniform2f(g->crt_res,   (float)g->w, (float)g->h);
		glUniform1f(g->crt_time,  time_sec);
		glUniform1f(g->crt_curve, t->curvature);
		glUniform1f(g->crt_scan,  t->scanline);
		glUniform1f(g->crt_vig,   t->vignette);
		glUniform1f(g->crt_ab,    t->aberration);
		glUniform1f(g->crt_glow,  t->glow);
		glUniform1f(g->crt_noise, t->noise);
	} else {
		glUseProgram(g->prog_blit);
		glUniform1i(g->blit_tex, 0);
	}
	fullscreen_tri(g);
	glEnable(GL_BLEND);
}

/* ------------------------------------------------------------------ *
 * Textures
 * ------------------------------------------------------------------ */
static unsigned
tex_new(const void *px, int w, int h)
{
	GLuint t;

	glGenTextures(1, &t);
	glBindTexture(GL_TEXTURE_2D, t);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
	    GL_UNSIGNED_BYTE, px);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	return t;
}

unsigned
ph_gfx_tex_rgba(const void *pixels, int w, int h)
{
	/* Rows are tightly packed; the default GL_UNPACK_ALIGNMENT of 4 is
	 * fine for RGBA but say so explicitly since the glyph atlas uploads
	 * odd-width sub-rectangles. */
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	return tex_new(pixels, w, h);
}

unsigned
ph_gfx_tex_blank(int w, int h)
{
	unsigned char *zero = ph_xcalloc((size_t)w * (size_t)h, 4);
	unsigned t;

	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	t = tex_new(zero, w, h);
	free(zero);
	return t;
}

void
ph_gfx_tex_sub(unsigned tex, int x, int y, int w, int h, const void *px)
{
	if (w <= 0 || h <= 0)
		return;
	glBindTexture(GL_TEXTURE_2D, (GLuint)tex);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h, GL_RGBA,
	    GL_UNSIGNED_BYTE, px);
}

void
ph_gfx_tex_free(unsigned tex)
{
	GLuint t = (GLuint)tex;

	if (t != 0)
		glDeleteTextures(1, &t);
}

unsigned
ph_gfx_tex_file(const char *path, int *w, int *h)
{
	unsigned char *px;
	unsigned t;
	int iw = 0, ih = 0;

	if ((px = ph_image_load(path, &iw, &ih)) == NULL)
		return 0;
	t = ph_gfx_tex_rgba(px, iw, ih);
	ph_image_free(px);
	if (w != NULL) *w = iw;
	if (h != NULL) *h = ih;
	return t;
}

/*
 * Read the default framebuffer back.  glReadPixels hands back rows
 * bottom-up, so they are flipped here into the top-down order every image
 * format expects.
 */
unsigned char *
ph_gfx_capture(struct ph_gfx *g, int *w, int *h)
{
	size_t stride = (size_t)g->w * 4;
	unsigned char *px = ph_xmalloc(stride * (size_t)g->h);
	unsigned char *row = ph_xmalloc(stride);
	int y;

	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glReadPixels(0, 0, g->w, g->h, GL_RGBA, GL_UNSIGNED_BYTE, px);

	for (y = 0; y < g->h / 2; y++) {
		unsigned char *a = px + (size_t)y * stride;
		unsigned char *b = px + (size_t)(g->h - 1 - y) * stride;

		memcpy(row, a, stride);
		memcpy(a, b, stride);
		memcpy(b, row, stride);
	}
	free(row);
	if (w != NULL) *w = g->w;
	if (h != NULL) *h = g->h;
	return px;
}
