/*
 * render.c - Frame rendering and drawing primitives
 *
 * Rounded geometry (progress bars + RECT elements) is rendered with a
 * signed-distance field. One shared routine means the track, the fill and
 * the border are always the *same* curve, so they nest pixel-perfectly,
 * every edge is anti-aliased, and gradients + soft shadows come for free.
 *
 * Images are resampled with a proper separable-quality kernel
 * (Lanczos-3 / Mitchell bicubic / bilinear) instead of nearest-neighbour.
 */

#include "splash.h"
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ========================================================================
 * Basic Rectangle Primitives
 * ======================================================================== */

void draw_filled_rect(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color) {
    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);

    for (int row = y0; row < y1; row++) {
        uint32_t *line = (uint32_t *)(buf->map + row * buf->pitch + x0 * 4);
        for (int col = x0; col < x1; col++)
            *line++ = color;
    }
}

void draw_rect_blend(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color) {
    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);

    for (int row = y0; row < y1; row++) {
        uint32_t *line = (uint32_t *)(buf->map + row * buf->pitch + x0 * 4);
        for (int col = x0; col < x1; col++, line++)
            blend_pixel(line, color);
    }
}

/* ========================================================================
 * Paint - solid colour or 2-stop linear gradient
 * ======================================================================== */

/* Linear interpolation between two ARGB colours (straight-alpha, sRGB). */
static inline uint32_t lerp_color(uint32_t a, uint32_t b, float t) {
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    int aa = (a >> 24) & 0xFF, ar = (a >> 16) & 0xFF, ag = (a >> 8) & 0xFF, ab = a & 0xFF;
    int ba = (b >> 24) & 0xFF, br = (b >> 16) & 0xFF, bg = (b >> 8) & 0xFF, bb = b & 0xFF;
    int ca = aa + (int)((ba - aa) * t + 0.5f);
    int cr = ar + (int)((br - ar) * t + 0.5f);
    int cg = ag + (int)((bg - ag) * t + 0.5f);
    int cb = ab + (int)((bb - ab) * t + 0.5f);
    return argb((uint8_t)ca, (uint8_t)cr, (uint8_t)cg, (uint8_t)cb);
}

/* Sample a paint at parametric position (u,v), both in [0,1] over the shape. */
static inline uint32_t paint_at(const paint_t *p, float u, float v) {
    if (p->gradient == GRAD_NONE) return p->color0;
    float t;
    if      (p->gradient == GRAD_HORIZONTAL) t = u;
    else if (p->gradient == GRAD_DIAGONAL)   t = (u + v) * 0.5f;
    else                                     t = v;   /* GRAD_VERTICAL */
    return lerp_color(p->color0, p->color1, t);
}

/* Convenience: a solid paint from a single colour. */
static inline paint_t paint_solid(uint32_t c) {
    paint_t p;
    p.color0 = c;
    p.color1 = c;
    p.gradient = GRAD_NONE;
    return p;
}

/* ========================================================================
 * Anti-aliased Rounded Rectangle (signed distance field)
 * ======================================================================== */

/* Signed distance from (px,py) to a rounded rectangle.
 * Box centred at (cx,cy), half-extents (hx,hy), corner radius r.
 * Negative inside, positive outside, zero exactly on the edge.
 * sqrtf() is only evaluated inside the four corner quadrants. */
static inline float sdf_round_rect(float px, float py,
                                   float cx, float cy,
                                   float hx, float hy, float r) {
    float qx = fabsf(px - cx) - hx + r;
    float qy = fabsf(py - cy) - hy + r;
    float ox = qx > 0.0f ? qx : 0.0f;
    float oy = qy > 0.0f ? qy : 0.0f;
    float outside;
    if (ox > 0.0f && oy > 0.0f)
        outside = sqrtf(ox * ox + oy * oy);   /* true corner region */
    else
        outside = ox + oy;                    /* straight edge: one term is 0 */
    float inside = fminf(fmaxf(qx, qy), 0.0f);
    return outside + inside - r;
}

/* Blend `coverage` (0..1) worth of `color` onto a single pixel. */
static inline void blend_coverage(uint32_t *dst, uint32_t color, float coverage) {
    if (coverage <= 0.0f) return;
    if (coverage > 1.0f) coverage = 1.0f;
    uint32_t a = (uint32_t)((float)(color >> 24) * coverage + 0.5f);
    if (a == 0) return;
    blend_pixel(dst, (color & 0x00FFFFFFu) | (a << 24));
}

/* Clamp a corner radius so it never exceeds half of the shorter side. */
static float clamp_radius(float w, float h, float r) {
    if (r < 0.0f) r = 0.0f;
    float maxr = (w < h ? w : h) * 0.5f;
    if (r > maxr) r = maxr;
    return r;
}

/* Filled anti-aliased rounded rectangle, solid colour or gradient. */
void draw_round_rect(drm_buffer_t *buf, float x, float y, float w, float h,
                     float radius, const paint_t *paint) {
    if (w <= 0.0f || h <= 0.0f) return;
    radius = clamp_radius(w, h, radius);

    float hx = w * 0.5f, hy = h * 0.5f;
    float cx = x + hx,   cy = y + hy;
    float inv_w = 1.0f / w, inv_h = 1.0f / h;
    int grad = paint->gradient;

    int x0 = clamp((int)floorf(x),       0, (int)buf->width);
    int y0 = clamp((int)floorf(y),       0, (int)buf->height);
    int x1 = clamp((int)ceilf (x + w),   0, (int)buf->width);
    int y1 = clamp((int)ceilf (y + h),   0, (int)buf->height);

    for (int py = y0; py < y1; py++) {
        uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
        float fy = (float)py + 0.5f;
        float v  = (fy - y) * inv_h;
        for (int px = x0; px < x1; px++, line++) {
            float fx = (float)px + 0.5f;
            float d  = sdf_round_rect(fx, fy, cx, cy, hx, hy, radius);
            float cov = 0.5f - d;                  /* 1px linear edge ramp */
            if (cov <= 0.0f) continue;
            uint32_t c = grad ? paint_at(paint, (fx - x) * inv_w, v)
                              : paint->color0;
            blend_coverage(line, c, cov);
        }
    }
}

/* Anti-aliased rounded-rectangle outline, grown inward from the outer edge. */
void draw_round_rect_outline(drm_buffer_t *buf, float x, float y, float w, float h,
                             float radius, float border_width, uint32_t color) {
    if (w <= 0.0f || h <= 0.0f || border_width <= 0.0f) return;
    radius = clamp_radius(w, h, radius);

    float maxbw = (w < h ? w : h) * 0.5f;
    if (border_width > maxbw) border_width = maxbw;

    float hx = w * 0.5f, hy = h * 0.5f;
    float cx = x + hx,   cy = y + hy;
    float half_bw = border_width * 0.5f;

    int x0 = clamp((int)floorf(x),       0, (int)buf->width);
    int y0 = clamp((int)floorf(y),       0, (int)buf->height);
    int x1 = clamp((int)ceilf (x + w),   0, (int)buf->width);
    int y1 = clamp((int)ceilf (y + h),   0, (int)buf->height);

    for (int py = y0; py < y1; py++) {
        uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
        float fy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++, line++) {
            float d = sdf_round_rect((float)px + 0.5f, fy, cx, cy, hx, hy, radius);
            /* distance to the stroke band that occupies d in [-border_width, 0] */
            float band = fabsf(d + half_bw) - half_bw;
            blend_coverage(line, color, 0.5f - band);
        }
    }
}

/* Filled rounded rectangle revealed left-to-right up to `fill_w`.
 *
 * The shape - and the corner radius - is always the full rectangle; only
 * how much of it is shown changes. The gradient is anchored to the full
 * shape, so it does not slide around as the bar fills. */
void draw_round_rect_progress(drm_buffer_t *buf, float x, float y, float w, float h,
                              float radius, float fill_w, const paint_t *paint) {
    if (w <= 0.0f || h <= 0.0f || fill_w <= 0.0f) return;
    radius = clamp_radius(w, h, radius);

    if (fill_w > w) fill_w = w;
    int full = (fill_w >= w - 0.01f);

    float hx = w * 0.5f, hy = h * 0.5f;
    float cx = x + hx,   cy = y + hy;
    float inv_w = 1.0f / w, inv_h = 1.0f / h;
    int grad = paint->gradient;
    float clip_edge = x + fill_w;          /* absolute x of the soft right edge */

    int x0 = clamp((int)floorf(x),                            0, (int)buf->width);
    int y0 = clamp((int)floorf(y),                            0, (int)buf->height);
    int x1 = clamp((int)ceilf(x + (full ? w : fill_w)) + 1,   0, (int)buf->width);
    int y1 = clamp((int)ceilf(y + h),                         0, (int)buf->height);

    for (int py = y0; py < y1; py++) {
        uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
        float fy = (float)py + 0.5f;
        float v  = (fy - y) * inv_h;
        for (int px = x0; px < x1; px++, line++) {
            float fx = (float)px + 0.5f;
            float d  = sdf_round_rect(fx, fy, cx, cy, hx, hy, radius);
            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            if (!full) {
                /* soft 1px vertical cut at the progress edge */
                float hmask = clip_edge - (float)px;
                if (hmask <= 0.0f) continue;
                if (hmask < 1.0f) cov *= hmask;
            }
            uint32_t c = grad ? paint_at(paint, (fx - x) * inv_w, v)
                              : paint->color0;
            blend_coverage(line, c, cov);
        }
    }
}

/* Soft drop shadow shaped like a rounded rectangle.
 *
 * The shadow is the same SDF shape with a smooth falloff over `blur`
 * pixels. Draw it BEFORE the element, offset by (dx,dy). */
void draw_round_rect_shadow(drm_buffer_t *buf, float x, float y, float w, float h,
                            float radius, float blur, uint32_t color) {
    if (w <= 0.0f || h <= 0.0f) return;
    radius = clamp_radius(w, h, radius);
    if (blur < 0.5f) blur = 0.5f;

    float hx = w * 0.5f, hy = h * 0.5f;
    float cx = x + hx,   cy = y + hy;
    float inv = 1.0f / (2.0f * blur);

    int x0 = clamp((int)floorf(x - blur),     0, (int)buf->width);
    int y0 = clamp((int)floorf(y - blur),     0, (int)buf->height);
    int x1 = clamp((int)ceilf (x + w + blur), 0, (int)buf->width);
    int y1 = clamp((int)ceilf (y + h + blur), 0, (int)buf->height);

    for (int py = y0; py < y1; py++) {
        uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
        float fy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++, line++) {
            float d = sdf_round_rect((float)px + 0.5f, fy, cx, cy, hx, hy, radius);
            float cov = (blur - d) * inv;            /* 1 well inside, 0 outside */
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            cov = cov * cov * (3.0f - 2.0f * cov);   /* smoothstep falloff */
            blend_coverage(line, color, cov);
        }
    }
}

/* ========================================================================
 * Image Resampling
 *
 * draw_image() is a direct resampler: each destination pixel is a weighted
 * sum of source texels. When the image is minified the filter is widened
 * by the scale factor, so downscaling averages instead of aliasing.
 * ======================================================================== */

/* Resampling kernel weight at signed offset x, for the chosen filter. */
static float kernel_weight(float x, int filter) {
    x = fabsf(x);
    switch (filter) {
    case IMG_BILINEAR:
        return x < 1.0f ? 1.0f - x : 0.0f;

    case IMG_BICUBIC: {                       /* Mitchell-Netravali B=C=1/3 */
        float x2 = x * x, x3 = x2 * x;
        if (x < 1.0f) return (7.0f / 6.0f) * x3 - 2.0f * x2 + 8.0f / 9.0f;
        if (x < 2.0f) return -(7.0f / 18.0f) * x3 + 2.0f * x2
                             - (10.0f / 3.0f) * x + 16.0f / 9.0f;
        return 0.0f;
    }

    case IMG_LANCZOS:                         /* Lanczos-3 */
    default:
        if (x >= 3.0f)   return 0.0f;
        if (x < 1e-6f)   return 1.0f;
        {
            float px = (float)M_PI * x;
            return (sinf(px) / px) * (sinf(px / 3.0f) / (px / 3.0f));
        }
    }
}

static float kernel_radius(int filter) {
    switch (filter) {
    case IMG_BILINEAR: return 1.0f;
    case IMG_BICUBIC:  return 2.0f;
    case IMG_LANCZOS:
    default:           return 3.0f;
    }
}

void draw_image(drm_buffer_t *buf, int x, int y, int w, int h,
                const uint8_t *rgba, int img_w, int img_h, int filter) {
    if (!rgba || img_w <= 0 || img_h <= 0 || w <= 0 || h <= 0) return;

    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);

    /* Fast path: nearest-neighbour, or any 1:1 blit (resampling is identity). */
    if (filter == IMG_NEAREST || (w == img_w && h == img_h)) {
        float sx = (float)img_w / (float)w;
        float sy = (float)img_h / (float)h;
        for (int row = y0; row < y1; row++) {
            int syc = clamp((int)((row - y) * sy), 0, img_h - 1);
            const uint8_t *srow = rgba + syc * img_w * 4;
            uint32_t *dst = (uint32_t *)(buf->map + row * buf->pitch + x0 * 4);
            for (int col = x0; col < x1; col++) {
                int sxc = clamp((int)((col - x) * sx), 0, img_w - 1);
                const uint8_t *p = srow + sxc * 4;
                blend_pixel(dst++, argb(p[3], p[0], p[1], p[2]));
            }
        }
        return;
    }

    float sx = (float)img_w / (float)w;       /* source texels per dst pixel */
    float sy = (float)img_h / (float)h;
    float supx = sx > 1.0f ? sx : 1.0f;       /* widen filter when minifying */
    float supy = sy > 1.0f ? sy : 1.0f;
    float base_r = kernel_radius(filter);
    float radx = base_r * supx, rady = base_r * supy;
    float inv_supx = 1.0f / supx, inv_supy = 1.0f / supy;

    for (int row = y0; row < y1; row++) {
        float cy = ((float)(row - y) + 0.5f) * sy - 0.5f;   /* source coord */
        int iy0 = (int)floorf(cy - rady);
        int iy1 = (int)ceilf (cy + rady);
        uint32_t *dst = (uint32_t *)(buf->map + row * buf->pitch + x0 * 4);

        for (int col = x0; col < x1; col++, dst++) {
            float cx = ((float)(col - x) + 0.5f) * sx - 0.5f;
            int ix0 = (int)floorf(cx - radx);
            int ix1 = (int)ceilf (cx + radx);

            float ar = 0.0f, ag = 0.0f, ab = 0.0f, aa = 0.0f, ws = 0.0f;

            for (int iy = iy0; iy <= iy1; iy++) {
                float wy = kernel_weight((iy - cy) * inv_supy, filter);
                if (wy == 0.0f) continue;
                const uint8_t *srow = rgba + clamp(iy, 0, img_h - 1) * img_w * 4;
                for (int ix = ix0; ix <= ix1; ix++) {
                    float wx = kernel_weight((ix - cx) * inv_supx, filter);
                    float wgt = wx * wy;
                    if (wgt == 0.0f) continue;
                    const uint8_t *p = srow + clamp(ix, 0, img_w - 1) * 4;
                    float pa = p[3] * (1.0f / 255.0f);   /* premultiply alpha */
                    ar += p[0] * pa * wgt;
                    ag += p[1] * pa * wgt;
                    ab += p[2] * pa * wgt;
                    aa += p[3] * wgt;
                    ws += wgt;
                }
            }
            if (ws <= 0.0f || aa <= 0.0f) continue;

            float oa = aa / ws;                  /* resampled alpha 0..255 */
            if (oa < 0.5f) continue;
            float k = 255.0f / aa;               /* un-premultiply */
            int rr = clamp((int)(ar * k + 0.5f), 0, 255);
            int gg = clamp((int)(ag * k + 0.5f), 0, 255);
            int bb = clamp((int)(ab * k + 0.5f), 0, 255);
            int al = clamp((int)(oa + 0.5f),     0, 255);
            blend_pixel(dst, argb((uint8_t)al, (uint8_t)rr, (uint8_t)gg, (uint8_t)bb));
        }
    }
}

/* ========================================================================
 * Image Scaling
 * ======================================================================== */

void calculate_scaled_rect(int buf_w, int buf_h, int img_w, int img_h,
    int mode, float custom_scale, int *out_x, int *out_y, int *out_w, int *out_h) {
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
        float scale = fminf((float)buf_w / img_w, (float)buf_h / img_h);
        *out_w = (int)(img_w * scale);
        *out_h = (int)(img_h * scale);
        *out_x = (buf_w - *out_w) / 2;
        *out_y = (buf_h - *out_h) / 2;
        break;
    }

    case SCALE_COVER: {
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

void draw_progress_bar(drm_buffer_t *buf, progress_bar_t *pb) {
    /* Resolve on-screen position from anchor + alignment */
    int x, y;

    if (pb->x < 0) {
        x = ((int)buf->width - pb->w) / 2;
    } else {
        x = pb->x;
        if (pb->align == ALIGN_CENTER)        x -= pb->w / 2;
        else if (pb->align == ALIGN_RIGHT)    x -= pb->w;
    }

    if (pb->y < 0) {
        y = ((int)buf->height - pb->h) / 2;
    } else {
        y = pb->y;
        if (pb->valign == VALIGN_MIDDLE)       y -= pb->h / 2;
        else if (pb->valign == VALIGN_BOTTOM)  y -= pb->h;
    }

    float fx = (float)x,     fy = (float)y;
    float fw = (float)pb->w, fh = (float)pb->h;
    float r  = (float)pb->radius;

    float bw = (pb->borderless || pb->border_width <= 0)
                 ? 0.0f : (float)pb->border_width;

    /* 0. Soft drop shadow of the whole bar */
    if (pb->shadow) {
        draw_round_rect_shadow(buf, fx + pb->shadow_dx, fy + pb->shadow_dy,
                               fw, fh, r, (float)pb->shadow_blur, pb->shadow_color);
    }

    /* 1. Background track */
    paint_t track = paint_solid(pb->bg_color);
    draw_round_rect(buf, fx, fy, fw, fh, r, &track);

    /* 2. Progress fill: the track's inner shape, revealed left-to-right.
     *    Insetting a rounded rect by `bw` also shrinks its radius by `bw`,
     *    so the fill nests exactly inside the border with no slivers. */
    float value = fclamp(pb->value, 0.0f, 1.0f);
    if (value > 0.0f) {
        float in_x = fx + bw;
        float in_y = fy + bw;
        float in_w = fw - 2.0f * bw;
        float in_h = fh - 2.0f * bw;
        float in_r = r - bw;
        if (in_r < 0.0f) in_r = 0.0f;
        if (in_w > 0.0f && in_h > 0.0f) {
            paint_t fill = { pb->bar_color, pb->bar_color2, pb->bar_gradient };
            draw_round_rect_progress(buf, in_x, in_y, in_w, in_h,
                                     in_r, in_w * value, &fill);
        }
    }

    /* 3. Border last, on top of track + fill */
    if (bw > 0.0f) {
        draw_round_rect_outline(buf, fx, fy, fw, fh, r, bw, pb->border_color);
    }

    /* 4. Percentage text */
    if (pb->show_percent && pb->value > 0.0f) {
        char percent_str[8];
        snprintf(percent_str, sizeof(percent_str), "%d%%", (int)(pb->value * 100));

        text_element_t te = {0};
        te.active    = 1;
        strncpy(te.text, percent_str, sizeof(te.text) - 1);
        te.x         = x + pb->w / 2;
        te.y         = y + pb->h / 2;
        te.align     = ALIGN_CENTER;
        te.valign    = VALIGN_MIDDLE;
        te.color     = pb->text_color;
        te.font_slot = pb->font_slot > 0 ? pb->font_slot : 0;
        te.font_size = pb->font_size > 0 ? pb->font_size : 0;
        draw_text_element(buf, &te);
    }
}

/* ========================================================================
 * Rectangle Element Drawing
 * ======================================================================== */

void draw_rect_element(drm_buffer_t *buf, rect_element_t *re) {
    if (re->w <= 0 || re->h <= 0) return;

    float fx = (float)re->x, fy = (float)re->y;
    float fw = (float)re->w, fh = (float)re->h;
    float r  = (float)re->radius;

    /* 0. Soft drop shadow */
    if (re->shadow) {
        draw_round_rect_shadow(buf, fx + re->shadow_dx, fy + re->shadow_dy,
                               fw, fh, r, (float)re->shadow_blur, re->shadow_color);
    }

    /* A rect is filled unless it is explicitly outline-only
     * (fill == 0 together with a positive border width). */
    int do_fill = re->fill || re->border_width <= 0;

    if (do_fill) {
        paint_t paint = { re->color, re->grad_color, re->grad_dir };
        draw_round_rect(buf, fx, fy, fw, fh, r, &paint);
    }

    if (re->border_width > 0)
        draw_round_rect_outline(buf, fx, fy, fw, fh, r,
                                (float)re->border_width, re->border_color);
}

/* ========================================================================
 * Frame Rendering
 * ======================================================================== */

void render_frame(splash_state_t *st) {
    drm_buffer_t *buf = &st->drm.buf[st->drm.front_buf ^ 1];

    draw_filled_rect(buf, 0, 0, buf->width, buf->height, st->bg_color);

    if (st->bg_loaded && st->bg_image.rgba) {
        int dx, dy, dw, dh;
        calculate_scaled_rect(buf->width, buf->height,
            st->bg_image.w, st->bg_image.h,
            st->bg_scale_mode, st->bg_custom_scale, &dx, &dy, &dw, &dh);
        draw_image(buf, dx, dy, dw, dh, st->bg_image.rgba,
                   st->bg_image.w, st->bg_image.h, st->bg_filter);
    }

    for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
        image_overlay_t *ov = &st->overlays[i];
        if (!ov->active || !ov->img.rgba) continue;

        int x = ov->x, y = ov->y, w = ov->w, h = ov->h;
        if (w <= 0) w = ov->img.w;
        if (h <= 0) h = ov->img.h;

        if (ov->align == ALIGN_CENTER)
            x -= w / 2;
        else if (ov->align == ALIGN_RIGHT)
            x -= w;

        if (ov->valign == VALIGN_MIDDLE)
            y -= h / 2;
        else if (ov->valign == VALIGN_BOTTOM)
            y -= h;

        draw_image(buf, x, y, w, h, ov->img.rgba, ov->img.w, ov->img.h, ov->filter);
    }

    for (int i = 0; i < MAX_RECTANGLES; i++) {
        if (st->rects[i].active)
            draw_rect_element(buf, &st->rects[i]);
    }

    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active)
            draw_progress_bar(buf, &st->bars[i]);
    }

    for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
        if (st->texts[i].active)
            draw_text_element(buf, &st->texts[i]);
    }

    drm_flip(&st->drm);
    st->needs_render = 0;
}
