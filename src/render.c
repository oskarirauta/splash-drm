/*
 * render.c - Frame rendering and drawing primitives
 */

#include "splash.h"

/* ========================================================================
 * Drawing Primitives
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
    int x = pb->x, y = pb->y, w = pb->w, h = pb->h;

    draw_filled_rect(buf, x, y, w, h, pb->bg_color);

    int fill = (int)(w * fclamp(pb->value, 0.0f, 1.0f));
    if (fill > 0)
        draw_filled_rect(buf, x, y, fill, h, pb->fg_color);

    /* Border */
    for (int i = 0; i < 2; i++) {
        uint32_t *top = (uint32_t *)(buf->map + (y + i) * buf->pitch + x * 4);
        uint32_t *bot = (uint32_t *)(buf->map + (y + h - 1 - i) * buf->pitch + x * 4);
        for (int j = 0; j < w; j++) {
            top[j] = pb->border_color;
            bot[j] = pb->border_color;
        }
    }
    for (int i = y; i < y + h; i++) {
        uint32_t *line = (uint32_t *)(buf->map + i * buf->pitch + x * 4);
        line[0] = pb->border_color;
        line[w - 1] = pb->border_color;
    }

    /* Inner text (optional) */
    if (pb->inner[0]) {
        int tw = text_width_font(pb->inner, pb->font_slot > 0 ? pb->font_slot : 0);
        int th = text_height_font(pb->font_slot > 0 ? pb->font_slot : 0);
        int tx = x + (w - tw) / 2;
        int ty = y + (h - th) / 2;

        text_element_t te = {0};
        te.active = 1;
        strncpy(te.text, pb->inner, sizeof(te.text) - 1);
        te.x = tx;
        te.y = ty;
        te.color = pb->text_color;
        te.font_slot = pb->font_slot > 0 ? pb->font_slot : 0;
        te.font_size = pb->font_size > 0 ? pb->font_size : 0;
        draw_text_element(buf, &te);
    }
}

/* ========================================================================
 * Rectangle Element Drawing
 * ======================================================================== */

void draw_rect_element(drm_buffer_t *buf, rect_element_t *re) {
    if (re->blend)
        draw_rect_blend(buf, re->x, re->y, re->w, re->h, re->color);
    else
        draw_filled_rect(buf, re->x, re->y, re->w, re->h, re->color);
}

/* ========================================================================
 * Frame Rendering
 * ======================================================================== */

void render_frame(splash_state_t *st) {
    drm_buffer_t *buf = &st->drm.buf[st->drm.front_buf ^ 1];

    /* Clear with background color */
    draw_filled_rect(buf, 0, 0, buf->width, buf->height, st->bg_color);

    /* Background image */
    if (st->bg_loaded && st->bg_image.rgba) {
        int dx, dy, dw, dh;
        calculate_scaled_rect(buf->width, buf->height,
            st->bg_image.w, st->bg_image.h,
            st->bg_scale_mode, st->bg_custom_scale, &dx, &dy, &dw, &dh);
        draw_image(buf, dx, dy, dw, dh, st->bg_image.rgba, st->bg_image.w, st->bg_image.h);
    }

    /* Overlays */
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

    /* Rectangles */
    for (int i = 0; i < MAX_RECTANGLES; i++) {
        if (st->rects[i].active)
            draw_rect_element(buf, &st->rects[i]);
    }

    /* Progress bars */
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active)
            draw_progress_bar(buf, &st->bars[i]);
    }

    /* Text elements */
    for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
        if (st->texts[i].active)
            draw_text_element(buf, &st->texts[i]);
    }

    /* Flip */
    drm_flip(&st->drm);
    st->needs_render = 0;
}
