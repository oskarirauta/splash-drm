/*
 * render.c - Frame rendering and drawing primitives
 */

#include "splash.h"
#include "math.h"

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
 * Bresenham Circle (for rounded corners)
 * ======================================================================== */

/* Precomputed circle offsets for given radius */
typedef struct {
    int *x_offsets;  /* x offset for each y */
    int *y_offsets;  /* y offset for each x */
    int size;
} circle_lut_t;

static void build_circle_lut(circle_lut_t *lut, int r) {
    lut->size = r + 1;
    lut->x_offsets = calloc(lut->size, sizeof(int));
    lut->y_offsets = calloc(lut->size, sizeof(int));
    
    int x = 0, y = r;
    int d = 3 - 2 * r;
    
    while (y >= x) {
        lut->x_offsets[y] = x;
        lut->y_offsets[x] = y;
        
        x++;
        if (d > 0) {
            y--;
            d = d + 4 * (x - y) + 10;
        } else {
            d = d + 4 * x + 6;
        }
    }
    
    /* Fill gaps */
    for (int i = 1; i <= r; i++) {
        if (lut->x_offsets[i] == 0) lut->x_offsets[i] = lut->x_offsets[i-1];
        if (lut->y_offsets[i] == 0) lut->y_offsets[i] = lut->y_offsets[i-1];
    }
}

static void free_circle_lut(circle_lut_t *lut) {
    free(lut->x_offsets);
    free(lut->y_offsets);
    lut->size = 0;
}

/* ========================================================================
 * Rounded Rectangle Drawing (optimized with Bresenham)
 * ======================================================================== */

void draw_rounded_rect(drm_buffer_t *buf, int x, int y, int w, int h, int radius,
                       uint32_t fill_color, uint32_t border_color, int border_width,
                       int fill, int blend) {
    if (radius <= 0) {
        /* Fallback to regular rectangle */
        if (fill) {
            if (blend)
                draw_rect_blend(buf, x, y, w, h, fill_color);
            else
                draw_filled_rect(buf, x, y, w, h, fill_color);
        }
        if (border_width > 0) {
            for (int i = 0; i < border_width; i++) {
                int x0 = clamp(x + i, 0, (int)buf->width);
                int y0 = clamp(y + i, 0, (int)buf->height);
                int x1 = clamp(x + w - 1 - i, 0, (int)buf->width - 1);
                int y1 = clamp(y + h - 1 - i, 0, (int)buf->height - 1);
                
                for (int px = x0; px <= x1; px++) {
                    uint32_t *top = (uint32_t *)(buf->map + y0 * buf->pitch + px * 4);
                    uint32_t *bot = (uint32_t *)(buf->map + y1 * buf->pitch + px * 4);
                    if (blend) {
                        blend_pixel(top, border_color);
                        blend_pixel(bot, border_color);
                    } else {
                        *top = border_color;
                        *bot = border_color;
                    }
                }
                for (int py = y0; py <= y1; py++) {
                    uint32_t *left = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
                    uint32_t *right = (uint32_t *)(buf->map + py * buf->pitch + x1 * 4);
                    if (blend) {
                        blend_pixel(left, border_color);
                        blend_pixel(right, border_color);
                    } else {
                        *left = border_color;
                        *right = border_color;
                    }
                }
            }
        }
        return;
    }
    
    /* Clamp radius */
    int r = radius;
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    
    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);
    
    /* Build lookup table for this radius */
    circle_lut_t lut;
    build_circle_lut(&lut, r);
    
    /* Fill interior */
    if (fill) {
        for (int row = y0; row < y1; row++) {
            int dy = row - y;
            int dx_start = 0;
            int dx_end = w;
            
            /* Top corners */
            if (dy < r) {
                int corner_dx = lut.x_offsets[r - dy];
                dx_start = r - corner_dx;
                dx_end = w - r + corner_dx;
            }
            /* Bottom corners */
            else if (dy >= h - r) {
                int corner_dy = dy - (h - r);
                int corner_dx = lut.x_offsets[corner_dy];
                dx_start = r - corner_dx;
                dx_end = w - r + corner_dx;
            }
            
            int start_x = clamp(x + dx_start, x0, x1);
            int end_x = clamp(x + dx_end, x0, x1);
            
            uint32_t *line = (uint32_t *)(buf->map + row * buf->pitch + start_x * 4);
            for (int px = start_x; px < end_x; px++, line++) {
                if (blend)
                    blend_pixel(line, fill_color);
                else
                    *line = fill_color;
            }
        }
    }
    
    /* Draw border */
    if (border_width > 0) {
        for (int bw = 0; bw < border_width; bw++) {
            int bx = x + bw;
            int by = y + bw;
            int bw_w = w - 2 * bw;
            int bw_h = h - 2 * bw;
            int br = r - bw;
            if (br < 0) br = 0;
            
            if (br > 0) {
                /* Rebuild LUT for this border radius */
                circle_lut_t bw_lut;
                build_circle_lut(&bw_lut, br);
                
                /* Top and bottom edges with rounded corners */
                for (int dx = br; dx < bw_w - br; dx++) {
                    int px_top = bx + dx;
                    int py_top = by;
                    int px_bot = bx + dx;
                    int py_bot = by + bw_h - 1;
                    
                    if (px_top >= x0 && px_top < x1 && py_top >= y0 && py_top < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_top * buf->pitch + px_top * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                    if (px_bot >= x0 && px_bot < x1 && py_bot >= y0 && py_bot < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_bot * buf->pitch + px_bot * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
                
                /* Left and right edges */
                for (int dy = br; dy < bw_h - br; dy++) {
                    int px_left = bx;
                    int py_left = by + dy;
                    int px_right = bx + bw_w - 1;
                    int py_right = by + dy;
                    
                    if (px_left >= x0 && px_left < x1 && py_left >= y0 && py_left < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_left * buf->pitch + px_left * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                    if (px_right >= x0 && px_right < x1 && py_right >= y0 && py_right < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_right * buf->pitch + px_right * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
                
                /* Four corners using LUT */
                /* Top-left */
                for (int dy = 0; dy <= br; dy++) {
                    int dx = bw_lut.x_offsets[br - dy];
                    int px = bx + br - dx;
                    int py = by + dy;
                    if (px >= x0 && px < x1 && py >= y0 && py < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py * buf->pitch + px * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
                /* Top-right */
                for (int dy = 0; dy <= br; dy++) {
                    int dx = bw_lut.x_offsets[br - dy];
                    int px = bx + bw_w - 1 - br + dx;
                    int py = by + dy;
                    if (px >= x0 && px < x1 && py >= y0 && py < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py * buf->pitch + px * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
                /* Bottom-right */
                for (int dy = 0; dy <= br; dy++) {
                    int dx = bw_lut.x_offsets[br - dy];
                    int px = bx + bw_w - 1 - br + dx;
                    int py = by + bw_h - 1 - dy;
                    if (px >= x0 && px < x1 && py >= y0 && py < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py * buf->pitch + px * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
                /* Bottom-left */
                for (int dy = 0; dy <= br; dy++) {
                    int dx = bw_lut.x_offsets[br - dy];
                    int px = bx + br - dx;
                    int py = by + bw_h - 1 - dy;
                    if (px >= x0 && px < x1 && py >= y0 && py < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py * buf->pitch + px * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
                
                free_circle_lut(&bw_lut);
            } else {
                /* No radius left, draw straight lines */
                for (int dx = 0; dx < bw_w; dx++) {
                    int px_top = bx + dx;
                    int py_top = by;
                    int px_bot = bx + dx;
                    int py_bot = by + bw_h - 1;
                    
                    if (px_top >= x0 && px_top < x1 && py_top >= y0 && py_top < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_top * buf->pitch + px_top * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                    if (px_bot >= x0 && px_bot < x1 && py_bot >= y0 && py_bot < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_bot * buf->pitch + px_bot * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
                for (int dy = 0; dy < bw_h; dy++) {
                    int px_left = bx;
                    int py_left = by + dy;
                    int px_right = bx + bw_w - 1;
                    int py_right = by + dy;
                    
                    if (px_left >= x0 && px_left < x1 && py_left >= y0 && py_left < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_left * buf->pitch + px_left * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                    if (px_right >= x0 && px_right < x1 && py_right >= y0 && py_right < y1) {
                        uint32_t *p = (uint32_t *)(buf->map + py_right * buf->pitch + px_right * 4);
                        if (blend) blend_pixel(p, border_color); else *p = border_color;
                    }
                }
            }
        }
    }
    
    free_circle_lut(&lut);
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
 * Progress Bar Drawing (with rounded corners and alignment)
 * ======================================================================== */

void draw_progress_bar(drm_buffer_t *buf, progress_bar_t *pb) {
    /* Calculate position based on alignment */
    int x = pb->x;
    int y = pb->y;
    
    if (x < 0 || pb->align == ALIGN_CENTER)
        x = (buf->width - pb->w) / 2;
    else if (pb->align == ALIGN_RIGHT)
        x = buf->width - pb->w - x;
    
    if (y < 0 || pb->valign == VALIGN_MIDDLE)
        y = (buf->height - pb->h) / 2;
    else if (pb->valign == VALIGN_BOTTOM)
        y = buf->height - pb->h - y;
    
    int w = pb->w;
    int h = pb->h;
    int r = pb->radius;
    
    /* Clamp radius */
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    
    /* Background (always rounded if radius > 0) */
    if (r > 0) {
        draw_rounded_rect(buf, x, y, w, h, r, pb->bg_color,
                         pb->borderless ? 0 : pb->border_color,
                         pb->borderless ? 0 : pb->border_width,
                         1, 0);
    } else {
        draw_filled_rect(buf, x, y, w, h, pb->bg_color);
        if (!pb->borderless && pb->border_width > 0) {
            /* Draw simple border */
            for (int i = 0; i < pb->border_width; i++) {
                int x0 = x + i;
                int y0 = y + i;
                int x1 = x + w - 1 - i;
                int y1 = y + h - 1 - i;
                for (int px = x0; px <= x1; px++) {
                    uint32_t *top = (uint32_t *)(buf->map + y0 * buf->pitch + px * 4);
                    uint32_t *bot = (uint32_t *)(buf->map + y1 * buf->pitch + px * 4);
                    *top = pb->border_color;
                    *bot = pb->border_color;
                }
                for (int py = y0; py <= y1; py++) {
                    uint32_t *left = (uint32_t *)(buf->map + py * buf->pitch + x0 * 4);
                    uint32_t *right = (uint32_t *)(buf->map + py * buf->pitch + x1 * 4);
                    *left = pb->border_color;
                    *right = pb->border_color;
                }
            }
        }
    }
    
    /* Fill */
    int fill_w = (int)(w * fclamp(pb->value, 0.0f, 1.0f));
    if (fill_w > 0) {
        if (r > 0) {
            /* Draw rounded fill with straight right edge */
            draw_rounded_fill(buf, x, y, fill_w, h, r, pb->bar_color);
        } else {
            draw_filled_rect(buf, x, y, fill_w, h, pb->bar_color);
        }
    }
    
    /* Percent text */
    if (pb->show_percent && pb->value > 0.0f) {
        char percent_str[8];
        snprintf(percent_str, sizeof(percent_str), "%d%%", (int)(pb->value * 100));
        
        text_element_t te = {0};
        te.active = 1;
        strncpy(te.text, percent_str, sizeof(te.text) - 1);
        te.x = x + w / 2;
        te.y = y + h / 2;
        te.align = ALIGN_CENTER;
        te.valign = VALIGN_MIDDLE;
        te.color = pb->text_color;
        te.font_slot = pb->font_slot > 0 ? pb->font_slot : 0;
        te.font_size = pb->font_size > 0 ? pb->font_size : 0;
        draw_text_element(buf, &te);
    }
}

/* ========================================================================
 * Rounded Fill (left side rounded, right side straight)
 * ======================================================================== */

void draw_rounded_fill(drm_buffer_t *buf, int x, int y, int w, int h, int r, uint32_t color) {
    if (r <= 0 || w <= 0) {
        if (w > 0) draw_filled_rect(buf, x, y, w, h, color);
        return;
    }
    
    /* Clamp radius to fill width */
    if (r > w / 2) r = w / 2;
    if (r > h / 2) r = h / 2;
    
    int x0 = clamp(x, 0, (int)buf->width);
    int y0 = clamp(y, 0, (int)buf->height);
    int x1 = clamp(x + w, 0, (int)buf->width);
    int y1 = clamp(y + h, 0, (int)buf->height);
    
    /* Build LUT for this radius */
    circle_lut_t lut;
    build_circle_lut(&lut, r);
    
    /* Fill with rounded left, straight right */
    for (int row = y0; row < y1; row++) {
        int dy = row - y;
        int dx_start = 0;
        int dx_end = w;
        
        /* Top-left corner */
        if (dy < r) {
            int corner_dx = lut.x_offsets[r - dy];
            dx_start = r - corner_dx;
        }
        /* Bottom-left corner */
        else if (dy >= h - r) {
            int corner_dy = dy - (h - r);
            int corner_dx = lut.x_offsets[corner_dy];
            dx_start = r - corner_dx;
        }
        
        int start_x = clamp(x + dx_start, x0, x1);
        int end_x = clamp(x + dx_end, x0, x1);
        
        uint32_t *line = (uint32_t *)(buf->map + row * buf->pitch + start_x * 4);
        for (int px = start_x; px < end_x; px++, line++) {
            *line = color;
        }
    }
    
    free_circle_lut(&lut);
}

/* ========================================================================
 * Rectangle Element Drawing
 * ======================================================================== */

void draw_rect_element(drm_buffer_t *buf, rect_element_t *re) {
    if (re->radius > 0 || re->fill || re->border_width > 0) {
        draw_rounded_rect(buf, re->x, re->y, re->w, re->h, re->radius,
                          re->color, re->border_color, re->border_width,
                          re->fill, re->blend);
    } else {
        /* Legacy: simple filled rectangle */
        if (re->blend)
            draw_rect_blend(buf, re->x, re->y, re->w, re->h, re->color);
        else
            draw_filled_rect(buf, re->x, re->y, re->w, re->h, re->color);
    }
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
