/*
 * font.c - stb_truetype font rendering.
 *
 * Features beyond a basic glyph blitter:
 *   - sub-pixel accurate glyph positioning (stbtt_*Subpixel)
 *   - kerning between adjacent glyphs
 *   - UTF-8 decoding, so accented characters render as single glyphs
 *   - multi-line text: '\n' splits the string into stacked lines
 *   - optional soft drop shadow (coverage buffer + separable box blur)
 *   - opacity: the element's master alpha scales glyph and shadow alike
 *
 * stb_truetype already produces anti-aliased coverage bitmaps, so text AA
 * has always been present; these additions just make it crisp and evenly
 * spaced. True hinting would need a different rasteriser (FreeType) and is
 * deliberately avoided - it would break the zero-runtime-dependency goal.
 */

#include "splash.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Upper bound on lines in one text element; bounds the coverage buffer. */
#define MAX_TEXT_LINES 32

static font_t g_fonts[MAX_FONTS];

/* ========================================================================
 * Font Management
 * ======================================================================== */

int font_load(const char *path, float pixel_height, int slot) {
	if (slot < 0 || slot >= MAX_FONTS)
		return -1;
	if (pixel_height < 8.0f || pixel_height > MAX_FONT_SIZE)
		return -1;

	font_unload(slot);

	FILE *fp = fopen(path, "rb");
	if (!fp)
		return -1;

	fseek(fp, 0, SEEK_END);
	long size = ftell(fp);
	fseek(fp, 0, SEEK_SET);
	if (size <= 0) {
		fclose(fp);
		return -1;
	}

	uint8_t *data = malloc((size_t)size);
	if (!data || fread(data, 1, (size_t)size, fp) != (size_t)size) {
		free(data);
		fclose(fp);
		return -1;
	}
	fclose(fp);

	stbtt_fontinfo *info = calloc(1, sizeof(stbtt_fontinfo));
	if (!info) {
		free(data);
		return -1;
	}

	if (!stbtt_InitFont(info, data, stbtt_GetFontOffsetForIndex(data, 0))) {
		free(info);
		free(data);
		return -1;
	}

	g_fonts[slot].info         = info;
	g_fonts[slot].data         = data;
	g_fonts[slot].data_size    = (size_t)size;
	g_fonts[slot].pixel_height = pixel_height;
	g_fonts[slot].scale        = stbtt_ScaleForPixelHeight(info, pixel_height);

	int ascent, descent, line_gap;
	stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
	g_fonts[slot].ascent   = (int)(ascent   * g_fonts[slot].scale);
	g_fonts[slot].descent  = (int)(descent  * g_fonts[slot].scale);
	g_fonts[slot].line_gap = (int)(line_gap * g_fonts[slot].scale);
	g_fonts[slot].baseline = g_fonts[slot].ascent;
	g_fonts[slot].loaded   = 1;
	strncpy(g_fonts[slot].path, path, sizeof(g_fonts[slot].path) - 1);

	return 0;
}

void font_unload(int slot) {
	if (slot < 0 || slot >= MAX_FONTS)
		return;
	if (g_fonts[slot].info)
		free(g_fonts[slot].info);
	if (g_fonts[slot].data)
		free(g_fonts[slot].data);
	memset(&g_fonts[slot], 0, sizeof(font_t));
}

void font_unload_all(void) {
	for (int i = 0; i < MAX_FONTS; i++)
		font_unload(i);
}

/* Resolve a font slot, falling back to slot 0 if the request is invalid
 * or unloaded - so text never silently vanishes over a bad font id. */
static font_t *get_font(int slot) {
	if (slot < 0 || slot >= MAX_FONTS || !g_fonts[slot].loaded) {
		if (g_fonts[0].loaded)
			return &g_fonts[0];
		return NULL;
	}
	return &g_fonts[slot];
}

/* ========================================================================
 * UTF-8 Decoding
 * ======================================================================== */

/* Decode one UTF-8 codepoint and advance *p past it. */
static int utf8_next(const char **p) {
	const unsigned char *s = (const unsigned char *)*p;
	unsigned int c = s[0];

	if (c < 0x80) {
		*p += 1;
		return (int)c;
	}
	if ((c & 0xE0) == 0xC0 && (s[1] & 0xC0) == 0x80) {
		*p += 2;
		return (int)(((c & 0x1F) << 6) | (s[1] & 0x3F));
	}
	if ((c & 0xF0) == 0xE0 && (s[1] & 0xC0) == 0x80 &&
	    (s[2] & 0xC0) == 0x80) {
		*p += 3;
		return (int)(((c & 0x0F) << 12) | ((s[1] & 0x3F) << 6) |
		             (s[2] & 0x3F));
	}
	if ((c & 0xF8) == 0xF0 && (s[1] & 0xC0) == 0x80 &&
	    (s[2] & 0xC0) == 0x80 && (s[3] & 0xC0) == 0x80) {
		*p += 4;
		return (int)(((c & 0x07) << 18) | ((s[1] & 0x3F) << 12) |
		             ((s[2] & 0x3F) << 6) | (s[3] & 0x3F));
	}

	*p += 1;					/* invalid lead byte - skip it */
	return 0xFFFD;
}

/* ========================================================================
 * Text Measurement
 * ======================================================================== */

/* Advance width of a length-bounded slice (one line of a string),
 * including kerning between adjacent glyphs. */
static float measure_line_scaled(stbtt_fontinfo *info, float scale,
                                  const char *text, int len) {
	float x = 0.0f;
	const char *p   = text;
	const char *end = text + len;
	int prev = 0;

	while (p < end) {
		int cp = utf8_next(&p);
		if (prev)
			x += stbtt_GetCodepointKernAdvance(info, prev, cp) * scale;
		int advance, lsb;
		stbtt_GetCodepointHMetrics(info, cp, &advance, &lsb);
		x += advance * scale;
		prev = cp;
	}
	return x;
}

/* ========================================================================
 * Coverage Buffer Helpers
 * ======================================================================== */

/* Composite an 8-bit coverage buffer onto the framebuffer in `color`. */
static void composite_coverage(drm_buffer_t *buf, const uint8_t *cov,
                               int cov_w, int cov_h, int dx, int dy,
                               uint32_t color) {
	uint32_t ca = color >> 24;
	uint8_t  cr = (color >> 16) & 0xFF;
	uint8_t  cg = (color >>  8) & 0xFF;
	uint8_t  cb =  color        & 0xFF;
	if (ca == 0)
		return;

	for (int yy = 0; yy < cov_h; yy++) {
		int py = dy + yy;
		if (py < 0 || py >= (int)buf->height)
			continue;
		const uint8_t *crow = cov + (size_t)yy * cov_w;
		uint32_t *drow = (uint32_t *)(buf->map + py * buf->pitch);
		for (int xx = 0; xx < cov_w; xx++) {
			int px = dx + xx;
			if (px < 0 || px >= (int)buf->width)
				continue;
			uint8_t c = crow[xx];
			if (c == 0)
				continue;
			uint32_t a = (ca * c) / 255;
			if (a == 0)
				continue;
			blend_pixel(&drow[px], argb((uint8_t)a, cr, cg, cb));
		}
	}
}

/*
 * One separable box-blur pass, horizontal or vertical. Edge samples
 * average over only the taps that are in range, so the border does not
 * darken towards the buffer edge.
 */
static void box_blur_pass(const uint8_t *src, uint8_t *dst,
                          int w, int h, int radius, int horizontal) {
	if (horizontal) {
		for (int y = 0; y < h; y++) {
			const uint8_t *s = src + (size_t)y * w;
			uint8_t *d = dst + (size_t)y * w;
			for (int x = 0; x < w; x++) {
				int sum = 0, n = 0;
				int lo = x - radius, hi = x + radius;
				if (lo < 0)
					lo = 0;
				if (hi > w - 1)
					hi = w - 1;
				for (int k = lo; k <= hi; k++) {
					sum += s[k];
					n++;
				}
				d[x] = (uint8_t)(sum / (n > 0 ? n : 1));
			}
		}
	} else {
		for (int x = 0; x < w; x++) {
			for (int y = 0; y < h; y++) {
				int sum = 0, n = 0;
				int lo = y - radius, hi = y + radius;
				if (lo < 0)
					lo = 0;
				if (hi > h - 1)
					hi = h - 1;
				for (int k = lo; k <= hi; k++) {
					sum += src[(size_t)k * w + x];
					n++;
				}
				dst[(size_t)y * w + x] =
					(uint8_t)(sum / (n > 0 ? n : 1));
			}
		}
	}
}

/* Three box passes approximate a Gaussian blur of roughly `blur` pixels. */
static void box_blur(uint8_t *buf, int w, int h, int blur) {
	if (blur < 1 || w <= 0 || h <= 0)
		return;
	uint8_t *tmp = malloc((size_t)w * h);
	if (!tmp)
		return;

	int r = blur / 3;
	if (r < 1)
		r = 1;
	for (int pass = 0; pass < 3; pass++) {
		box_blur_pass(buf, tmp, w, h, r, 1);	/* horizontal: buf -> tmp */
		box_blur_pass(tmp, buf, w, h, r, 0);	/* vertical:   tmp -> buf */
	}
	free(tmp);
}

/* ========================================================================
 * Glyph Rasterisation
 * ======================================================================== */

/*
 * Rasterise one line of text into the coverage buffer. Works purely in
 * buffer coordinates: `pen_x` is the starting pen position and
 * `baseline_y` the baseline row. Sub-pixel positioned and kerned.
 */
static void rasterize_line(stbtt_fontinfo *info, float scale,
                           uint8_t *cov, int cov_w, int cov_h,
                           float pen_x, int baseline_y,
                           const char *text, int len) {
	const char *p   = text;
	const char *end = text + len;
	float pen = pen_x;
	int prev = 0;

	while (p < end) {
		int cp = utf8_next(&p);
		if (prev)
			pen += stbtt_GetCodepointKernAdvance(info, prev, cp) * scale;

		int ix = (int)floorf(pen);
		float shift = pen - (float)ix;		/* sub-pixel fraction */

		int gw, gh, gxoff, gyoff;
		unsigned char *bm = stbtt_GetCodepointBitmapSubpixel(
			info, scale, scale, shift, 0.0f, cp,
			&gw, &gh, &gxoff, &gyoff);

		if (bm) {
			int gx = ix + gxoff;
			int gy = baseline_y + gyoff;
			for (int row = 0; row < gh; row++) {
				int cy = gy + row;
				if (cy < 0 || cy >= cov_h)
					continue;
				const unsigned char *brow = bm + (size_t)row * gw;
				uint8_t *crow = cov + (size_t)cy * cov_w;
				for (int col = 0; col < gw; col++) {
					int cx = gx + col;
					if (cx < 0 || cx >= cov_w)
						continue;
					if (brow[col] > crow[cx])
						crow[cx] = brow[col];
				}
			}
			stbtt_FreeBitmap(bm, NULL);
		}

		int advance, lsb;
		stbtt_GetCodepointHMetrics(info, cp, &advance, &lsb);
		pen += advance * scale;
		prev = cp;
	}
}

/* ========================================================================
 * Text Rendering
 * ======================================================================== */

void draw_text_element(drm_buffer_t *buf, text_element_t *te) {
	font_t *f = get_font(te->font_slot);
	if (!f || !te->text[0])
		return;

	float op = te->opacity;
	if (op <= 0.0f)
		return;

	stbtt_fontinfo *info = f->info;
	float scale = f->scale;
	if (te->font_size > 0)
		scale = stbtt_ScaleForPixelHeight(info, te->font_size);

	/* Vertical metrics at this scale. */
	int ascent, descent, line_gap;
	stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
	int asc_px  = (int)(ascent  * scale);
	int desc_px = (int)(descent * scale);
	int th = asc_px - desc_px;			/* single-line height */
	int line_adv = th + (int)(line_gap * scale);	/* baseline-to-baseline */
	if (line_adv < 1)
		line_adv = 1;

	/* Split the text into lines on '\n'. */
	const char *line_ptr[MAX_TEXT_LINES];
	int         line_len[MAX_TEXT_LINES];
	int nlines = 0;
	{
		const char *start = te->text;
		for (const char *s = te->text;; s++) {
			if (*s == '\n' || *s == '\0') {
				if (nlines < MAX_TEXT_LINES) {
					line_ptr[nlines] = start;
					line_len[nlines] = (int)(s - start);
					nlines++;
				}
				if (*s == '\0')
					break;
				start = s + 1;
			}
		}
	}
	if (nlines == 0)
		return;

	/* Measure: widest line plus total block height. */
	float lw[MAX_TEXT_LINES];
	int   maxw = 0;
	for (int i = 0; i < nlines; i++) {
		lw[i] = measure_line_scaled(info, scale,
		                            line_ptr[i], line_len[i]);
		int w = (int)(lw[i] + 0.5f);
		if (w > maxw)
			maxw = w;
	}
	int block_h = (nlines - 1) * line_adv + th;
	if (maxw <= 0 || block_h <= 0)
		return;

	/* Block placement: valign positions the whole block, align positions
	 * each line within it (relative to the anchor point te->x). */
	int block_x;					/* fb x of the buffer's left */
	if      (te->align == ALIGN_CENTER) block_x = te->x - maxw / 2;
	else if (te->align == ALIGN_RIGHT)  block_x = te->x - maxw;
	else                                block_x = te->x;

	int block_y;					/* fb y of the block's top */
	if      (te->valign == VALIGN_MIDDLE) block_y = te->y - block_h / 2;
	else if (te->valign == VALIGN_BOTTOM) block_y = te->y - block_h;
	else                                  block_y = te->y;

	/* The whole block is rasterised once into an 8-bit coverage buffer.
	 * It is composited twice: blurred and offset as the shadow, then
	 * sharp in the text colour on top. `pad` leaves room for glyph
	 * overhang and for the blur to spread into. */
	int pad = te->shadow
	          ? ((te->shadow_blur > 0 ? te->shadow_blur : 0) + 3) : 3;
	int cov_w = maxw + 2 * pad;
	int cov_h = block_h + 2 * pad;
	int origin_x = block_x - pad;			/* fb position of cov[0,0] */
	int origin_y = block_y - pad;

	uint8_t *cov = calloc((size_t)cov_w * cov_h, 1);
	if (!cov)
		return;

	/* Rasterise every line into the shared coverage buffer. */
	for (int i = 0; i < nlines; i++) {
		float x_off;				/* line's left within the buffer */
		if      (te->align == ALIGN_CENTER)
			x_off = pad + (maxw - lw[i]) * 0.5f;
		else if (te->align == ALIGN_RIGHT)
			x_off = pad + (maxw - lw[i]);
		else
			x_off = pad;

		int baseline = pad + asc_px + i * line_adv;
		rasterize_line(info, scale, cov, cov_w, cov_h,
		               x_off, baseline, line_ptr[i], line_len[i]);
	}

	/* Drop shadow: a blurred copy, composited first so it sits behind. */
	if (te->shadow) {
		uint8_t *sh = malloc((size_t)cov_w * cov_h);
		if (sh) {
			memcpy(sh, cov, (size_t)cov_w * cov_h);
			box_blur(sh, cov_w, cov_h, te->shadow_blur);
			composite_coverage(buf, sh, cov_w, cov_h,
			                   origin_x + te->shadow_dx,
			                   origin_y + te->shadow_dy,
			                   apply_opacity(te->shadow_color, op));
			free(sh);
		}
	}

	/* The text itself, sharp, on top. */
	composite_coverage(buf, cov, cov_w, cov_h, origin_x, origin_y,
	                   apply_opacity(te->color, op));

	free(cov);
}
