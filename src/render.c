/*
 * render.c - Frame rendering and drawing primitives.
 *
 * Rounded geometry (progress bars and RECT elements) is rendered with a
 * signed-distance field; the same SDF toolkit also draws the boot
 * spinner. Every element carries an `opacity` multiplier, applied by
 * scaling colour alpha (solid shapes) or sample alpha (images) so
 * animations fade cleanly.
 *
 * Images are resampled with a quality kernel (Lanczos-3 / Mitchell /
 * bilinear), or copied 1:1 / nearest on the fast path.
 */

#include "splash.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================
 * Basic Rectangle Primitive
 * ======================================================================== */

/*
 * Premultiply a straight-alpha ARGB colour so that blend_pixel, which
 * operates on premultiplied destinations, reads a consistent value from
 * the buffer.  The identity for a=0 and a=255 avoids any computation on
 * the common cases.
 */
static inline uint32_t premultiply(uint32_t color) {
	uint8_t a = color >> 24;
	if (a == 0 || a == 255)
		return color;
	uint8_t r = (uint8_t)((uint32_t)((color >> 16) & 0xFF) * a / 255);
	uint8_t g = (uint8_t)((uint32_t)((color >>  8) & 0xFF) * a / 255);
	uint8_t b = (uint8_t)((uint32_t)( color        & 0xFF) * a / 255);
	return argb(a, r, g, b);
}

/* Axis-aligned fill - used to clear the framebuffer each frame. */
void draw_filled_rect(drm_buffer_t *buf, int x, int y, int w, int h,
                      uint32_t color) {
	/* Write in premultiplied form so blend_pixel reads a consistent
	 * representation regardless of the colour's alpha value. */
	color = premultiply(color);
	int x0 = clamp(x,     0, (int)buf->width);
	int y0 = clamp(y,     0, (int)buf->height);
	int x1 = clamp(x + w, 0, (int)buf->width);
	int y1 = clamp(y + h, 0, (int)buf->height);

	for (int row = y0; row < y1; row++) {
		uint32_t *line = (uint32_t *)(buf->map + row * buf->pitch + x0 * 4);
		for (int col = x0; col < x1; col++)
			*line++ = color;
	}
}

/* ========================================================================
 * Paint - solid colour or 2-stop linear gradient
 * ======================================================================== */

/* Linear interpolation between two ARGB colours (straight alpha, sRGB). */
static inline uint32_t lerp_color(uint32_t a, uint32_t b, float t) {
	if (t <= 0.0f)
		return a;
	if (t >= 1.0f)
		return b;

	int aa = (a >> 24) & 0xFF, ar = (a >> 16) & 0xFF,
	    ag = (a >>  8) & 0xFF, ab =  a        & 0xFF;
	int ba = (b >> 24) & 0xFF, br = (b >> 16) & 0xFF,
	    bg = (b >>  8) & 0xFF, bb =  b        & 0xFF;

	int ca = aa + (int)((ba - aa) * t + 0.5f);
	int cr = ar + (int)((br - ar) * t + 0.5f);
	int cg = ag + (int)((bg - ag) * t + 0.5f);
	int cb = ab + (int)((bb - ab) * t + 0.5f);
	return argb((uint8_t)ca, (uint8_t)cr, (uint8_t)cg, (uint8_t)cb);
}

/* Sample a paint at parametric position (u, v), both in [0, 1]. */
static inline uint32_t paint_at(const paint_t *p, float u, float v) {
	if (p->gradient == GRAD_NONE)
		return p->color0;

	float t;
	if (p->gradient == GRAD_HORIZONTAL)
		t = u;
	else if (p->gradient == GRAD_DIAGONAL)
		t = (u + v) * 0.5f;
	else
		t = v;					/* GRAD_VERTICAL */
	return lerp_color(p->color0, p->color1, t);
}

/* ========================================================================
 * Anti-aliased Rounded Rectangle (signed distance field)
 * ======================================================================== */

/*
 * Signed distance from (px, py) to a rounded rectangle centred at
 * (cx, cy) with half-extents (hx, hy) and corner radius r. Negative
 * inside, positive outside, zero exactly on the edge.
 */
static inline float sdf_round_rect(float px, float py,
                                   float cx, float cy,
                                   float hx, float hy, float r) {
	float qx = fabsf(px - cx) - hx + r;
	float qy = fabsf(py - cy) - hy + r;
	float ox = qx > 0.0f ? qx : 0.0f;
	float oy = qy > 0.0f ? qy : 0.0f;

	float outside;
	if (ox > 0.0f && oy > 0.0f)
		outside = sqrtf(ox * ox + oy * oy);	/* true corner region */
	else
		outside = ox + oy;			/* straight edge: one term is 0 */

	float inside = fminf(fmaxf(qx, qy), 0.0f);
	return outside + inside - r;
}

/* Blend `coverage` (0..1) worth of `color` onto a single pixel. */
static inline void blend_coverage(uint32_t *dst, uint32_t color,
                                  float coverage) {
	if (coverage <= 0.0f)
		return;
	if (coverage > 1.0f)
		coverage = 1.0f;

	uint32_t a = (uint32_t)((float)(color >> 24) * coverage + 0.5f);
	if (a == 0)
		return;
	blend_pixel(dst, (color & 0x00FFFFFFu) | (a << 24));
}

/* Clamp a corner radius so it never exceeds half of the shorter side. */
static float clamp_radius(float w, float h, float r) {
	if (r < 0.0f)
		r = 0.0f;
	float maxr = (w < h ? w : h) * 0.5f;
	if (r > maxr)
		r = maxr;
	return r;
}

/* Filled anti-aliased rounded rectangle, solid colour or gradient. */
void draw_round_rect(drm_buffer_t *buf, float x, float y, float w, float h,
                     float radius, const paint_t *paint) {
	if (w <= 0.0f || h <= 0.0f)
		return;
	radius = clamp_radius(w, h, radius);

	float hx = w * 0.5f, hy = h * 0.5f;
	float cx = x + hx,   cy = y + hy;
	float inv_w = 1.0f / w, inv_h = 1.0f / h;
	int grad = paint->gradient;

	int x0 = clamp((int)floorf(x),     0, (int)buf->width);
	int y0 = clamp((int)floorf(y),     0, (int)buf->height);
	int x1 = clamp((int)ceilf(x + w),  0, (int)buf->width);
	int y1 = clamp((int)ceilf(y + h),  0, (int)buf->height);

	for (int py = y0; py < y1; py++) {
		uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
		float fy = (float)py + 0.5f;
		float v  = (fy - y) * inv_h;
		for (int px = x0; px < x1; px++, line++) {
			float fx  = (float)px + 0.5f;
			float d   = sdf_round_rect(fx, fy, cx, cy, hx, hy, radius);
			float cov = 0.5f - d;
			if (cov <= 0.0f)
				continue;
			uint32_t c = grad ? paint_at(paint, (fx - x) * inv_w, v)
			                  : paint->color0;
			blend_coverage(line, c, cov);
		}
	}
}

/* Anti-aliased rounded-rectangle outline, grown inward from the edge. */
void draw_round_rect_outline(drm_buffer_t *buf, float x, float y,
                             float w, float h, float radius,
                             float border_width, uint32_t color) {
	if (w <= 0.0f || h <= 0.0f || border_width <= 0.0f)
		return;
	radius = clamp_radius(w, h, radius);

	float maxbw = (w < h ? w : h) * 0.5f;
	if (border_width > maxbw)
		border_width = maxbw;

	float hx = w * 0.5f, hy = h * 0.5f;
	float cx = x + hx,   cy = y + hy;
	float half_bw = border_width * 0.5f;

	int x0 = clamp((int)floorf(x),     0, (int)buf->width);
	int y0 = clamp((int)floorf(y),     0, (int)buf->height);
	int x1 = clamp((int)ceilf(x + w),  0, (int)buf->width);
	int y1 = clamp((int)ceilf(y + h),  0, (int)buf->height);

	for (int py = y0; py < y1; py++) {
		uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
		float fy = (float)py + 0.5f;
		for (int px = x0; px < x1; px++, line++) {
			float d = sdf_round_rect((float)px + 0.5f, fy,
			                         cx, cy, hx, hy, radius);
			/* Distance to the centre of the stroke band. */
			float band = fabsf(d + half_bw) - half_bw;
			blend_coverage(line, color, 0.5f - band);
		}
	}
}

/* Filled rounded rectangle revealed left-to-right up to `fill_w`. */
void draw_round_rect_progress(drm_buffer_t *buf, float x, float y,
                              float w, float h, float radius,
                              float fill_w, const paint_t *paint) {
	if (w <= 0.0f || h <= 0.0f || fill_w <= 0.0f)
		return;
	radius = clamp_radius(w, h, radius);

	if (fill_w > w)
		fill_w = w;
	int full = (fill_w >= w - 0.01f);

	float hx = w * 0.5f, hy = h * 0.5f;
	float cx = x + hx,   cy = y + hy;
	float inv_w = 1.0f / w, inv_h = 1.0f / h;
	int grad = paint->gradient;
	float clip_edge = x + fill_w;

	int x0 = clamp((int)floorf(x),                          0, (int)buf->width);
	int y0 = clamp((int)floorf(y),                          0, (int)buf->height);
	int x1 = clamp((int)ceilf(x + (full ? w : fill_w)) + 1, 0, (int)buf->width);
	int y1 = clamp((int)ceilf(y + h),                       0, (int)buf->height);

	for (int py = y0; py < y1; py++) {
		uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
		float fy = (float)py + 0.5f;
		float v  = (fy - y) * inv_h;
		for (int px = x0; px < x1; px++, line++) {
			float fx  = (float)px + 0.5f;
			float d   = sdf_round_rect(fx, fy, cx, cy, hx, hy, radius);
			float cov = 0.5f - d;
			if (cov <= 0.0f)
				continue;
			if (cov > 1.0f)
				cov = 1.0f;
			if (!full) {
				/* Soft 1px cut at the progress edge. */
				float hmask = clip_edge - (float)px;
				if (hmask <= 0.0f)
					continue;
				if (hmask < 1.0f)
					cov *= hmask;
			}
			uint32_t c = grad ? paint_at(paint, (fx - x) * inv_w, v)
			                  : paint->color0;
			blend_coverage(line, c, cov);
		}
	}
}

/* Soft drop shadow shaped like a rounded rectangle. */
void draw_round_rect_shadow(drm_buffer_t *buf, float x, float y,
                            float w, float h, float radius,
                            float blur, uint32_t color) {
	if (w <= 0.0f || h <= 0.0f)
		return;
	radius = clamp_radius(w, h, radius);
	if (blur < 0.5f)
		blur = 0.5f;

	float hx = w * 0.5f, hy = h * 0.5f;
	float cx = x + hx,   cy = y + hy;
	float inv = 1.0f / (2.0f * blur);

	int x0 = clamp((int)floorf(x - blur),     0, (int)buf->width);
	int y0 = clamp((int)floorf(y - blur),     0, (int)buf->height);
	int x1 = clamp((int)ceilf(x + w + blur),  0, (int)buf->width);
	int y1 = clamp((int)ceilf(y + h + blur),  0, (int)buf->height);

	for (int py = y0; py < y1; py++) {
		uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
		float fy = (float)py + 0.5f;
		for (int px = x0; px < x1; px++, line++) {
			float d   = sdf_round_rect((float)px + 0.5f, fy,
			                           cx, cy, hx, hy, radius);
			float cov = (blur - d) * inv;
			if (cov <= 0.0f)
				continue;
			if (cov > 1.0f)
				cov = 1.0f;
			/* Smoothstep falloff for a softer shadow edge. */
			cov = cov * cov * (3.0f - 2.0f * cov);
			blend_coverage(line, color, cov);
		}
	}
}

/* ========================================================================
 * Boot Spinner
 *
 * A ring of tapered "spokes". Each spoke is a capsule (a fully rounded
 * rectangle) drawn with the SDF toolkit by rotating the query point into
 * the spoke's local frame. Brightness ramps around the ring; the whole
 * ring rotates continuously, so the bright spoke sweeps around like the
 * Apple boot indicator.
 * ======================================================================== */

void draw_spinner(drm_buffer_t *buf, spinner_t *sp, uint64_t now) {
	if (!sp->active || sp->opacity <= 0.0f || sp->radius <= 0)
		return;

	int N = sp->spokes > 0 ? sp->spokes : 12;
	if (N < 3)
		N = 3;
	if (N > 64)
		N = 64;

	float cx = (sp->x < 0) ? (float)buf->width  * 0.5f : (float)sp->x;
	float cy = (sp->y < 0) ? (float)buf->height * 0.5f : (float)sp->y;

	float r_out = (float)sp->radius;
	float r_in  = r_out * 0.45f;
	float W     = r_out * 0.24f;			/* spoke width */
	if (W < 2.0f)
		W = 2.0f;
	float half_len = (r_out - r_in) * 0.5f;
	float half_w   = W * 0.5f;
	float mid_r    = (r_out + r_in) * 0.5f;

	uint32_t period = sp->period_ms > 0 ? sp->period_ms : 900;
	float rot = 2.0f * (float)M_PI *
	            ((float)(uint32_t)((now - sp->start_ms) % period) /
	             (float)period);

	uint8_t base_a = sp->color >> 24;
	float   op     = sp->opacity;
	float   step   = 2.0f * (float)M_PI / (float)N;

	for (int i = 0; i < N; i++) {
		float ang = rot + (float)i * step;
		float ca  = cosf(ang), sa = sinf(ang);

		/* Brightness ramps around the ring; the bright spoke rotates. */
		float bright = 0.15f + 0.85f * ((float)i / (float)(N - 1));
		uint32_t a = (uint32_t)((float)base_a * bright * op + 0.5f);
		if (a == 0)
			continue;
		uint32_t col = (sp->color & 0x00FFFFFFu) | (a << 24);

		float scx = cx + ca * mid_r;		/* spoke centre */
		float scy = cy + sa * mid_r;

		/* Axis-aligned bounding box of this rotated capsule: the exact rotated
		 * half-extents (+1px for the AA edge) instead of the loose 45° worst
		 * case half_len+half_w, so far fewer pixels are SDF-tested per spoke. */
		float aca = fabsf(ca), asa = fabsf(sa);
		float ex = half_len * aca + half_w * asa + 1.0f;
		float ey = half_len * asa + half_w * aca + 1.0f;
		int x0 = clamp((int)floorf(scx - ex), 0, (int)buf->width);
		int y0 = clamp((int)floorf(scy - ey), 0, (int)buf->height);
		int x1 = clamp((int)ceilf(scx + ex),  0, (int)buf->width);
		int y1 = clamp((int)ceilf(scy + ey),  0, (int)buf->height);

		for (int py = y0; py < y1; py++) {
			uint32_t *line =
				(uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
			float dy = (float)py + 0.5f - scy;
			for (int px = x0; px < x1; px++, line++) {
				float dx = (float)px + 0.5f - scx;
				/* Rotate (dx, dy) by -ang into the spoke's frame. */
				float lx =  dx * ca + dy * sa;
				float ly = -dx * sa + dy * ca;
				float d = sdf_round_rect(lx, ly, 0.0f, 0.0f,
				                         half_len, half_w, half_w);
				blend_coverage(line, col, 0.5f - d);
			}
		}
	}
}

/* ========================================================================
 * Image Resampling
 * ======================================================================== */

/* Reconstruction kernel weight at offset x, for the chosen filter. */
static float kernel_weight(float x, int filter) {
	x = fabsf(x);
	switch (filter) {
	case IMG_BILINEAR:
		return x < 1.0f ? 1.0f - x : 0.0f;

	case IMG_BICUBIC: {				/* Mitchell-Netravali, B=C=1/3 */
		float x2 = x * x, x3 = x2 * x;
		if (x < 1.0f)
			return (7.0f / 6.0f) * x3 - 2.0f * x2 + 8.0f / 9.0f;
		if (x < 2.0f)
			return -(7.0f / 18.0f) * x3 + 2.0f * x2
			       - (10.0f / 3.0f) * x + 16.0f / 9.0f;
		return 0.0f;
	}

	case IMG_LANCZOS:				/* Lanczos-3 */
	default:
		if (x >= 3.0f)
			return 0.0f;
		if (x < 1e-6f)
			return 1.0f;
		{
			float px = (float)M_PI * x;
			return (sinf(px) / px) *
			       (sinf(px / 3.0f) / (px / 3.0f));
		}
	}
}

/* Kernel support radius in source pixels. */
static float kernel_radius(int filter) {
	switch (filter) {
	case IMG_BILINEAR:
		return 1.0f;
	case IMG_BICUBIC:
		return 2.0f;
	case IMG_LANCZOS:
	default:
		return 3.0f;
	}
}

/* Bilinear sample of an RGBA source at (fx,fy), returned as straight ARGB. */
static uint32_t sample_bilinear(const uint8_t *rgba, int iw, int ih,
                                float fx, float fy) {
	fx -= 0.5f;
	fy -= 0.5f;
	int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
	int x1 = x0 + 1, y1 = y0 + 1;
	float tx = fx - (float)x0, ty = fy - (float)y0;
	x0 = clamp(x0, 0, iw - 1);
	x1 = clamp(x1, 0, iw - 1);
	y0 = clamp(y0, 0, ih - 1);
	y1 = clamp(y1, 0, ih - 1);
	const uint8_t *p00 = rgba + ((size_t)y0 * iw + x0) * 4;
	const uint8_t *p10 = rgba + ((size_t)y0 * iw + x1) * 4;
	const uint8_t *p01 = rgba + ((size_t)y1 * iw + x0) * 4;
	const uint8_t *p11 = rgba + ((size_t)y1 * iw + x1) * 4;
	float w00 = (1 - tx) * (1 - ty), w10 = tx * (1 - ty);
	float w01 = (1 - tx) * ty,       w11 = tx * ty;
	uint8_t o[4];
	for (int c = 0; c < 4; c++)
		o[c] = (uint8_t)(p00[c]*w00 + p10[c]*w10 + p01[c]*w01 + p11[c]*w11 + 0.5f);
	return argb(o[3], o[0], o[1], o[2]);
}

/* Coverage (0..1, 1px AA) of a point in element space [0,w]x[0,h] inside a
 * rounded rectangle with corner radius r. Standard rounded-box SDF. */
static float rrect_coverage(float px, float py, float w, float h, float r) {
	float hw = w * 0.5f, hh = h * 0.5f;
	if (r > hw) r = hw;
	if (r > hh) r = hh;
	float qx = fabsf(px - hw) - (hw - r);
	float qy = fabsf(py - hh) - (hh - r);
	float ox = qx > 0.0f ? qx : 0.0f, oy = qy > 0.0f ? qy : 0.0f;
	float d  = sqrtf(ox * ox + oy * oy) + fminf(fmaxf(qx, qy), 0.0f) - r;
	return fclamp(0.5f - d, 0.0f, 1.0f);
}

/*
 * Draw an image into (dx,dy,dw,dh) with optional rotation about its centre, a
 * rounded-corner clip, and a multiply tint, via inverse mapping + bilinear
 * sampling. Used for overlays that set angle/radius/tint; the plain case still
 * goes through draw_image() (which has the nicer resampling kernels).
 */
static void draw_image_ex(drm_buffer_t *buf, int dx, int dy, int dw, int dh,
                          const uint8_t *rgba, int iw, int ih,
                          float opacity, float angle_deg, int radius,
                          uint32_t tint) {
	if (dw <= 0 || dh <= 0 || iw <= 0 || ih <= 0 || opacity <= 0.0f)
		return;

	float cx = (float)dx + (float)dw * 0.5f;
	float cy = (float)dy + (float)dh * 0.5f;
	float hw = (float)dw * 0.5f, hh = (float)dh * 0.5f;
	float ang = angle_deg * (float)M_PI / 180.0f;
	float ca = cosf(ang), sa = sinf(ang);

	float ex = fabsf(hw * ca) + fabsf(hh * sa);   /* rotated-rect half-extents */
	float ey = fabsf(hw * sa) + fabsf(hh * ca);
	int x0 = clamp((int)floorf(cx - ex) - 1, 0, (int)buf->width);
	int x1 = clamp((int)ceilf (cx + ex) + 1, 0, (int)buf->width);
	int y0 = clamp((int)floorf(cy - ey) - 1, 0, (int)buf->height);
	int y1 = clamp((int)ceilf (cy + ey) + 1, 0, (int)buf->height);

	int   has_tint = (tint >> 24) > 0;
	float tstr = has_tint ? (float)(tint >> 24) / 255.0f : 0.0f;
	int   tr = (tint >> 16) & 0xFF, tg = (tint >> 8) & 0xFF, tb = tint & 0xFF;

	for (int py = y0; py < y1; py++) {
		uint32_t *dst = (uint32_t *)(buf->map + (size_t)py * buf->pitch) + x0;
		for (int px = x0; px < x1; px++, dst++) {
			float rx = ((float)px + 0.5f) - cx;
			float ry = ((float)py + 0.5f) - cy;
			float ux =  rx * ca + ry * sa + hw;   /* inverse-rotate -> element */
			float uy = -rx * sa + ry * ca + hh;
			if (ux < 0.0f || ux >= (float)dw || uy < 0.0f || uy >= (float)dh)
				continue;

			float cov = 1.0f;
			if (radius > 0) {
				cov = rrect_coverage(ux, uy, (float)dw, (float)dh, (float)radius);
				if (cov <= 0.0f)
					continue;
			}

			uint32_t s = sample_bilinear(rgba, iw, ih,
			                             ux / (float)dw * (float)iw,
			                             uy / (float)dh * (float)ih);
			uint8_t salpha = s >> 24;
			if (salpha == 0)
				continue;
			int sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
			if (has_tint) {
				sr += (int)(((float)(sr * tr / 255) - (float)sr) * tstr);
				sg += (int)(((float)(sg * tg / 255) - (float)sg) * tstr);
				sb += (int)(((float)(sb * tb / 255) - (float)sb) * tstr);
			}
			float a = (float)salpha * opacity * cov;
			uint32_t src = ((uint32_t)(a + 0.5f) << 24) | ((uint32_t)sr << 16) |
			               ((uint32_t)sg << 8) | (uint32_t)sb;
			blend_pixel(dst, src);
		}
	}
}

/*
 * Blit an RGBA image into the destination rectangle. The nearest filter,
 * and any exact 1:1 copy, take a fast path; otherwise the image is
 * resampled with the chosen kernel. `opacity` (0..1) scales the result.
 */
void draw_image(drm_buffer_t *buf, int x, int y, int w, int h,
                const uint8_t *rgba, int img_w, int img_h,
                int filter, float opacity) {
	if (!rgba || img_w <= 0 || img_h <= 0 || w <= 0 || h <= 0)
		return;
	if (opacity <= 0.0f)
		return;

	int x0 = clamp(x,     0, (int)buf->width);
	int y0 = clamp(y,     0, (int)buf->height);
	int x1 = clamp(x + w, 0, (int)buf->width);
	int y1 = clamp(y + h, 0, (int)buf->height);

	/* Fast path: nearest-neighbour, or any exact 1:1 blit. */
	if (filter == IMG_NEAREST || (w == img_w && h == img_h)) {
		float sx = (float)img_w / (float)w;
		float sy = (float)img_h / (float)h;
		for (int row = y0; row < y1; row++) {
			int syc = clamp((int)((row - y) * sy), 0, img_h - 1);
			const uint8_t *srow = rgba + syc * img_w * 4;
			uint32_t *dst =
				(uint32_t *)(buf->map + row * buf->pitch + x0 * 4);
			for (int col = x0; col < x1; col++) {
				int sxc = clamp((int)((col - x) * sx), 0, img_w - 1);
				const uint8_t *p = srow + sxc * 4;
				uint8_t a = p[3];
				if (opacity < 1.0f)
					a = (uint8_t)((float)a * opacity + 0.5f);
				blend_pixel(dst++, argb(a, p[0], p[1], p[2]));
			}
		}
		return;
	}

	float sx = (float)img_w / (float)w;
	float sy = (float)img_h / (float)h;
	/* When minifying, widen the kernel footprint to average more. */
	float supx = sx > 1.0f ? sx : 1.0f;
	float supy = sy > 1.0f ? sy : 1.0f;
	float base_r = kernel_radius(filter);
	float radx = base_r * supx, rady = base_r * supy;
	float inv_supx = 1.0f / supx, inv_supy = 1.0f / supy;

	for (int row = y0; row < y1; row++) {
		float cy = ((float)(row - y) + 0.5f) * sy - 0.5f;
		int iy0 = (int)floorf(cy - rady);
		int iy1 = (int)ceilf(cy + rady);
		uint32_t *dst =
			(uint32_t *)(buf->map + row * buf->pitch + x0 * 4);

		for (int col = x0; col < x1; col++, dst++) {
			float cx = ((float)(col - x) + 0.5f) * sx - 0.5f;
			int ix0 = (int)floorf(cx - radx);
			int ix1 = (int)ceilf(cx + radx);

			/* Premultiplied-alpha accumulation, then un-premultiply. */
			float ar = 0.0f, ag = 0.0f, ab = 0.0f;
			float aa = 0.0f, ws = 0.0f;

			for (int iy = iy0; iy <= iy1; iy++) {
				float wy = kernel_weight((iy - cy) * inv_supy, filter);
				if (wy == 0.0f)
					continue;
				const uint8_t *srow =
					rgba + clamp(iy, 0, img_h - 1) * img_w * 4;
				for (int ix = ix0; ix <= ix1; ix++) {
					float wx =
						kernel_weight((ix - cx) * inv_supx, filter);
					float wgt = wx * wy;
					if (wgt == 0.0f)
						continue;
					const uint8_t *p =
						srow + clamp(ix, 0, img_w - 1) * 4;
					float pa = p[3] * (1.0f / 255.0f);
					ar += p[0] * pa * wgt;
					ag += p[1] * pa * wgt;
					ab += p[2] * pa * wgt;
					aa += p[3] * wgt;
					ws += wgt;
				}
			}
			if (ws <= 0.0f || aa <= 0.0f)
				continue;

			float oa = aa / ws;
			if (opacity < 1.0f)
				oa *= opacity;
			if (oa < 0.5f)
				continue;
			float k = 255.0f / aa;
			int rr = clamp((int)(ar * k + 0.5f), 0, 255);
			int gg = clamp((int)(ag * k + 0.5f), 0, 255);
			int bb = clamp((int)(ab * k + 0.5f), 0, 255);
			int al = clamp((int)(oa + 0.5f),     0, 255);
			blend_pixel(dst, argb((uint8_t)al, (uint8_t)rr,
			                      (uint8_t)gg, (uint8_t)bb));
		}
	}
}

/*
 * Resample src (iw x ih) into the straight-RGBA buffer `out` (dw x dh) using
 * exactly draw_image()'s kernel and rounding at opacity 1 - pixels draw_image
 * would drop (resampled alpha < 0.5) are written fully transparent. This lets a
 * static background be resampled once and then blitted 1:1 with IMG_NEAREST
 * every frame (bit-identical to the live path) instead of re-resampled.
 */
static void resample_image_to(uint8_t *out, int dw, int dh,
                              const uint8_t *rgba, int iw, int ih, int filter) {
	float sx = (float)iw / (float)dw;
	float sy = (float)ih / (float)dh;
	float supx = sx > 1.0f ? sx : 1.0f;
	float supy = sy > 1.0f ? sy : 1.0f;
	float base_r = kernel_radius(filter);
	float radx = base_r * supx, rady = base_r * supy;
	float inv_supx = 1.0f / supx, inv_supy = 1.0f / supy;

	for (int row = 0; row < dh; row++) {
		float cy = ((float)row + 0.5f) * sy - 0.5f;
		int iy0 = (int)floorf(cy - rady);
		int iy1 = (int)ceilf(cy + rady);
		uint8_t *orow = out + (size_t)row * dw * 4;

		for (int col = 0; col < dw; col++) {
			float cx = ((float)col + 0.5f) * sx - 0.5f;
			int ix0 = (int)floorf(cx - radx);
			int ix1 = (int)ceilf(cx + radx);

			float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f, ws = 0.0f;
			for (int iy = iy0; iy <= iy1; iy++) {
				float wy = kernel_weight((iy - cy) * inv_supy, filter);
				if (wy == 0.0f)
					continue;
				const uint8_t *srow = rgba + clamp(iy, 0, ih - 1) * iw * 4;
				for (int ix = ix0; ix <= ix1; ix++) {
					float wx = kernel_weight((ix - cx) * inv_supx, filter);
					float wgt = wx * wy;
					if (wgt == 0.0f)
						continue;
					const uint8_t *p = srow + clamp(ix, 0, iw - 1) * 4;
					float pa = p[3] * (1.0f / 255.0f);
					ar += p[0] * pa * wgt;
					ag += p[1] * pa * wgt;
					ab += p[2] * pa * wgt;
					aa += p[3] * wgt;
					ws += wgt;
				}
			}
			uint8_t *o = orow + (size_t)col * 4;
			float oa = (ws > 0.0f && aa > 0.0f) ? aa / ws : 0.0f;
			if (oa < 0.5f) {			/* draw_image drops these */
				o[0] = o[1] = o[2] = o[3] = 0;
				continue;
			}
			float k = 255.0f / aa;
			o[0] = (uint8_t)clamp((int)(ar * k + 0.5f), 0, 255);
			o[1] = (uint8_t)clamp((int)(ag * k + 0.5f), 0, 255);
			o[2] = (uint8_t)clamp((int)(ab * k + 0.5f), 0, 255);
			o[3] = (uint8_t)clamp((int)(oa + 0.5f),     0, 255);
		}
	}
}

/* ========================================================================
 * Image Scaling
 * ======================================================================== */

/* Resolve the on-screen rectangle for a background image under `mode`. */
void calculate_scaled_rect(int buf_w, int buf_h, int img_w, int img_h,
                           int mode, float custom_scale,
                           int *out_x, int *out_y, int *out_w, int *out_h) {
	switch (mode) {
	case SCALE_NONE:
		*out_w = img_w;
		*out_h = img_h;
		*out_x = (buf_w - img_w) / 2;
		*out_y = (buf_h - img_h) / 2;
		break;

	case SCALE_STRETCH:
		*out_w = buf_w;
		*out_h = buf_h;
		*out_x = 0;
		*out_y = 0;
		break;

	case SCALE_CUSTOM:
		*out_w = (int)(img_w * custom_scale);
		*out_h = (int)(img_h * custom_scale);
		*out_x = (buf_w - *out_w) / 2;
		*out_y = (buf_h - *out_h) / 2;
		break;

	case SCALE_CONTAIN:
	default: {
		/* Largest fit with no cropping. */
		float scale = fminf((float)buf_w / img_w, (float)buf_h / img_h);
		*out_w = (int)(img_w * scale);
		*out_h = (int)(img_h * scale);
		*out_x = (buf_w - *out_w) / 2;
		*out_y = (buf_h - *out_h) / 2;
		break;
	}

	case SCALE_COVER: {
		/* Smallest cover; image may extend past the screen edges. */
		float scale = fmaxf((float)buf_w / img_w, (float)buf_h / img_h);
		*out_w = (int)(img_w * scale);
		*out_h = (int)(img_h * scale);
		*out_x = (buf_w - *out_w) / 2;
		*out_y = (buf_h - *out_h) / 2;
		break;
	}
	}
}

/* ========================================================================
 * Progress Bar Drawing
 * ======================================================================== */

/*
 * Indeterminate progress: a soft raised-cosine highlight band that sweeps
 * across the track. `phase` (0..1) is the sweep position; the band enters
 * from the left edge and exits past the right.
 */
void draw_round_rect_sweep(drm_buffer_t *buf, float x, float y,
                           float w, float h, float radius,
                           const paint_t *paint, float phase) {
	if (w <= 0.0f || h <= 0.0f)
		return;
	radius = clamp_radius(w, h, radius);

	float hx = w * 0.5f, hy = h * 0.5f;
	float cx = x + hx,   cy = y + hy;
	float inv_w = 1.0f / w, inv_h = 1.0f / h;
	int grad = paint->gradient;

	float band = w * 0.40f;				/* highlight band width */
	if (band < 8.0f)
		band = 8.0f;
	float half = band * 0.5f;
	float center = -half + phase * (w + band);	/* sweeps on then off */

	int x0 = clamp((int)floorf(x),     0, (int)buf->width);
	int y0 = clamp((int)floorf(y),     0, (int)buf->height);
	int x1 = clamp((int)ceilf(x + w),  0, (int)buf->width);
	int y1 = clamp((int)ceilf(y + h),  0, (int)buf->height);

	for (int py = y0; py < y1; py++) {
		uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
		float fy = (float)py + 0.5f;
		float v  = (fy - y) * inv_h;
		for (int px = x0; px < x1; px++, line++) {
			float fx  = (float)px + 0.5f;
			float d   = sdf_round_rect(fx, fy, cx, cy, hx, hy, radius);
			float cov = 0.5f - d;
			if (cov <= 0.0f)
				continue;
			if (cov > 1.0f)
				cov = 1.0f;

			float bd = (fx - center) / half;	/* -1..1 in the band */
			if (bd <= -1.0f || bd >= 1.0f)
				continue;
			float inten = 0.5f + 0.5f * cosf(bd * (float)M_PI);

			uint32_t c = grad ? paint_at(paint, (fx - x) * inv_w, v)
			                  : paint->color0;
			blend_coverage(line, c, cov * inten);
		}
	}
}

/* Draw a centred "NN%" label for `value` (0..1) at (cx, cy). Shared by the
 * progress-bar and arc renderers; each caller resolves its own font slot/size
 * and centre, so the rounding (+0.5) stays identical between the two. */
static void draw_centered_percent(drm_buffer_t *buf, float value,
                                  int cx, int cy, int font_slot,
                                  float font_size, uint32_t color,
                                  float opacity) {
	text_element_t te = {0};
	te.active    = 1;
	te.opacity   = opacity;
	/* Clamp before the float->int cast: an out-of-range client value
	 * (e.g. 1e30) would make (int)(value*100) undefined behaviour. */
	snprintf(te.text, sizeof(te.text), "%d%%",
	         (int)(fclamp(value, 0.0f, 1.0f) * 100.0f + 0.5f));
	te.x         = cx;
	te.y         = cy;
	te.align     = ALIGN_CENTER;
	te.valign    = VALIGN_MIDDLE;
	te.color     = color;
	te.font_slot = font_slot;
	te.font_size = font_size;
	draw_text_element(buf, &te);
}

void draw_progress_bar(drm_buffer_t *buf, progress_bar_t *pb) {
	float op = pb->opacity;
	if (op <= 0.0f)
		return;

	/* Resolve the top-left corner from the anchor + alignment. A negative
	 * x or y anchors to the screen centre on that axis. */
	int anchor_x = resolve_anchor(pb->x, (int)buf->width);
	int anchor_y = resolve_anchor(pb->y, (int)buf->height);
	int x = anchor_x, y = anchor_y;
	if      (pb->align == ALIGN_CENTER) x -= pb->w / 2;
	else if (pb->align == ALIGN_RIGHT)  x -= pb->w;
	if      (pb->valign == VALIGN_MIDDLE) y -= pb->h / 2;
	else if (pb->valign == VALIGN_BOTTOM) y -= pb->h;

	float fx = (float)x,     fy = (float)y;
	float fw = (float)pb->w, fh = (float)pb->h;
	float r  = (float)pb->radius;

	float bw = (pb->border_width <= 0) ? 0.0f : (float)pb->border_width;

	/* The border is inset and the progress fill sits inside it. On a very
	 * thin bar there is no room for both: a border wider than half the bar
	 * collapses the fill to nothing, so the bar renders as just its (often
	 * invisible) track and appears to draw nothing at all. Cap the border so
	 * the fill always keeps at least 1px on the shorter axis. A 1px-tall bar
	 * thus drops the border entirely and shows as a clean line. */
	int shorter = pb->w < pb->h ? pb->w : pb->h;
	float max_bw = (float)((shorter - 1) / 2);
	if (max_bw < 0.0f)
		max_bw = 0.0f;
	if (bw > max_bw)
		bw = max_bw;

	/* 0. Soft drop shadow of the whole bar. */
	if (pb->shadow) {
		draw_round_rect_shadow(buf, fx + pb->shadow_dx, fy + pb->shadow_dy,
		                       fw, fh, r, (float)pb->shadow_blur,
		                       apply_opacity(pb->shadow_color, op));
	}

	/* 1. Background track. */
	paint_t track = { apply_opacity(pb->bg_color, op),
	                  apply_opacity(pb->bg_color, op), GRAD_NONE };
	draw_round_rect(buf, fx, fy, fw, fh, r, &track);

	/* 2. Progress fill - inset inside the border. */
	float in_x = fx + bw;
	float in_y = fy + bw;
	float in_w = fw - 2.0f * bw;
	float in_h = fh - 2.0f * bw;
	float in_r = r - bw;
	if (in_r < 0.0f)
		in_r = 0.0f;

	if (pb->indeterminate) {
		/* Sweeping highlight band - no measurable value. */
		if (in_w > 0.0f && in_h > 0.0f) {
			uint32_t period = pb->indet_period_ms > 0
			                  ? pb->indet_period_ms : 1100;
			float phase =
				(float)(uint32_t)((now_ms() - pb->indet_start_ms)
				                  % period) / (float)period;
			paint_t fill = { apply_opacity(pb->bar_color,  op),
			                 apply_opacity(pb->bar_color2, op),
			                 pb->bar_gradient };
			draw_round_rect_sweep(buf, in_x, in_y, in_w, in_h,
			                      in_r, &fill, phase);
		}
	} else {
		float value = fclamp(pb->value, 0.0f, 1.0f);
		if (value > 0.0f && in_w > 0.0f && in_h > 0.0f) {
			paint_t fill = { apply_opacity(pb->bar_color,  op),
			                 apply_opacity(pb->bar_color2, op),
			                 pb->bar_gradient };
			draw_round_rect_progress(buf, in_x, in_y, in_w, in_h,
			                         in_r, in_w * value, &fill);
		}
	}

	/* 3. Border last, on top of the track and fill. */
	if (bw > 0.0f) {
		draw_round_rect_outline(buf, fx, fy, fw, fh, r, bw,
		                        apply_opacity(pb->border_color, op));
	}

	/* 4. Percentage text (only meaningful for a determinate bar). */
	if (!pb->indeterminate && pb->show_percent && pb->value > 0.0f) {
		draw_centered_percent(buf, pb->value,
		                      x + pb->w / 2, y + pb->h / 2,
		                      pb->font_slot > 0 ? pb->font_slot : 0,
		                      pb->font_size > 0 ? pb->font_size : 0,
		                      pb->text_color, op);
	}
}

/* ========================================================================
 * Rectangle Element Drawing
 * ======================================================================== */

void draw_rect_element(drm_buffer_t *buf, rect_element_t *re) {
	if (re->w <= 0 || re->h <= 0)
		return;
	float op = re->opacity;
	if (op <= 0.0f)
		return;

	/* Resolve the top-left corner from the anchor + alignment. A negative
	 * x or y anchors to the screen centre on that axis. */
	int anchor_x = resolve_anchor(re->x, (int)buf->width);
	int anchor_y = resolve_anchor(re->y, (int)buf->height);
	int rx = anchor_x, ry = anchor_y;
	if      (re->align == ALIGN_CENTER) rx -= re->w / 2;
	else if (re->align == ALIGN_RIGHT)  rx -= re->w;
	if      (re->valign == VALIGN_MIDDLE) ry -= re->h / 2;
	else if (re->valign == VALIGN_BOTTOM) ry -= re->h;

	float fx = (float)rx, fy = (float)ry;
	float fw = (float)re->w, fh = (float)re->h;
	float r  = (float)re->radius;

	/* 0. Soft drop shadow. */
	if (re->shadow) {
		draw_round_rect_shadow(buf, fx + re->shadow_dx, fy + re->shadow_dy,
		                       fw, fh, r, (float)re->shadow_blur,
		                       apply_opacity(re->shadow_color, op));
	}

	/* A rect is filled unless it is explicitly outline-only. */
	int do_fill = re->fill || re->border_width <= 0;

	if (do_fill) {
		paint_t paint = { apply_opacity(re->color,      op),
		                  apply_opacity(re->grad_color, op),
		                  re->grad_dir };
		draw_round_rect(buf, fx, fy, fw, fh, r, &paint);
	}

	if (re->border_width > 0)
		draw_round_rect_outline(buf, fx, fy, fw, fh, r,
		                        (float)re->border_width,
		                        apply_opacity(re->border_color, op));
}

/* ========================================================================
 * Arc Progress Bar
 * ======================================================================== */

static inline float deg2rad(float d) {
	return d * (float)(M_PI / 180.0);
}

/*
 * Per-pixel coverage of an arc segment:
 *   ring_alpha  — coverage from the ring SDF (inner/outer edge AA)
 *   dist        — pixel distance from centre (for angular AA in pixel units)
 *   ang_offset  — angle of this pixel from the arc's start, in radians [0, 2π)
 *   arc_len     — arc length in radians
 *
 * Returns 0 if the pixel is outside the arc, otherwise ring_alpha multiplied
 * by the soft edge factors at both angular boundaries (1-pixel AA).
 */
static float arc_pixel_alpha(float ring_alpha, float dist,
                             float ang_offset, float arc_len) {
	if (ring_alpha <= 0.0f || ang_offset < 0.0f || ang_offset > arc_len)
		return 0.0f;
	if (arc_len >= 2.0f * (float)M_PI - 0.001f)
		return ring_alpha; /* full circle: no angular edges */
	float da = dist * ang_offset;
	float db = dist * (arc_len - ang_offset);
	float ea = fclamp(da + 0.5f, 0.0f, 1.0f);
	float eb = fclamp(db + 0.5f, 0.0f, 1.0f);
	return ring_alpha * ea * eb;
}

/*
 * Filled or outlined ellipse / circle with anti-aliased edges. The boundary is
 * (dx/rx)^2 + (dy/ry)^2 = 1; we approximate the signed pixel distance to it for
 * a 1px AA band (exact for circles, close enough for ellipses). thickness > 0
 * draws an outline ring of that width just inside the boundary; a negative
 * centre on an axis means "screen centre".
 */
void draw_ellipse(drm_buffer_t *buf, ellipse_t *e) {
	if (!e->active || e->opacity <= 0.0f)
		return;

	float rx = (float)e->rx;
	float ry = (float)(e->ry > 0 ? e->ry : e->rx);
	if (rx < 0.5f || ry < 0.5f)
		return;

	int   icx = resolve_anchor(e->cx, (int)buf->width);
	int   icy = resolve_anchor(e->cy, (int)buf->height);
	float cx  = (float)icx + 0.5f;
	float cy  = (float)icy + 0.5f;
	float t   = (float)e->thickness;
	int   filled = (t <= 0.0f);

	int x0 = clamp((int)floorf(cx - rx) - 1, 0, (int)buf->width);
	int x1 = clamp((int)ceilf (cx + rx) + 1, 0, (int)buf->width);
	int y0 = clamp((int)floorf(cy - ry) - 1, 0, (int)buf->height);
	int y1 = clamp((int)ceilf (cy + ry) + 1, 0, (int)buf->height);

	uint32_t base   = apply_opacity(e->color, e->opacity);
	uint8_t  base_a = base >> 24;
	if (base_a == 0)
		return;

	for (int py = y0; py < y1; py++) {
		uint32_t *dst = (uint32_t *)(buf->map + (size_t)py * buf->pitch) + x0;
		float dy = (float)py - cy;
		for (int px = x0; px < x1; px++, dst++) {
			float dx = (float)px - cx;
			float nx = dx / rx, ny = dy / ry;
			float f  = nx * nx + ny * ny;
			float sf = sqrtf(f);
			/* |grad(sqrt f)| converts the implicit value to a pixel distance. */
			float gx = nx / rx, gy = ny / ry;
			float gmag = sqrtf(gx * gx + gy * gy);
			if (gmag < 1e-6f)
				gmag = 1e-6f;
			float d = (sf - 1.0f) / gmag;          /* signed px dist, <0 inside */

			float cov;
			if (filled) {
				cov = fclamp(0.5f - d, 0.0f, 1.0f);
			} else {
				float outer = fclamp(0.5f - d, 0.0f, 1.0f);      /* inside boundary */
				float inner = fclamp(d + t + 0.5f, 0.0f, 1.0f);  /* outside inner edge */
				cov = outer * inner;
			}
			if (cov <= 0.0f)
				continue;

			uint32_t c = base;
			if (cov < 1.0f) {
				uint8_t a = (uint8_t)((float)base_a * cov + 0.5f);
				c = (base & 0x00FFFFFFu) | ((uint32_t)a << 24);
			}
			blend_pixel(dst, c);
		}
	}
}

/*
 * Anti-aliased thick line between two endpoints. cap 0 = flat/butt ends
 * (clipped to the segment, the natural look for a divider), cap 1 = round
 * (a capsule). Coverage comes from the perpendicular distance to the segment.
 */
void draw_line(drm_buffer_t *buf, line_t *l) {
	if (!l->active || l->opacity <= 0.0f)
		return;

	float ax = (float)l->x1 + 0.5f, ay = (float)l->y1 + 0.5f;
	float bx = (float)l->x2 + 0.5f, by = (float)l->y2 + 0.5f;
	float hw = (float)(l->thickness > 0 ? l->thickness : 2) * 0.5f;
	float abx = bx - ax, aby = by - ay;
	float len2 = abx * abx + aby * aby;
	float len  = sqrtf(len2);
	int   round = (l->cap == 1);

	/* A zero-length segment has no area with butt caps - the flat ends clip
	 * it away to nothing, so don't draw the faint stub it would otherwise
	 * leave. A round cap genuinely degenerates to a dot, so let it through. */
	if (!round && len2 < 1e-6f)
		return;

	int x0 = clamp((int)floorf(fminf(ax, bx) - hw) - 1, 0, (int)buf->width);
	int x1 = clamp((int)ceilf (fmaxf(ax, bx) + hw) + 1, 0, (int)buf->width);
	int y0 = clamp((int)floorf(fminf(ay, by) - hw) - 1, 0, (int)buf->height);
	int y1 = clamp((int)ceilf (fmaxf(ay, by) + hw) + 1, 0, (int)buf->height);

	uint32_t base   = apply_opacity(l->color, l->opacity);
	uint8_t  base_a = base >> 24;
	if (base_a == 0)
		return;

	for (int py = y0; py < y1; py++) {
		uint32_t *dst = (uint32_t *)(buf->map + (size_t)py * buf->pitch) + x0;
		float pay = ((float)py + 0.5f) - ay;
		for (int px = x0; px < x1; px++, dst++) {
			float pax = ((float)px + 0.5f) - ax;
			float t = (len2 > 1e-6f) ? (pax * abx + pay * aby) / len2 : 0.0f;

			float cov;
			if (round) {
				float tc = fclamp(t, 0.0f, 1.0f);
				float dx = pax - tc * abx, dy = pay - tc * aby;
				cov = fclamp(hw + 0.5f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
			} else {
				float dx = pax - t * abx, dy = pay - t * aby;   /* perpendicular */
				float cov_perp =
				    fclamp(hw + 0.5f - sqrtf(dx * dx + dy * dy), 0.0f, 1.0f);
				float along = t * len;                          /* px from end a */
				float e0 = fclamp(along + 0.5f, 0.0f, 1.0f);
				float e1 = fclamp((len - along) + 0.5f, 0.0f, 1.0f);
				cov = cov_perp * e0 * e1;
			}
			if (cov <= 0.0f)
				continue;

			uint32_t c = base;
			if (cov < 1.0f) {
				uint8_t a = (uint8_t)((float)base_a * cov + 0.5f);
				c = (base & 0x00FFFFFFu) | ((uint32_t)a << 24);
			}
			blend_pixel(dst, c);
		}
	}
}

/*
 * Step / boot-stage indicator: a centred row of `count` dots (or pills), the
 * first `current` drawn in color_done and the rest in color_todo. Composes the
 * ellipse and rounded-rect primitives rather than rasterising directly.
 */
void draw_stepper(drm_buffer_t *buf, stepper_t *s) {
	if (!s->active || s->opacity <= 0.0f || s->count <= 0)
		return;

	float op   = fclamp(s->opacity, 0.0f, 1.0f);
	int   dots = (s->style != 1);
	int   size = s->size > 0 ? s->size : 12;
	int   gap  = s->gap  > 0 ? s->gap  : size + 4;
	int   step_w = dots ? size * 2 : (s->length > 0 ? s->length : size * 3);
	int   step_h = dots ? size * 2 : size;
	int   n      = s->count;
	int   total_w = n * step_w + (n - 1) * gap;

	int ax = (s->x < 0) ? (int)buf->width  / 2 : s->x;
	int ay = (s->y < 0) ? (int)buf->height / 2 : s->y;
	int left = ax;
	if (s->align == ALIGN_CENTER)        left = ax - total_w / 2;
	else if (s->align == ALIGN_RIGHT)    left = ax - total_w;
	int top = ay;
	if (s->valign == VALIGN_MIDDLE)      top = ay - step_h / 2;
	else if (s->valign == VALIGN_BOTTOM) top = ay - step_h;

	for (int i = 0; i < n; i++) {
		int      done = (i < s->current);
		uint32_t col  = done ? s->color_done : s->color_todo;
		int      x    = left + i * (step_w + gap);

		if (dots) {
			ellipse_t e;
			memset(&e, 0, sizeof(e));
			e.active    = 1;
			e.opacity   = op;
			e.cx        = x + size;
			e.cy        = top + size;
			e.rx        = size;
			e.ry        = size;
			e.thickness = done ? 0 : s->thickness;
			e.color     = col;
			draw_ellipse(buf, &e);
		} else {
			float radius = (float)step_h / 2.0f;
			if (!done && s->thickness > 0) {
				draw_round_rect_outline(buf, (float)x, (float)top,
				                        (float)step_w, (float)step_h,
				                        radius, (float)s->thickness,
				                        apply_opacity(col, op));
			} else {
				paint_t p = { apply_opacity(col, op), 0, GRAD_NONE };
				draw_round_rect(buf, (float)x, (float)top,
				                (float)step_w, (float)step_h, radius, &p);
			}
		}
	}
}

/* Draw the current frame of a sprite animation, scaled like an overlay. */
void draw_sprite(drm_buffer_t *buf, sprite_t *sp) {
	if (!sp->active || sp->opacity <= 0.0f || sp->frame_count <= 0)
		return;

	int idx = sp->current;
	if (idx < 0)                 idx = 0;
	if (idx >= sp->frame_count)  idx = sp->frame_count - 1;
	image_t *fr = &sp->frames[idx];
	if (!fr->rgba || fr->w <= 0 || fr->h <= 0)
		return;

	int w = sp->w, h = sp->h;
	if (w <= 0 && h <= 0) { w = fr->w; h = fr->h; }
	else if (h <= 0)        h = (int)((long)w * fr->h / fr->w);
	else if (w <= 0)        w = (int)((long)h * fr->w / fr->h);
	if (w < 1) w = 1;
	if (h < 1) h = 1;

	int ax = (sp->x < 0) ? (int)buf->width  / 2 : sp->x;
	int ay = (sp->y < 0) ? (int)buf->height / 2 : sp->y;
	int x = ax, y = ay;
	if      (sp->align == ALIGN_CENTER) x -= w / 2;
	else if (sp->align == ALIGN_RIGHT)  x -= w;
	if      (sp->valign == VALIGN_MIDDLE) y -= h / 2;
	else if (sp->valign == VALIGN_BOTTOM) y -= h;

	draw_image(buf, x, y, w, h, fr->rgba, fr->w, fr->h,
	           sp->filter, sp->opacity);
}

void draw_arc_bar(drm_buffer_t *buf, arc_bar_t *ab, uint64_t now) {
	float op = fclamp(ab->opacity, 0.0f, 1.0f);
	if (op <= 0.0f || ab->radius <= 0)
		return;

	int cx = (ab->x < 0) ? (int)buf->width  / 2 : ab->x;
	int cy = (ab->y < 0) ? (int)buf->height / 2 : ab->y;
	int R  = ab->radius;

	int thickness = (ab->thickness > 0) ? ab->thickness : (R / 4 > 0 ? R / 4 : 1);
	if (thickness > R) thickness = R;

	float mid_r  = (float)R - thickness * 0.5f;
	float half_t = thickness * 0.5f;

	float sweep_deg = (ab->sweep > 0.0f && ab->sweep < 360.0f) ? ab->sweep : 360.0f;
	float sweep_rad = deg2rad(sweep_deg);
	float start_rad = deg2rad(ab->start_angle);

	/* Determine the filled portion (indeterminate overrides value). */
	float fill_start_rad, fill_rad;
	if (ab->indeterminate && ab->indet_period_ms > 0) {
		float phase = fmodf((float)(now - ab->indet_start_ms) /
		                    (float)ab->indet_period_ms, 1.0f);
		fill_start_rad = start_rad + phase * sweep_rad;
		fill_rad       = sweep_rad / 3.0f;
	} else {
		fill_start_rad = start_rad;
		fill_rad       = fclamp(ab->value, 0.0f, 1.0f) * sweep_rad;
	}

	int has_bg   = (ab->bg_color >> 24) > 0;
	int has_fill = (fill_rad > 0.001f);

	int x0 = clamp(cx - R - 2, 0, (int)buf->width);
	int y0 = clamp(cy - R - 2, 0, (int)buf->height);
	int x1 = clamp(cx + R + 2, 0, (int)buf->width);
	int y1 = clamp(cy + R + 2, 0, (int)buf->height);

	/* Background and fill share the ring SDF and the pixel angle, so compute
	 * `dist` and the atan2 once per pixel and apply both (background first,
	 * then fill on top) - identical output to two passes, half the sqrt/atan2. */
	if (has_bg || has_fill) {
		int gradient = (ab->bar_gradient != GRAD_NONE) &&
		               ((ab->bar_color2 >> 24) > 0);
		for (int py = y0; py < y1; py++) {
			uint32_t *row = (uint32_t *)(buf->map + py * buf->pitch);
			for (int px = x0; px < x1; px++) {
				float dx   = (float)px - cx + 0.5f;
				float dy   = (float)py - cy + 0.5f;
				float dist = sqrtf(dx*dx + dy*dy);
				float ring = fclamp(0.5f - (fabsf(dist - mid_r) - half_t),
				                    0.0f, 1.0f);
				if (ring <= 0.0f) continue;

				float theta = atan2f(dy, dx);

				if (has_bg) {
					float ang = fmodf(theta - start_rad, 2.0f*(float)M_PI);
					if (ang < 0.0f) ang += 2.0f*(float)M_PI;
					float a = arc_pixel_alpha(ring, dist, ang, sweep_rad);
					if (a > 0.0f)
						blend_pixel(&row[px],
						            apply_opacity(ab->bg_color, a * op));
				}

				if (has_fill) {
					float ang = fmodf(theta - fill_start_rad, 2.0f*(float)M_PI);
					if (ang < 0.0f) ang += 2.0f*(float)M_PI;
					/* Indeterminate fill can wrap past the sweep boundary;
					 * otherwise the fill starts at the arc start so `ang` is
					 * the same offset, clamped to the sweep. */
					float a = ab->indeterminate
					          ? arc_pixel_alpha(ring, dist, ang, fill_rad)
					          : arc_pixel_alpha(ring, dist, ang,
					                            fclamp(fill_rad, 0.0f, sweep_rad));
					if (a > 0.0f) {
						uint32_t c = gradient
						   ? lerp_color(ab->bar_color, ab->bar_color2,
						               fclamp(ang / fill_rad, 0.0f, 1.0f))
						   : ab->bar_color;
						blend_pixel(&row[px], apply_opacity(c, a * op));
					}
				}
			}
		}
	}

	/* Pass 3: round end caps for the filled arc */
	if (ab->cap && has_fill) {
		float endpoints[2][2] = {
			{ cx + mid_r * cosf(fill_start_rad),          cy + mid_r * sinf(fill_start_rad)          },
			{ cx + mid_r * cosf(fill_start_rad + fill_rad), cy + mid_r * sinf(fill_start_rad + fill_rad) },
		};
		for (int side = 0; side < 2; side++) {
			float capx = endpoints[side][0];
			float capy = endpoints[side][1];
			int bx0 = clamp((int)(capx - half_t) - 1, 0, (int)buf->width);
			int by0 = clamp((int)(capy - half_t) - 1, 0, (int)buf->height);
			int bx1 = clamp((int)(capx + half_t) + 2, 0, (int)buf->width);
			int by1 = clamp((int)(capy + half_t) + 2, 0, (int)buf->height);
			for (int py = by0; py < by1; py++) {
				uint32_t *row = (uint32_t *)(buf->map + py * buf->pitch);
				for (int px = bx0; px < bx1; px++) {
					float dx = (float)px - capx + 0.5f;
					float dy = (float)py - capy + 0.5f;
					float a  = fclamp(half_t + 0.5f - sqrtf(dx*dx + dy*dy), 0.0f, 1.0f);
					if (a <= 0.0f) continue;
					blend_pixel(&row[px], apply_opacity(ab->bar_color, a * op));
				}
			}
		}
	}

	/* Centre label */
	if (ab->show_percent && ab->font_slot >= 0 && ab->font_size > 0.0f) {
		draw_centered_percent(buf, ab->value, cx, cy,
		                      ab->font_slot, ab->font_size,
		                      ab->text_color, ab->opacity);
	}
}

/* ========================================================================
 * Frame Rendering
 * ======================================================================== */

/* Compose one complete frame into the back buffer, then present it. */
void render_frame(splash_state_t *st) {
	drm_buffer_t *buf = &st->drm.buf[st->drm.front_buf ^ 1];
	uint64_t now = now_ms();

	draw_filled_rect(buf, 0, 0, buf->width, buf->height, st->bg_color);

	/* Outgoing background, underneath, during a crossfade. bg_prev.rgba is
	 * non-NULL exactly while an outgoing image is held, so it is its own flag. */
	if (st->bg_prev.rgba) {
		int dx, dy, dw, dh;
		calculate_scaled_rect(buf->width, buf->height,
		                      st->bg_prev.w, st->bg_prev.h,
		                      st->bg_prev_scale_mode,
		                      st->bg_prev_custom_scale,
		                      &dx, &dy, &dw, &dh);
		draw_image(buf, dx, dy, dw, dh, st->bg_prev.rgba,
		           st->bg_prev.w, st->bg_prev.h,
		           st->bg_prev_filter, 1.0f);
	}

	/* Current background (fades in over the previous one). */
	if (st->bg_loaded && st->bg_image.rgba) {
		int dx, dy, dw, dh;
		calculate_scaled_rect(buf->width, buf->height,
		                      st->bg_image.w, st->bg_image.h,
		                      st->bg_scale_mode, st->bg_custom_scale,
		                      &dx, &dy, &dw, &dh);

		/* Steady state (no crossfade, opacity 1, kernel filter): resample once
		 * into the cache, then blit it 1:1 - bit-identical to the live path but
		 * without re-Lanczos-ing the same image every frame. The brief crossfade
		 * (opacity < 1) and the cheap nearest filter use the direct path. */
		int cacheable = (st->bg_opacity >= 1.0f) &&
		                (st->bg_filter != IMG_NEAREST) && dw > 0 && dh > 0;
		if (cacheable) {
			if (st->bg_cache_dirty || !st->bg_cache.rgba ||
			    st->bg_cache.w != dw || st->bg_cache.h != dh ||
			    st->bg_cache_filter != st->bg_filter) {
				free_image(&st->bg_cache);
				st->bg_cache.rgba = malloc((size_t)dw * dh * 4);
				if (st->bg_cache.rgba) {
					resample_image_to(st->bg_cache.rgba, dw, dh,
					                  st->bg_image.rgba,
					                  st->bg_image.w, st->bg_image.h,
					                  st->bg_filter);
					st->bg_cache.w      = dw;
					st->bg_cache.h      = dh;
					st->bg_cache_filter = st->bg_filter;
				}
				st->bg_cache_dirty = 0;
			}
		}

		if (cacheable && st->bg_cache.rgba)
			draw_image(buf, dx, dy, dw, dh, st->bg_cache.rgba,
			           dw, dh, IMG_NEAREST, 1.0f);
		else
			draw_image(buf, dx, dy, dw, dh, st->bg_image.rgba,
			           st->bg_image.w, st->bg_image.h,
			           st->bg_filter, st->bg_opacity);
	}

	/* Image overlays. */
	for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
		image_overlay_t *ov = &st->overlays[i];
		if (!ov->active || ov->hidden || !ov->img.rgba)
			continue;

		/* Resolve the draw size. If only one of w/h is given, the other
		 * is derived from the image's aspect ratio; if neither is given,
		 * the image is drawn at its native size. */
		int w = ov->w, h = ov->h;
		if (w <= 0 && h <= 0) {
			w = ov->img.w;
			h = ov->img.h;
		} else if (h <= 0) {
			h = (int)((long)w * ov->img.h / ov->img.w);
		} else if (w <= 0) {
			w = (int)((long)h * ov->img.w / ov->img.h);
		}
		if (w < 1) w = 1;
		if (h < 1) h = 1;

		/* Resolve the top-left corner from the anchor + alignment.
		 * A negative x or y anchors to the screen centre on that axis. */
		int anchor_x = (ov->x < 0) ? (int)buf->width  / 2 : ov->x;
		int anchor_y = (ov->y < 0) ? (int)buf->height / 2 : ov->y;
		int x = anchor_x, y = anchor_y;
		if      (ov->align == ALIGN_CENTER) x -= w / 2;
		else if (ov->align == ALIGN_RIGHT)  x -= w;
		if      (ov->valign == VALIGN_MIDDLE) y -= h / 2;
		else if (ov->valign == VALIGN_BOTTOM) y -= h;

		if (ov->angle != 0.0f || ov->radius > 0 || (ov->tint >> 24) > 0)
			draw_image_ex(buf, x, y, w, h, ov->img.rgba,
			              ov->img.w, ov->img.h, ov->opacity,
			              ov->angle, ov->radius, ov->tint);
		else
			draw_image(buf, x, y, w, h, ov->img.rgba,
			           ov->img.w, ov->img.h, ov->filter, ov->opacity);
	}

	for (int i = 0; i < MAX_SPRITES; i++) {
		if (st->sprites[i].active && !st->sprites[i].hidden)
			draw_sprite(buf, &st->sprites[i]);
	}

	/* Rectangles, then ellipses, then progress bars, then text, spinners on top. */
	for (int i = 0; i < MAX_RECTANGLES; i++) {
		if (st->rects[i].active && !st->rects[i].hidden)
			draw_rect_element(buf, &st->rects[i]);
	}

	for (int i = 0; i < MAX_ELLIPSES; i++) {
		if (st->ellipses[i].active && !st->ellipses[i].hidden)
			draw_ellipse(buf, &st->ellipses[i]);
	}

	for (int i = 0; i < MAX_LINES; i++) {
		if (st->lines[i].active && !st->lines[i].hidden)
			draw_line(buf, &st->lines[i]);
	}

	for (int i = 0; i < MAX_STEPPERS; i++) {
		if (st->steppers[i].active && !st->steppers[i].hidden)
			draw_stepper(buf, &st->steppers[i]);
	}

	for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
		if (st->bars[i].active && !st->bars[i].hidden)
			draw_progress_bar(buf, &st->bars[i]);
	}

	for (int i = 0; i < MAX_ARC_BARS; i++) {
		if (st->arcs[i].active && !st->arcs[i].hidden)
			draw_arc_bar(buf, &st->arcs[i], now);
	}

	for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
		if (st->texts[i].active && !st->texts[i].hidden)
			draw_text_element(buf, &st->texts[i]);
	}

	for (int i = 0; i < MAX_MARQUEES; i++) {
		if (st->marquees[i].active && !st->marquees[i].hidden)
			draw_marquee(buf, &st->marquees[i]);
	}

	for (int i = 0; i < MAX_CONSOLES; i++) {
		if (st->consoles[i].active && !st->consoles[i].hidden)
			draw_console_element(buf, &st->consoles[i]);
	}

	for (int i = 0; i < MAX_QR_ELEMENTS; i++) {
		if (st->qrs[i].active && !st->qrs[i].hidden)
			draw_qr_element(buf, &st->qrs[i]);
	}

	for (int i = 0; i < MAX_SPINNERS; i++) {
		if (st->spinners[i].active)
			draw_spinner(buf, &st->spinners[i], now);
	}

	/* Fade-out overlay on exit: a full-screen colour ramping to opaque between
	 * fade_start_ms and exit_at_ms, on top of everything. */
	if (st->fade_active && st->exit_at_ms > st->fade_start_ms) {
		uint64_t span = st->exit_at_ms - st->fade_start_ms;
		float p = (float)(now - st->fade_start_ms) / (float)span;
		if (p < 0.0f) p = 0.0f;
		if (p > 1.0f) p = 1.0f;
		uint32_t a = (uint32_t)(p * 255.0f + 0.5f);
		uint32_t c = (st->fade_color & 0x00FFFFFFu) | (a << 24);
		paint_t fade = { c, c, GRAD_NONE };
		draw_round_rect(buf, 0.0f, 0.0f,
		                (float)buf->width, (float)buf->height, 0.0f, &fade);
	}

	drm_flip(&st->drm);
	st->needs_render = 0;
}
