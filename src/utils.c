/*
 * utils.c - Utility functions for splash-drm
 */

#include "splash.h"

/* ========================================================================
 * Color Parsing
 * ======================================================================== */

uint32_t parse_color(const char *str) {
    if (!str || str[0] != '#') return argb(255, 255, 255, 255);
    
    unsigned int r = 255, g = 255, b = 255, a = 255;
    int len = strlen(str);
    
    if (len == 7) { /* #RRGGBB */
        sscanf(str, "#%02x%02x%02x", &r, &g, &b);
    } else if (len == 9) { /* #RRGGBBAA */
        sscanf(str, "#%02x%02x%02x%02x", &r, &g, &b, &a);
    } else if (len == 4) { /* #RGB */
        sscanf(str, "#%1x%1x%1x", &r, &g, &b);
        r *= 17; g *= 17; b *= 17;
    }
    
    return argb((uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* ========================================================================
 * Progress Bar Color Themes
 * ======================================================================== */

void set_default_progress_colors(progress_bar_t *pb, int style) {
    switch (style) {
        case 0: /* Modern blue */
            pb->bg_color = argb(255, 30, 30, 30);
            pb->fg_color = argb(255, 0, 150, 255);
            pb->border_color = argb(255, 60, 60, 60);
            pb->text_color = argb(255, 255, 255, 255);
            break;
        case 1: /* Green success */
            pb->bg_color = argb(255, 30, 30, 30);
            pb->fg_color = argb(255, 0, 200, 100);
            pb->border_color = argb(255, 60, 60, 60);
            pb->text_color = argb(255, 255, 255, 255);
            break;
        case 2: /* Amber warning */
            pb->bg_color = argb(255, 30, 30, 30);
            pb->fg_color = argb(255, 255, 180, 0);
            pb->border_color = argb(255, 60, 60, 60);
            pb->text_color = argb(255, 255, 255, 255);
            break;
        case 3: /* Red error */
            pb->bg_color = argb(255, 30, 30, 30);
            pb->fg_color = argb(255, 255, 60, 60);
            pb->border_color = argb(255, 60, 60, 60);
            pb->text_color = argb(255, 255, 255, 255);
            break;
        case 4: /* Purple accent */
            pb->bg_color = argb(255, 30, 30, 30);
            pb->fg_color = argb(255, 150, 50, 255);
            pb->border_color = argb(255, 60, 60, 60);
            pb->text_color = argb(255, 255, 255, 255);
            break;
        case 5: /* Cyan cool */
            pb->bg_color = argb(255, 30, 30, 30);
            pb->fg_color = argb(255, 0, 200, 200);
            pb->border_color = argb(255, 60, 60, 60);
            pb->text_color = argb(255, 255, 255, 255);
            break;
        default:
            pb->bg_color = argb(255, 40, 40, 40);
            pb->fg_color = argb(255, 0, 150, 255);
            pb->border_color = argb(255, 80, 80, 80);
            pb->text_color = argb(255, 255, 255, 255);
            break;
    }
}

/* ========================================================================
 * Clamp Functions
 * ======================================================================== */

int clamp(int val, int min, int max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}

float fclamp(float val, float min, float max) {
    if (val < min) return min;
    if (val > max) return max;
    return val;
}
