/*
 * text.c -- stb_truetype glyph cache on a GL atlas.
 *
 * ATLAS PACKING
 * A shelf packer: glyphs are laid left to right on a row whose height is
 * the tallest glyph placed on it; when the row fills, a new row starts
 * below.  For a monospace face at a fixed pixel size every glyph is very
 * nearly the same height, so shelf packing wastes almost nothing here and
 * costs a dozen lines instead of the few hundred a skyline packer needs.
 *
 * GLYPH CACHE
 * Open-addressed hash table, power-of-two capacity, linear probing.  Keyed
 * on the Unicode code point.  Linear probing is the right choice because
 * the table is small, never deletes, and is looked up once per character
 * per frame -- cache locality dominates.
 */
#include "text.h"

#include <stdlib.h>
#include <string.h>

#include "stb_truetype.h"

#define CACHE_BITS 12			/* 4096 slots */
#define CACHE_SIZE (1u << CACHE_BITS)
#define CACHE_MASK (CACHE_SIZE - 1u)

struct glyph {
	uint32_t cp;			/* 0 means "empty slot" */
	float    u0, v0, u1, v1;
	float    xoff, yoff;		/* from pen position / baseline */
	float    w, h;
	float    advance;
	int      valid;
};

struct ph_font {
	stbtt_fontinfo info;
	float          scale;
	float          ascent, descent, linegap;
	int            px;

	unsigned       tex;
	int            dim;
	int            pen_x, pen_y, row_h;	/* shelf packer state */

	struct glyph  *cache;
	unsigned char *scratch;		/* RGBA staging for one glyph upload */
	size_t         scratch_sz;
};

/* ------------------------------------------------------------------ *
 * Construction
 * ------------------------------------------------------------------ */
struct ph_font *
ph_font_create(const unsigned char *ttf, size_t len, int px)
{
	struct ph_font *f;
	int a, d, g, off;

	if (ttf == NULL || len == 0 || px < 4)
		return NULL;

	f = ph_xcalloc(1, sizeof(*f));
	f->px = px;

	/* A .otf/.ttf may be a collection; index 0 is the only face here. */
	if ((off = stbtt_GetFontOffsetForIndex(ttf, 0)) < 0 ||
	    !stbtt_InitFont(&f->info, ttf, off)) {
		ph_warn("text: could not parse the baked typeface");
		free(f);
		return NULL;
	}
	f->scale = stbtt_ScaleForPixelHeight(&f->info, (float)px);
	stbtt_GetFontVMetrics(&f->info, &a, &d, &g);
	f->ascent  = (float)a * f->scale;
	f->descent = (float)d * f->scale;		/* negative */
	f->linegap = (float)g * f->scale;

	/* Atlas big enough for the Latin ranges plus a healthy number of
	 * icons at this size, without reserving 4MB for a 14px face. */
	f->dim = (px <= 22) ? 512 : 1024;
	f->tex = ph_gfx_tex_blank(f->dim, f->dim);
	f->pen_x = f->pen_y = f->row_h = 1;	/* 1px margin avoids bleeding */

	f->cache = ph_xcalloc(CACHE_SIZE, sizeof(*f->cache));
	f->scratch_sz = (size_t)(px + 8) * (size_t)(px + 8) * 4 * 4;
	f->scratch = ph_xmalloc(f->scratch_sz);
	return f;
}

void
ph_font_destroy(struct ph_font *f)
{
	if (f == NULL)
		return;
	if (f->tex != 0)
		ph_gfx_tex_free(f->tex);
	free(f->cache);
	free(f->scratch);
	free(f);
}

float ph_font_height(const struct ph_font *f)
{
	return f == NULL ? 0.0f : f->ascent - f->descent + f->linegap;
}

float ph_font_ascent(const struct ph_font *f)
{
	return f == NULL ? 0.0f : f->ascent;
}

/* ------------------------------------------------------------------ *
 * Glyph cache
 * ------------------------------------------------------------------ */
static uint32_t
hash_cp(uint32_t cp)
{
	/* Knuth multiplicative: code points arrive in dense runs (ASCII, or
	 * one PUA block), and a plain mask would pile them into adjacent
	 * slots. */
	return (cp * 2654435761u) >> (32 - CACHE_BITS);
}

static struct glyph *
cache_slot(struct ph_font *f, uint32_t cp)
{
	uint32_t i = hash_cp(cp);
	unsigned probes = 0;

	for (;;) {
		struct glyph *s = &f->cache[i & CACHE_MASK];

		if (s->cp == cp && s->valid)
			return s;
		if (s->cp == 0)
			return s;		/* free slot */
		if (++probes > CACHE_SIZE)
			return NULL;		/* full: refuse rather than spin */
		i++;
	}
}

/* Rasterise `cp` into the atlas and fill *out. */
static int
glyph_bake(struct ph_font *f, uint32_t cp, struct glyph *out)
{
	unsigned char *bmp;
	int gw = 0, gh = 0, xo = 0, yo = 0, adv = 0, lsb = 0, x, y;

	stbtt_GetCodepointHMetrics(&f->info, (int)cp, &adv, &lsb);
	out->cp = cp;
	out->advance = (float)adv * f->scale;
	out->valid = 1;
	out->u0 = out->v0 = out->u1 = out->v1 = 0.0f;
	out->w = out->h = out->xoff = out->yoff = 0.0f;

	bmp = stbtt_GetCodepointBitmap(&f->info, 0, f->scale, (int)cp,
	    &gw, &gh, &xo, &yo);
	if (bmp == NULL || gw <= 0 || gh <= 0) {
		/* Space, or a code point with no outline: advance only. */
		if (bmp != NULL)
			stbtt_FreeBitmap(bmp, NULL);
		return 1;
	}

	/* Shelf packing, with a one-pixel gutter so linear filtering at the
	 * glyph edge cannot pick up a neighbour. */
	if (f->pen_x + gw + 1 >= f->dim) {
		f->pen_x = 1;
		f->pen_y += f->row_h + 1;
		f->row_h = 0;
	}
	if (f->pen_y + gh + 1 >= f->dim) {
		stbtt_FreeBitmap(bmp, NULL);
		ph_warn("text: glyph atlas full at %dpx (U+%04X dropped)",
		    f->px, cp);
		return 0;
	}

	/* stb hands back 8-bit coverage; the renderer has exactly one
	 * texture format, so expand to white-with-alpha RGBA. */
	if ((size_t)gw * (size_t)gh * 4 > f->scratch_sz) {
		f->scratch_sz = (size_t)gw * (size_t)gh * 4;
		f->scratch = ph_xrealloc(f->scratch, f->scratch_sz);
	}
	for (y = 0; y < gh; y++) {
		for (x = 0; x < gw; x++) {
			unsigned char *p = f->scratch + ((size_t)y * gw + x) * 4;

			p[0] = p[1] = p[2] = 255;
			p[3] = bmp[(size_t)y * gw + x];
		}
	}
	ph_gfx_tex_sub(f->tex, f->pen_x, f->pen_y, gw, gh, f->scratch);
	stbtt_FreeBitmap(bmp, NULL);

	out->u0 = (float)f->pen_x / (float)f->dim;
	out->v0 = (float)f->pen_y / (float)f->dim;
	out->u1 = (float)(f->pen_x + gw) / (float)f->dim;
	out->v1 = (float)(f->pen_y + gh) / (float)f->dim;
	out->w  = (float)gw;
	out->h  = (float)gh;
	out->xoff = (float)xo;
	out->yoff = (float)yo;		/* relative to the baseline, usually < 0 */

	f->pen_x += gw + 1;
	if (gh > f->row_h)
		f->row_h = gh;
	return 1;
}

static struct glyph *
glyph_get(struct ph_font *f, uint32_t cp)
{
	struct glyph *s = cache_slot(f, cp);

	if (s == NULL)
		return NULL;
	if (s->valid && s->cp == cp)
		return s;
	if (!glyph_bake(f, cp, s))
		return NULL;
	return s;
}

/* ------------------------------------------------------------------ *
 * Measurement
 * ------------------------------------------------------------------ */
float
ph_text_width_n(struct ph_font *f, const char *s, int len)
{
	const char *p = s, *end;
	float w = 0.0f;
	uint32_t cp, prev = 0;

	if (f == NULL || s == NULL)
		return 0.0f;
	end = (len < 0) ? NULL : s + len;

	while (*p != '\0' && (end == NULL || p < end)) {
		const char *before = p;
		struct glyph *gl;

		if ((cp = ph_utf8_next(&p)) == 0)
			break;
		if (end != NULL && p > end) {	/* a multibyte char straddling
						 * the limit: stop cleanly */
			p = before;
			break;
		}
		if (cp == '\n')
			continue;
		if ((gl = glyph_get(f, cp)) == NULL)
			continue;
		if (prev != 0)
			w += (float)stbtt_GetCodepointKernAdvance(&f->info,
			    (int)prev, (int)cp) * f->scale;
		w += gl->advance;
		prev = cp;
	}
	return w;
}

float
ph_text_width(struct ph_font *f, const char *s)
{
	return ph_text_width_n(f, s, -1);
}

/* ------------------------------------------------------------------ *
 * Drawing
 * ------------------------------------------------------------------ */
float
ph_text_draw_n(struct ph_gfx *g, struct ph_font *f, float x, float y,
    const char *s, int len, ph_rgba c)
{
	const char *p = s, *end;
	float pen = x, base;
	uint32_t cp, prev = 0;

	/* The guard must precede any use of f: base was previously computed
	 * in the initializer, which dereferenced f before this check. */
	if (f == NULL || s == NULL)
		return x;
	base = y + f->ascent;
	end = (len < 0) ? NULL : s + len;

	while (*p != '\0' && (end == NULL || p < end)) {
		const char *before = p;
		struct glyph *gl;

		if ((cp = ph_utf8_next(&p)) == 0)
			break;
		if (end != NULL && p > end) {
			p = before;
			break;
		}
		if (cp == '\n')
			continue;
		if ((gl = glyph_get(f, cp)) == NULL)
			continue;
		if (prev != 0)
			pen += (float)stbtt_GetCodepointKernAdvance(&f->info,
			    (int)prev, (int)cp) * f->scale;
		if (gl->w > 0.0f)
			ph_gfx_tex(g, f->tex,
			    pen + gl->xoff, base + gl->yoff, gl->w, gl->h,
			    gl->u0, gl->v0, gl->u1, gl->v1, c);
		pen += gl->advance;
		prev = cp;
	}
	return pen;
}

float
ph_text_draw(struct ph_gfx *g, struct ph_font *f, float x, float y,
    const char *s, ph_rgba c)
{
	return ph_text_draw_n(g, f, x, y, s, -1, c);
}

float
ph_text_draw_clip(struct ph_gfx *g, struct ph_font *f, float x, float y,
    float maxw, const char *s, ph_rgba c)
{
	static const char ell[] = "\xe2\x80\xa6";	/* U+2026 */
	const char *p = s;
	float ellw, pen = x, limit;
	uint32_t cp, prev = 0;

	if (f == NULL || s == NULL)
		return x;
	if (ph_text_width(f, s) <= maxw)
		return ph_text_draw(g, f, x, y, s, c);

	ellw = ph_text_width(f, ell);
	limit = x + maxw - ellw;

	while (*p != '\0') {
		struct glyph *gl;
		float adv;

		if ((cp = ph_utf8_next(&p)) == 0)
			break;
		if (cp == '\n')
			continue;
		if ((gl = glyph_get(f, cp)) == NULL)
			continue;
		adv = gl->advance;
		if (prev != 0)
			adv += (float)stbtt_GetCodepointKernAdvance(&f->info,
			    (int)prev, (int)cp) * f->scale;
		if (pen + adv > limit)
			break;
		if (gl->w > 0.0f)
			ph_gfx_tex(g, f->tex,
			    pen + gl->xoff, y + f->ascent + gl->yoff,
			    gl->w, gl->h,
			    gl->u0, gl->v0, gl->u1, gl->v1, c);
		pen += adv;
		prev = cp;
	}
	return ph_text_draw(g, f, pen, y, ell, c);
}

/* ------------------------------------------------------------------ *
 * Word wrapping
 *
 * Greedy: accumulate words until the next one would overflow, then break.
 * Explicit '\n' always breaks.  A single word longer than the line is
 * emitted on its own line and allowed to overflow rather than being cut
 * mid-word, which reads better for a URL or a long identifier.
 * ------------------------------------------------------------------ */
int
ph_text_wrap(struct ph_font *f, const char *s, float maxw,
    struct ph_line *out, int max_lines)
{
	const char *line = s, *word = s, *p = s;
	int n = 0;

	if (f == NULL || s == NULL || max_lines <= 0)
		return 0;

	while (n < max_lines) {
		if (*p == '\0' || *p == '\n') {
			/*
			 * The run up to a terminator still has to fit.  Without
			 * this check the last line of a paragraph was emitted
			 * at whatever width it happened to be and ran past the
			 * panel, because the break test below only fires on a
			 * space.
			 */
			if (word > line &&
			    ph_text_width_n(f, line, (int)(p - line)) > maxw) {
				out[n].p = line;
				out[n].len = (int)(word - line - 1);
				n++;
				line = word;
				if (n >= max_lines)
					break;
			}
			out[n].p = line;
			out[n].len = (int)(p - line);
			n++;
			if (*p == '\0')
				break;
			line = word = ++p;
			continue;
		}
		if (*p == ' ') {
			/* Candidate break point: does the line still fit? */
			if (ph_text_width_n(f, line, (int)(p - line)) > maxw &&
			    word > line) {
				out[n].p = line;
				out[n].len = (int)(word - line - 1);
				n++;
				line = word;
				continue;
			}
			word = p + 1;
		}
		p++;
	}
	/* Trailing fragment that did not fit in max_lines is dropped by
	 * design: the caller asked for at most that many lines. */
	return n;
}
