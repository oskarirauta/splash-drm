/*
 * render.c - Graphics rendering engine
 * 
 * CPU-based rendering with alpha blending.
 * All drawing operations work directly on DRM dumb buffers.
 */

#include "splash.h"

/* ========================================================================
 * Pixel Operations
 * ======================================================================== */

static inline void blend_pixel(uint32_t *dst, uint32_t src) {
    uint8_t sa = src >> 24;
    if (sa == 0) return;
    if (sa == 255) { *dst = src; return; }
    
    uint32_t d = *dst;
    uint8_t da = d >> 24;
    uint8_t sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
    uint8_t dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    
    uint16_t a = sa + ((da * (255 - sa)) >> 8);
    uint16_t r = ((sr * sa) + (dr * (255 - sa))) >> 8;
    uint16_t g = ((sg * sa) + (dg * (255 - sa))) >> 8;
    uint16_t b = ((sb * sa) + (db * (255 - sa))) >> 8;
    
    *dst = argb((uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* ========================================================================
 * Rectangle Drawing
 * ======================================================================== */

void draw_filled_rect(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color) {
    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);
    
    for (int row = y0; row < y1; row++) {
        uint32_t *line = (uint32_t*)(buf->map + row * buf->pitch);
        for (int col = x0; col < x1; col++) {
            line[col] = color;
        }
    }
}

void draw_rect_blend(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color) {
    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);
    
    for (int row = y0; row < y1; row++) {
        uint32_t *line = (uint32_t*)(buf->map + row * buf->pitch);
        for (int col = x0; col < x1; col++) {
            blend_pixel(&line[col], color);
        }
    }
}

void draw_rect_element(drm_buffer_t *buf, rect_element_t *re) {
    if (!re || !re->active) return;
    
    if (re->blend) {
        draw_rect_blend(buf, re->x, re->y, re->w, re->h, re->color);
    } else {
        draw_filled_rect(buf, re->x, re->y, re->w, re->h, re->color);
    }
}

/* ========================================================================
 * Image Drawing with Scaling
 * ======================================================================== */

void draw_image(drm_buffer_t *buf, int x, int y, int w, int h, 
                const uint8_t *rgba, int img_w, int img_h) {
    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);
    
    if (w <= 0 || h <= 0 || img_w <= 0 || img_h <= 0) return;
    
    float scale_x = (float)img_w / w;
    float scale_y = (float)img_h / h;
    
    for (int row = y0; row < y1; row++) {
        uint32_t *line = (uint32_t*)(buf->map + row * buf->pitch);
        int src_y = (int)((row - y) * scale_y);
        if (src_y >= img_h) src_y = img_h - 1;
        if (src_y < 0) src_y = 0;
        
        for (int col = x0; col < x1; col++) {
            int src_x = (int)((col - x) * scale_x);
            if (src_x >= img_w) src_x = img_w - 1;
            if (src_x < 0) src_x = 0;
            
            const uint8_t *p = rgba + (src_y * img_w + src_x) * 4;
            uint32_t color = argb(p[3], p[0], p[1], p[2]);
            blend_pixel(&line[col], color);
        }
    }
}

void calculate_scaled_rect(int buf_w, int buf_h, int img_w, int img_h,
                           int mode, int *out_x, int *out_y, int *out_w, int *out_h) {
    switch (mode) {
        case SCALE_STRETCH:
            *out_x = 0; *out_y = 0;
            *out_w = buf_w; *out_h = buf_h;
            break;
        case SCALE_NONE:
            *out_x = (buf_w - img_w) / 2;
            *out_y = (buf_h - img_h) / 2;
            *out_w = img_w; *out_h = img_h;
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
    if (!pb || !pb->active) return;
    
    int bw = 2; /* border width */
    
    /* Border */
    draw_filled_rect(buf, pb->x, pb->y, pb->w, pb->h, pb->border_color);
    /* Background */
    draw_filled_rect(buf, pb->x + bw, pb->y + bw, 
                     pb->w - 2*bw, pb->h - 2*bw, pb->bg_color);
    
    /* Fill */
    int fill_w = (int)((pb->w - 2*bw) * pb->value / 100.0f);
    if (fill_w > 0) {
        draw_filled_rect(buf, pb->x + bw, pb->y + bw, fill_w, 
                         pb->h - 2*bw, pb->fg_color);
    }
    
    /* Text overlay */
    char text[256];
    if (pb->inner[0]) {
        snprintf(text, sizeof(text), "%s%s%s %.0f%%", 
                 pb->prefix, pb->inner, pb->suffix, pb->value);
    } else {
        snprintf(text, sizeof(text), "%s%.0f%%%s", 
                 pb->prefix, pb->value, pb->suffix);
    }
    
    int tw = text_width(text);
    int tx = pb->x + (pb->w - tw) / 2;
    int th = text_height();
    int ty = pb->y + (pb->h - th) / 2;
    
    /* Draw text with a slight shadow for readability */
    text_element_t te = {
        .active = 1,
        .x = tx,
        .y = ty,
        .align = ALIGN_LEFT,
        .color = pb->text_color
    };
    strncpy(te.text, text, 255);
    draw_text_element(buf, &te);
}

/* ========================================================================
 * Main Frame Rendering
 * ======================================================================== */

void render_frame(splash_state_t *st) {
    drm_buffer_t *buf = &st->drm.buf[st->drm.front_buf ^ 1];
    
    /* Clear to black */
    memset(buf->map, 0, buf->size);
    
    /* Background image */
    if (st->bg_loaded && st->bg_image.rgba) {
        int dx, dy, dw, dh;
        calculate_scaled_rect(buf->width, buf->height, 
                               st->bg_image.w, st->bg_image.h,
                               st->bg_scale_mode, &dx, &dy, &dw, &dh);
        draw_image(buf, dx, dy, dw, dh, st->bg_image.rgba, st->bg_image.w, st->bg_image.h);
    }
    
    /* Rectangles (drawn before overlays, can be used to cover bg areas) */
    for (int i = 0; i < MAX_RECTANGLES; i++) {
        draw_rect_element(buf, &st->rects[i]);
    }
    
    /* Image overlays */
    for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
        image_overlay_t *ov = &st->overlays[i];
        if (!ov->active || !ov->img.rgba) continue;
        
        int draw_w = (ov->w > 0) ? ov->w : ov->img.w;
        int draw_h = (ov->h > 0) ? ov->h : ov->img.h;
        
        int draw_x = ov->x;
        int draw_y = ov->y;
        
        if (ov->align == ALIGN_CENTER) draw_x -= draw_w / 2;
        else if (ov->align == ALIGN_RIGHT) draw_x -= draw_w;
        
        if (ov->valign == VALIGN_MIDDLE) draw_y -= draw_h / 2;
        else if (ov->valign == VALIGN_BOTTOM) draw_y -= draw_h;
        
        draw_image(buf, draw_x, draw_y, draw_w, draw_h, 
                   ov->img.rgba, ov->img.w, ov->img.h);
    }
    
    /* Progress bars */
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        draw_progress_bar(buf, &st->bars[i]);
    }
    
    /* Text elements (drawn last, on top of everything) */
    for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
        draw_text_element(buf, &st->texts[i]);
    }
    
    /* Flip buffers */
    drm_flip(&st->drm);
    st->needs_render = 0;
    st->ready = 1;
}
