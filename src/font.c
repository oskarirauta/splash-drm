/*
 * font.c - Font rendering using stb_truetype
 * 
 * Single-file, public domain TrueType rasterizer.
 */

#include "splash.h"

#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

static font_t g_font = {0};

/* ========================================================================
 * Font Loading
 * ======================================================================== */

int font_load(const char *path, float pixel_height) {
    if (g_font.loaded) {
        font_unload();
    }
    
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    
    off_t sz = lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    
    g_font.data = malloc(sz);
    if (!g_font.data) { close(fd); return -1; }
    
    if (read(fd, g_font.data, sz) != sz) {
        free(g_font.data);
        close(fd);
        return -1;
    }
    close(fd);
    
    stbtt_fontinfo *info = malloc(sizeof(stbtt_fontinfo));
    if (!info) {
        free(g_font.data);
        g_font.data = NULL;
        return -1;
    }
    
    if (!stbtt_InitFont(info, g_font.data, 0)) {
        free(info);
        free(g_font.data);
        g_font.data = NULL;
        return -1;
    }
    
    g_font.info = info;
    g_font.pixel_height = pixel_height;
    g_font.scale = stbtt_ScaleForPixelHeight(info, pixel_height);
    
    int ascent, descent, line_gap;
    stbtt_GetFontVMetrics(info, &ascent, &descent, &line_gap);
    g_font.ascent = (int)(ascent * g_font.scale);
    g_font.descent = (int)(descent * g_font.scale);
    g_font.line_gap = (int)(line_gap * g_font.scale);
    g_font.baseline = g_font.ascent;
    g_font.data_size = sz;
    g_font.loaded = 1;
    
    return 0;
}

void font_unload(void) {
    if (!g_font.loaded) return;
    
    free(g_font.info);
    g_font.info = NULL;
    
    free(g_font.data);
    g_font.data = NULL;
    
    memset(&g_font, 0, sizeof(g_font));
}

int font_is_loaded(void) {
    return g_font.loaded;
}

/* ========================================================================
 * Text Measurement
 * ======================================================================== */

int text_width(const char *text) {
    if (!g_font.loaded || !text || !*text) return 0;
    
    stbtt_fontinfo *info = (stbtt_fontinfo*)g_font.info;
    int x = 0;
    
    while (*text) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, *text, &advance, &lsb);
        x += (int)(advance * g_font.scale);
        text++;
    }
    return x;
}

int text_height(void) {
    if (!g_font.loaded) return 0;
    return g_font.ascent - g_font.descent;
}

/* ========================================================================
 * Text Rendering
 * ======================================================================== */

void draw_text_element(drm_buffer_t *buf, text_element_t *te) {
    if (!g_font.loaded || !te || !te->active || !te->text[0]) return;
    
    stbtt_fontinfo *info = (stbtt_fontinfo*)g_font.info;
    
    int w = text_width(te->text);
    int draw_x = te->x;
    if (te->align == ALIGN_CENTER) draw_x = te->x - w / 2;
    else if (te->align == ALIGN_RIGHT) draw_x = te->x - w;
    
    int cx = draw_x;
    const char *text = te->text;
    
    while (*text) {
        int advance, lsb;
        stbtt_GetCodepointHMetrics(info, *text, &advance, &lsb);
        
        int gw, gh, xoff, yoff;
        uint8_t *bitmap = stbtt_GetCodepointBitmap(info, g_font.scale, g_font.scale,
                                                    *text, &gw, &gh, &xoff, &yoff);
        
        if (bitmap) {
            int bx = cx + xoff;
            int by = te->y + g_font.baseline + yoff;
            
            for (int row = 0; row < gh; row++) {
                int py = by + row;
                if (py < 0 || py >= (int)buf->height) continue;
                uint32_t *line = (uint32_t*)(buf->map + py * buf->pitch);
                
                for (int col = 0; col < gw; col++) {
                    int px = bx + col;
                    if (px < 0 || px >= (int)buf->width) continue;
                    
                    uint8_t alpha = bitmap[row * gw + col];
                    if (alpha) {
                        uint8_t r = (te->color >> 16) & 0xFF;
                        uint8_t g = (te->color >> 8) & 0xFF;
                        uint8_t b = te->color & 0xFF;
                        uint32_t c = argb((alpha * (te->color >> 24)) >> 8, r, g, b);
                        blend_pixel(&line[px], c);
                    }
                }
            }
            stbtt_FreeBitmap(bitmap, NULL);
        }
        
        cx += (int)(advance * g_font.scale);
        text++;
    }
}
