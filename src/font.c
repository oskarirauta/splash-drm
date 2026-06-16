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
#include "stb/stb_truetype.h"

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

	char resolved[PATH_MAX];
	if (resolve_font_path(path, resolved, sizeof(resolved)) == 0)
		path = resolved;

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

/*
 * Lookup table for sRGB re-encoding: g_srgb_lut[i] = sqrt(i/255) * 255.
 * Indexed by a linear-light value in 0..255; result is the sRGB byte.
 * Initialized once by init_srgb_lut() before the first draw call.
 *
 * Why: blending text coverage in sRGB (display) space makes 50% coverage
 * appear as only ~21% brightness due to the display's gamma curve.  Blending
 * in linear light and re-encoding to sRGB fixes this, producing visibly
 * softer, more natural anti-aliased edges.
 */
static uint8_t g_srgb_lut[256];
static int     g_srgb_lut_ready;

static void init_srgb_lut(void) {
	if (g_srgb_lut_ready)
		return;
	for (int i = 0; i < 256; i++)
		g_srgb_lut[i] = (uint8_t)(sqrtf((float)i / 255.0f) * 255.0f + 0.5f);
	g_srgb_lut_ready = 1;
}

/*
 * Composite an 8-bit coverage buffer onto the framebuffer in `color`,
 * blending in linearised light (gamma=2 approximation) for smooth AA edges.
 *
 * Source `color` is straight-alpha ARGB.  Destination is premultiplied ARGB.
 * Un-premultiply the destination before linearising; re-premultiply output.
 */
static void composite_coverage(drm_buffer_t *buf, const uint8_t *cov,
                               int cov_w, int cov_h, int dx, int dy,
                               uint32_t color,
                               int clip_x0, int clip_y0,
                               int clip_x1, int clip_y1) {
	uint32_t ca = color >> 24;
	uint8_t  cr = (color >> 16) & 0xFF;
	uint8_t  cg = (color >>  8) & 0xFF;
	uint8_t  cb =  color        & 0xFF;
	if (ca == 0)
		return;

	/* Linearise source colour once (sRGB² / 255, result in 0..255). */
	uint32_t slr = (uint32_t)cr * cr / 255;
	uint32_t slg = (uint32_t)cg * cg / 255;
	uint32_t slb = (uint32_t)cb * cb / 255;

	for (int yy = 0; yy < cov_h; yy++) {
		int py = dy + yy;
		if (py < clip_y0 || py >= clip_y1)
			continue;
		const uint8_t *crow = cov + (size_t)yy * cov_w;
		uint32_t *drow = (uint32_t *)(buf->map + py * buf->pitch);
		for (int xx = 0; xx < cov_w; xx++) {
			int px = dx + xx;
			if (px < clip_x0 || px >= clip_x1)
				continue;
			uint8_t c = crow[xx];
			if (c == 0)
				continue;
			uint32_t a = (ca * (uint32_t)c) / 255;
			if (a == 0)
				continue;

			uint32_t d  = drow[px];
			uint8_t  da = d >> 24;
			uint8_t  dr = (d >> 16) & 0xFF;
			uint8_t  dg = (d >>  8) & 0xFF;
			uint8_t  db =  d        & 0xFF;

			/* Un-premultiply destination to straight-alpha RGB. */
			if (da > 0 && da < 255) {
				dr = (uint8_t)((uint32_t)dr * 255 / da);
				dg = (uint8_t)((uint32_t)dg * 255 / da);
				db = (uint8_t)((uint32_t)db * 255 / da);
			}

			/* Linearise destination and blend in linear light.
			 * With inv_a = 255-a, the sum slr*a + dlr*inv_a is bounded
			 * by 255*255 = 65025, so dividing by 255 stays in 0..255. */
			uint32_t inv_a = 255 - a;
			uint32_t dlr   = (uint32_t)dr * dr / 255;
			uint32_t dlg   = (uint32_t)dg * dg / 255;
			uint32_t dlb   = (uint32_t)db * db / 255;

			uint32_t rl = (slr * a + dlr * inv_a) / 255;
			uint32_t gl = (slg * a + dlg * inv_a) / 255;
			uint32_t bl = (slb * a + dlb * inv_a) / 255;

			/* Re-encode to sRGB via the lookup table. */
			uint8_t rr = g_srgb_lut[rl];
			uint8_t rg = g_srgb_lut[gl];
			uint8_t rb = g_srgb_lut[bl];

			/* Output alpha and premultiplied RGB. */
			uint32_t ra = a + (((uint32_t)da * inv_a) >> 8);
			drow[px] = argb((uint8_t)ra,
			                (uint8_t)((uint32_t)rr * ra / 255),
			                (uint8_t)((uint32_t)rg * ra / 255),
			                (uint8_t)((uint32_t)rb * ra / 255));
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
 * Word Wrap
 * ======================================================================== */

/*
 * Reflow `src` into `dst` so that no line exceeds `max_w` pixels.
 * Hard '\n' characters in the source are preserved and reset the line
 * width. Spaces at word boundaries are replaced by '\n' when wrapping;
 * a word that is wider than max_w on its own is placed on its own line
 * rather than split mid-glyph.
 */
static void wrap_text(stbtt_fontinfo *info, float scale,
                      const char *src, char *dst, size_t dst_sz, int max_w) {
	int   adv, lsb;
	stbtt_GetCodepointHMetrics(info, ' ', &adv, &lsb);
	float space_w = adv * scale;

	size_t      dpos   = 0;
	float       line_w = 0.0f;
	const char *p      = src;

	while (*p && dpos < dst_sz - 1) {
		/* Hard line break: propagate and reset. */
		if (*p == '\n') {
			dst[dpos++] = '\n';
			line_w = 0.0f;
			p++;
			continue;
		}

		/* Skip inter-word spaces (we reinsert them ourselves). */
		if (*p == ' ') {
			p++;
			continue;
		}

		/* Find end of word (ASCII-safe: UTF-8 bytes >0x7F never equal ' '/'\n'). */
		const char *word = p;
		while (*p && *p != ' ' && *p != '\n')
			p++;
		int   wlen  = (int)(p - word);
		float word_w = measure_line_scaled(info, scale, word, wlen);

		if (line_w > 0.0f) {
			if (line_w + space_w + word_w > (float)max_w) {
				/* Wrap: start a new line. */
				if (dpos < dst_sz - 1)
					dst[dpos++] = '\n';
				line_w = 0.0f;
			} else {
				/* Fits on current line: add a space. */
				if (dpos < dst_sz - 1)
					dst[dpos++] = ' ';
				line_w += space_w;
			}
		}

		/* Copy the word. */
		for (int i = 0; i < wlen && dpos < dst_sz - 1; i++)
			dst[dpos++] = word[i];
		line_w += word_w;
	}
	dst[dpos] = '\0';
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

	init_srgb_lut();

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

	/* Optional word wrap: reflow the text into a temporary buffer. */
	char        wrap_buf[sizeof(te->text)];
	const char *text_src = te->text;
	if (te->wrap) {
		int max_w = (te->wrap_width > 0) ? te->wrap_width : (int)buf->width;
		wrap_text(info, scale, te->text, wrap_buf, sizeof(wrap_buf), max_w);
		text_src = wrap_buf;
	}

	/* Split the text into lines on '\n'. */
	const char *line_ptr[MAX_TEXT_LINES];
	int         line_len[MAX_TEXT_LINES];
	int nlines = 0;
	{
		const char *start = text_src;
		for (const char *s = text_src;; s++) {
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

	/* Resolve the anchor point: a negative te->x / te->y centres on that
	 * axis. valign then positions the whole block, align each line within
	 * it, relative to that anchor. */
	int anchor_x = (te->x < 0) ? (int)buf->width  / 2 : te->x;
	int anchor_y = (te->y < 0) ? (int)buf->height / 2 : te->y;

	int block_x;					/* fb x of the buffer's left */
	if      (te->align == ALIGN_CENTER) block_x = anchor_x - maxw / 2;
	else if (te->align == ALIGN_RIGHT)  block_x = anchor_x - maxw;
	else                                block_x = anchor_x;

	int block_y;					/* fb y of the block's top */
	if      (te->valign == VALIGN_MIDDLE) block_y = anchor_y - block_h / 2;
	else if (te->valign == VALIGN_BOTTOM) block_y = anchor_y - block_h;
	else                                  block_y = anchor_y;

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
			                   apply_opacity(te->shadow_color, op),
			                   0, 0,
			                   (int)buf->width, (int)buf->height);
			free(sh);
		}
	}

	/* The text itself, sharp, on top. */
	composite_coverage(buf, cov, cov_w, cov_h, origin_x, origin_y,
	                   apply_opacity(te->color, op),
	                   0, 0, (int)buf->width, (int)buf->height);

	free(cov);
}

/* ========================================================================
 * Console / Scrolling Log Area Rendering
 * ======================================================================== */

/*
 * Render a console_t element: optional background fill, then the most
 * recent lines anchored to the bottom of the console box (newer lines
 * appear at the bottom; older lines scroll off the top when the buffer
 * fills up). Text is clipped to the console rectangle.
 */
void draw_console_element(drm_buffer_t *buf, console_t *con) {
	if (!con->active || con->w <= 0 || con->h <= 0 || con->line_count == 0)
		return;

	float op = con->opacity;
	if (op <= 0.0f)
		return;

	/* Resolve negative-centre shorthand (same convention as other elements). */
	int cx = (con->x < 0) ? ((int)buf->width  - con->w) / 2 : con->x;
	int cy = (con->y < 0) ? ((int)buf->height - con->h) / 2 : con->y;
	/* Temporarily patch the con fields so the rest of the function uses the
	 * resolved coordinates without changing the stored state. */
	int saved_x = con->x, saved_y = con->y;
	con->x = cx; con->y = cy;

	font_t *f = get_font(con->font_slot);
	if (!f)
		return;

	stbtt_fontinfo *info  = f->info;
	float           scale = f->scale;
	if (con->font_size > 0.0f)
		scale = stbtt_ScaleForPixelHeight(info, con->font_size);

	init_srgb_lut();

	int ascent, descent, line_gap;
	stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
	int asc_px   = (int)(ascent  * scale);
	int desc_px  = (int)(descent * scale);   /* negative */
	int th       = asc_px - desc_px;
	int line_adv = th + (int)(line_gap * scale);
	if (line_adv < 1)
		line_adv = 1;

	/* Optional background. */
	if ((con->bg_color >> 24) > 0)
		draw_filled_rect(buf, con->x, con->y, con->w, con->h,
		                 apply_opacity(con->bg_color, op));

	int pad      = con->padding;
	int inner_h  = con->h - 2 * pad;
	if (inner_h <= 0)
		return;

	int max_vis = inner_h / line_adv;
	if (max_vis <= 0)
		return;

	/* How many lines to show: the most recent min(count, max_vis). */
	int vis = con->line_count < max_vis ? con->line_count : max_vis;

	/* Clip rectangle for compositing. */
	int clip_x0 = con->x;
	int clip_y0 = con->y;
	int clip_x1 = con->x + con->w;
	int clip_y1 = con->y + con->h;

	/* "Grow from bottom": when fewer lines than max_vis, push them down
	 * so that the newest line always sits at the bottom of the box. */
	int top_y = con->y + pad + (max_vis - vis) * line_adv;

	/* Iterate oldest-to-newest (top-to-bottom in display). */
	int max_lines  = con->max_lines;
	int oldest_idx = (con->head - vis + max_lines * 2) % max_lines;

	uint32_t color = apply_opacity(con->color, op);

	for (int i = 0; i < vis; i++) {
		int         idx  = (oldest_idx + i) % max_lines;
		const char *text = con->lines[idx];
		if (!text[0])
			continue;

		int line_y = top_y + i * line_adv;   /* top pixel of the line */

		if (line_y >= clip_y1)
			break;
		if (line_y + th < clip_y0)
			continue;

		int   tlen  = (int)strlen(text);
		float lw    = measure_line_scaled(info, scale, text, tlen);
		int   cov_w = (int)(lw + 0.5f) + 4;   /* +4 for glyph overhang */
		int   cov_h = th + 4;
		if (cov_w <= 0 || cov_h <= 0)
			continue;

		uint8_t *cov = calloc((size_t)cov_w * cov_h, 1);
		if (!cov)
			continue;

		rasterize_line(info, scale, cov, cov_w, cov_h,
		               2.0f, asc_px + 2, text, tlen);

		composite_coverage(buf, cov, cov_w, cov_h,
		                   con->x + pad - 2, line_y - 2,
		                   color,
		                   clip_x0, clip_y0, clip_x1, clip_y1);
		free(cov);
	}

	con->x = saved_x;
	con->y = saved_y;
}
