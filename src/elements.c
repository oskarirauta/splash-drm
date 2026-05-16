/*
 * elements.c - Element management
 *
 * Provides allocation, lookup, and cleanup for all visual elements.
 */

#include "splash.h"

/* ========================================================================
 * Text Elements
 * ======================================================================== */

text_element_t* text_find(splash_state_t *st, int id) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
        if (st->texts[i].active && st->texts[i].id == id)
            return &st->texts[i];
    }
    return NULL;
}

text_element_t* text_alloc(splash_state_t *st) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
        if (!st->texts[i].active) {
            memset(&st->texts[i], 0, sizeof(text_element_t));
            st->texts[i].active = 1;
            st->texts[i].opacity = 1.0f;
            return &st->texts[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * Image Overlays
 * ======================================================================== */

image_overlay_t* overlay_find(splash_state_t *st, int id) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
        if (st->overlays[i].active && st->overlays[i].id == id)
            return &st->overlays[i];
    }
    return NULL;
}

image_overlay_t* overlay_alloc(splash_state_t *st) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
        if (!st->overlays[i].active) {
            memset(&st->overlays[i], 0, sizeof(image_overlay_t));
            st->overlays[i].active = 1;
            st->overlays[i].opacity = 1.0f;
            return &st->overlays[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * Rectangle Elements
 * ======================================================================== */

rect_element_t* rect_find(splash_state_t *st, int id) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_RECTANGLES; i++) {
        if (st->rects[i].active && st->rects[i].id == id)
            return &st->rects[i];
    }
    return NULL;
}

rect_element_t* rect_alloc(splash_state_t *st) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_RECTANGLES; i++) {
        if (!st->rects[i].active) {
            memset(&st->rects[i], 0, sizeof(rect_element_t));
            st->rects[i].active = 1;
            st->rects[i].opacity = 1.0f;
            return &st->rects[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * Spinners
 * ======================================================================== */

spinner_t* spinner_find(splash_state_t *st, int id) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_SPINNERS; i++) {
        if (st->spinners[i].used && st->spinners[i].id == id)
            return &st->spinners[i];
    }
    return NULL;
}

spinner_t* spinner_alloc(splash_state_t *st) {
    if (!st) return NULL;
    for (int i = 0; i < MAX_SPINNERS; i++) {
        if (!st->spinners[i].used) {
            memset(&st->spinners[i], 0, sizeof(spinner_t));
            st->spinners[i].used    = 1;
            st->spinners[i].active  = 1;
            st->spinners[i].opacity = 1.0f;
            return &st->spinners[i];
        }
    }
    return NULL;
}

/* ========================================================================
 * Global Cleanup
 * ======================================================================== */

void clear_all_elements(splash_state_t *st) {
    if (!st) return;

    /* Clear text elements */
    for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
        st->texts[i].active = 0;
    }

    /* Clear image overlays */
    for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
        free_image(&st->overlays[i].img);
        st->overlays[i].active = 0;
    }

    /* Clear rectangles */
    for (int i = 0; i < MAX_RECTANGLES; i++) {
        st->rects[i].active = 0;
    }

    /* Clear progress bars */
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        st->bars[i].active = 0;
    }

    /* Clear spinners */
    for (int i = 0; i < MAX_SPINNERS; i++) {
        st->spinners[i].active = 0;
        st->spinners[i].used   = 0;
    }

    /* Clear background (and any in-flight crossfade) */
    st->bg_loaded = 0;
    free_image(&st->bg_image);
    free_image(&st->bg_prev);
    st->bg_prev_loaded = 0;
    st->bg_anim.active = 0;
    st->bg_opacity = 1.0f;
}
