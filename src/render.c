/*
 * render.c - Frame rendering and drawing primitives
 *
 * Rounded geometry (progress bars + RECT elements) is rendered with a
 * signed-distance field. One shared routine means the track, the fill and
 * the border are always the *same* curve, so they nest pixel-perfectly,
 * and every edge is anti-aliased.
 */

#include "splash.h"
#include <math.h>

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
 * Anti-aliased Rounded Rectangle (signed distance field)
 *
 * Coverage is derived analytically from the distance to the shape's edge,
 * which gives a smooth 1px edge and a corner radius that is independent of
 * how wide the shape happens to be.
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

/* Filled anti-aliased rounded rectangle. */
void draw_round_rect(drm_buffer_t *buf, float x, float y, float w, float h,
                     float radius, uint32_t color) {
    if (w <= 0.0f || h <= 0.0f) return;
    radius = clamp_radius(w, h, radius);

    float hx = w * 0.5f, hy = h * 0.5f;
    float cx = x + hx,   cy = y + hy;

    int x0 = clamp((int)floorf(x),       0, (int)buf->width);
    int y0 = clamp((int)floorf(y),       0, (int)buf->height);
    int x1 = clamp((int)ceilf (x + w),   0, (int)buf->width);
    int y1 = clamp((int)ceilf (y + h),   0, (int)buf->height);

    for (int py = y0; py < y1; py++) {
        uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
        float fy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++, line++) {
            float d = sdf_round_rect((float)px + 0.5f, fy, cx, cy, hx, hy, radius);
            blend_coverage(line, color, 0.5f - d);   /* 1px linear edge ramp */
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
 * The shape - and therefore the corner radius - is always the full
 * rectangle; only how much of it is shown changes. The left corners are
 * thus rock-steady regardless of the progress value, and once fill_w
 * reaches w the right corners round off on their own. */
void draw_round_rect_progress(drm_buffer_t *buf, float x, float y, float w, float h,
                              float radius, float fill_w, uint32_t color) {
    if (w <= 0.0f || h <= 0.0f || fill_w <= 0.0f) return;
    radius = clamp_radius(w, h, radius);

    if (fill_w > w) fill_w = w;
    int full = (fill_w >= w - 0.01f);

    float hx = w * 0.5f, hy = h * 0.5f;
    float cx = x + hx,   cy = y + hy;
    float clip_edge = x + fill_w;        /* absolute x of the soft right edge */

    int x0 = clamp((int)floorf(x),                            0, (int)buf->width);
    int y0 = clamp((int)floorf(y),                            0, (int)buf->height);
    int x1 = clamp((int)ceilf(x + (full ? w : fill_w)) + 1,   0, (int)buf->width);
    int y1 = clamp((int)ceilf(y + h),                         0, (int)buf->height);

    for (int py = y0; py < y1; py++) {
        uint32_t *line = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
        float fy = (float)py + 0.5f;
        for (int px = x0; px < x1; px++, line++) {
            float d = sdf_round_rect((float)px + 0.5f, fy, cx, cy, hx, hy, radius);
            float cov = 0.5f - d;
            if (cov <= 0.0f) continue;
            if (cov > 1.0f) cov = 1.0f;
            if (!full) {
                /* soft 1px vertical cut at the progress edge */
                float hmask = clip_edge - (float)px;
                if (hmask <= 0.0f) continue;
                if (hmask < 1.0f) cov *= hmask;
            }
            blend_coverage(line, color, cov);
        }
    }
}

/* ========================================================================
 * Image Drawing
 * ======================================================================== */

void draw_image(drm_buffer_t *buf, int x, int y, int w, int h,
    const uint8_t *rgba, int img_w, int img_h) {
    if (!rgba || img_w <= 0 || img_h <= 0) return;

    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);

    float scale_x = (float)img_w / w;
    float scale_y = (float)img_h / h;

    for (int row = y0; row < y1; row++) {
        int src_y = (int)((row - y) * scale_y);
        src_y = clamp(src_y, 0, img_h - 1);
        const uint8_t *src_line = rgba + src_y * img_w * 4;
        uint32_t *dst = (uint32_t *)(buf->map + row * buf->pitch + x0 * 4);

        for (int col = x0; col < x1; col++) {
            int src_x = (int)((col - x) * scale_x);
            src_x = clamp(src_x, 0, img_w - 1);
            const uint8_t *p = src_line + src_x * 4;
            uint32_t px = argb(p[3], p[0], p[1], p[2]);
            blend_pixel(dst++, px);
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

    /* 1. Background track */
    draw_round_rect(buf, fx, fy, fw, fh, r, pb->bg_color);

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
            draw_round_rect_progress(buf, in_x, in_y, in_w, in_h,
                                     in_r, in_w * value, pb->bar_color);
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

    /* A rect is filled unless it is explicitly outline-only
     * (fill == 0 together with a positive border width). */
    int do_fill = re->fill || re->border_width <= 0;

    if (do_fill)
        draw_round_rect(buf, fx, fy, fw, fh, r, re->color);

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
        draw_image(buf, dx, dy, dw, dh, st->bg_image.rgba, st->bg_image.w, st->bg_image.h);
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

        draw_image(buf, x, y, w, h, ov->img.rgba, ov->img.w, ov->img.h);
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
