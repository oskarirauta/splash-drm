/*
 * font.c - Minimal stb_truetype font rendering
 */

#include "splash.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static font_t g_fonts[MAX_FONTS];

/* ========================================================================
 * Font Management
 * ======================================================================== */

int font_load(const char *path, float pixel_height, int slot) {
    if (slot < 0 || slot >= MAX_FONTS) return -1;
    if (pixel_height < 8.0f || pixel_height > MAX_FONT_SIZE) return -1;

    font_unload(slot);

    FILE *fp = fopen(path, "rb");
    if (!fp) return -1;

    fseek(fp, 0, SEEK_END);
    long size = ftell(fp);
    fseek(fp, 0, SEEK_SET);

    uint8_t *data = malloc(size);
    if (!data || fread(data, 1, size, fp) != (size_t)size) {
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

    g_fonts[slot].info = info;
    g_fonts[slot].data = data;
    g_fonts[slot].data_size = size;
    g_fonts[slot].pixel_height = pixel_height;
    g_fonts[slot].scale = stbtt_ScaleForPixelHeight(info, pixel_height);

    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
    g_fonts[slot].ascent = (int)(ascent * g_fonts[slot].scale);
    g_fonts[slot].descent = (int)(descent * g_fonts[slot].scale);
    g_fonts[slot].line_gap = (int)(line_gap * g_fonts[slot].scale);
    g_fonts[slot].baseline = g_fonts[slot].ascent;
    g_fonts[slot].loaded = 1;
    strncpy(g_fonts[slot].path, path, sizeof(g_fonts[slot].path) - 1);

    return 0;
}

void font_unload(int slot) {
    if (slot < 0 || slot >= MAX_FONTS) return;
    if (g_fonts[slot].info) free(g_fonts[slot].info);
    if (g_fonts[slot].data) free(g_fonts[slot].data);
    memset(&g_fonts[slot], 0, sizeof(font_t));
}

void font_unload_all(void) {
    for (int i = 0; i < MAX_FONTS; i++)
        font_unload(i);
}

int font_is_loaded(int slot) {
    if (slot < 0 || slot >= MAX_FONTS) return 0;
    return g_fonts[slot].loaded;
}

int font_count_loaded(void) {
    int count = 0;
    for (int i = 0; i < MAX_FONTS; i++)
        if (g_fonts[i].loaded) count++;
    return count;
}

/* ========================================================================
 * Text Measurement
 * ======================================================================== */

static font_t* get_font(int slot) {
    if (slot < 0 || slot >= MAX_FONTS || !g_fonts[slot].loaded) {
        /* Fallback to slot 0 if available */
        if (g_fonts[0].loaded) return &g_fonts[0];
        return NULL;
    }
    return &g_fonts[slot];
}

int text_width_font(const char *text, int font_slot) {
    font_t *f = get_font(font_slot);
    if (!f || !text) return 0;

    stbtt_fontinfo *info = f->info;
    float scale = f->scale;
    int x = 0;

    for (const char *p = text; *p; p++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, *p, &advance, &lsb);
        x += (int)(advance * scale);
    }
    return x;
}

int text_height_font(int font_slot) {
    font_t *f = get_font(font_slot);
    if (!f) return 0;
    return f->ascent - f->descent;
}

int text_baseline_font(int font_slot) {
    font_t *f = get_font(font_slot);
    if (!f) return 0;
    return f->baseline;
}

/* Backwards compatibility */
int text_width(const char *text) { return text_width_font(text, 0); }
int text_height(void) { return text_height_font(0); }

/* ========================================================================
 * Glyph Rendering
 * ======================================================================== */

static void draw_glyph(drm_buffer_t *buf, int x0, int y0, int gw, int gh,
                       uint8_t *bitmap, uint32_t color) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    for (int y = 0; y < gh; y++) {
        int py = y0 + y;
        if (py < 0 || py >= (int)buf->height) continue;
        for (int x = 0; x < gw; x++) {
            int px = x0 + x;
            if (px < 0 || px >= (int)buf->width) continue;
            uint8_t a = bitmap[y * gw + x];
            if (a == 0) continue;
            uint32_t *pixel = (uint32_t *)(buf->map + py * buf->pitch + px * 4);
            blend_pixel(pixel, argb(a, r, g, b));
        }
    }
}

void draw_text_element(drm_buffer_t *buf, text_element_t *te) {
    font_t *f = get_font(te->font_slot);
    if (!f || !te->text[0]) return;

    stbtt_fontinfo *info = f->info;
    float scale = f->scale;
    if (te->font_size > 0) {
        scale = stbtt_ScaleForPixelHeight(info, te->font_size);
    }

    int tw = 0;
    for (const char *p = te->text; *p; p++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, *p, &advance, &lsb);
        tw += (int)(advance * scale);
    }

    int th = f->ascent - f->descent;
    if (te->font_size > 0) {
        int ascent, descent, line_gap;
        stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
        int fh_ascent = (int)(ascent * scale);
        int fh_descent = (int)(descent * scale);
        th = fh_ascent - fh_descent;
    }

    int x = te->x;
    int y = te->y + f->baseline;
    if (te->font_size > 0) {
        int ascent, descent, line_gap;
        stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
        y = te->y + (int)(ascent * scale);
    }

    if (te->align == ALIGN_CENTER)
        x -= tw / 2;
    else if (te->align == ALIGN_RIGHT)
        x -= tw;

    for (const char *p = te->text; *p; p++) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, *p, &advance, &lsb);

        int c_x1, c_y1, c_x2, c_y2;
        stbtt_GetCodepointBitmapBox(info, *p, scale, scale, &c_x1, &c_y1, &c_x2, &c_y2);

        int gw = c_x2 - c_x1;
        int gh = c_y2 - c_y1;
        if (gw > 0 && gh > 0) {
            uint8_t *bitmap = stbtt_GetCodepointBitmap(info, scale, scale, *p, NULL, NULL, NULL, NULL);
            if (bitmap) {
                draw_glyph(buf, x + c_x1, y + c_y1, gw, gh, bitmap, te->color);
                stbtt_FreeBitmap(bitmap, NULL);
            }
        }
        x += (int)(advance * scale);
    }
}
