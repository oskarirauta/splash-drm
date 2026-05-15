/*
 * splash.h - Main header for splash-drm bootsplash daemon
 */

#ifndef SPLASH_H
#define SPLASH_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <time.h>
#include <math.h>

#include <libdrm/drm.h>
#include <xf86drmMode.h>

/* ========================================================================
 * Build Configuration
 * ======================================================================== */

#define SPLASH_VERSION "2.0.0"
#define MAX_TEXT_ELEMENTS 16
#define MAX_IMAGE_OVERLAYS 16
#define MAX_PROGRESS_BARS 8
#define MAX_RECTANGLES 16
#define CMD_MAX_LEN 4096
#define MAX_FONT_SIZE 72
#define PIPE_TIMEOUT_MS 100
#define RENDER_FPS 30

/* ========================================================================
 * Image Scaling Modes
 * ======================================================================== */

#define SCALE_COVER 0
#define SCALE_CONTAIN 1
#define SCALE_STRETCH 2
#define SCALE_NONE 3

/* ========================================================================
 * Alignment Constants
 * ======================================================================== */

#define ALIGN_LEFT 0
#define ALIGN_CENTER 1
#define ALIGN_RIGHT 2

#define VALIGN_TOP 0
#define VALIGN_MIDDLE 1
#define VALIGN_BOTTOM 2

/* ========================================================================
 * Color Utilities (inline for performance)
 * ======================================================================== */

static inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return argb(255, r, g, b);
}

/* Parse #RGB, #RRGGBB, #RRGGBBAA colors */
uint32_t parse_color(const char *str);

/* ========================================================================
 * Pixel Blending (used by both render.c and font.c)
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
 * DRM/KMS Types
 * ======================================================================== */

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t fb_id;
    uint64_t size;
    uint8_t *map;
} drm_buffer_t;

/* Renamed to avoid collision with libdrm's drm_context_t */
typedef struct {
    int fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    drmModeModeInfo mode;
    drm_buffer_t buf[2];
    int front_buf;
    struct {
        uint32_t crtc_id;
        uint32_t buffer_id;
        uint32_t x, y;
        drmModeModeInfo mode;
        int mode_valid;
    } saved_crtc;
} splash_drm_t;

/* ========================================================================
 * Image Type
 * ======================================================================== */

typedef struct {
    uint8_t *rgba;
    int w, h;
} image_t;

int load_image(const char *path, image_t *img);
void free_image(image_t *img);

/* ========================================================================
 * Font Type
 * ======================================================================== */

typedef struct {
    void *info;
    uint8_t *data;
    size_t data_size;
    float scale;
    int baseline;
    int ascent;
    int descent;
    int line_gap;
    float pixel_height;
    int loaded;
} font_t;

int font_load(const char *path, float pixel_height);
void font_unload(void);
int font_is_loaded(void);
int text_width(const char *text);
int text_height(void);

/* ========================================================================
 * Element Types
 * ======================================================================== */

typedef struct {
    int active;
    int id;
    char text[256];
    int x, y;
    int align;
    uint32_t color;
} text_element_t;

typedef struct {
    int active;
    int id;
    image_t img;
    int x, y;
    int w, h;
    int align;
    int valign;
} image_overlay_t;

typedef struct {
    int active;
    int id;
    int x, y, w, h;
    uint32_t color;
    int blend;
} rect_element_t;

typedef struct {
    int active;
    int id;
    int x, y, w, h;
    int style;
    char prefix[128];
    char suffix[16];
    char inner[128];
    float value;
    uint32_t bg_color;
    uint32_t fg_color;
    uint32_t border_color;
    uint32_t text_color;
} progress_bar_t;

/* ========================================================================
 * Main State
 * ======================================================================== */

typedef struct {
    splash_drm_t drm;
    image_t bg_image;
    int bg_loaded;
    int bg_scale_mode;
    text_element_t texts[MAX_TEXT_ELEMENTS];
    image_overlay_t overlays[MAX_IMAGE_OVERLAYS];
    rect_element_t rects[MAX_RECTANGLES];
    progress_bar_t bars[MAX_PROGRESS_BARS];
    int pipe_fd;
    char pipe_path[256];
    int running;
    int needs_render;
    int ready;
} splash_state_t;

/* ========================================================================
 * Function Prototypes
 * ======================================================================== */

/* drm.c */
int drm_init(splash_drm_t *ctx, const char *device);
void drm_cleanup(splash_drm_t *ctx);
void drm_flip(splash_drm_t *ctx);

/* render.c */
void render_frame(splash_state_t *st);
void draw_filled_rect(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color);
void draw_rect_blend(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color);
void draw_image(drm_buffer_t *buf, int x, int y, int w, int h,
    const uint8_t *rgba, int img_w, int img_h);
void draw_text_element(drm_buffer_t *buf, text_element_t *te);
void draw_progress_bar(drm_buffer_t *buf, progress_bar_t *pb);
void draw_rect_element(drm_buffer_t *buf, rect_element_t *re);
void calculate_scaled_rect(int buf_w, int buf_h, int img_w, int img_h,
    int mode, int *out_x, int *out_y, int *out_w, int *out_h);

/* cmd.c */
int handle_command(splash_state_t *st, const char *cmdline);

/* pipe.c */
int pipe_create(const char *path);
int pipe_reopen(splash_state_t *st, const char *new_path);
int pipe_read_command(splash_state_t *st, char *buf, int max_len);

/* elements.c */
text_element_t* text_find(splash_state_t *st, int id);
text_element_t* text_alloc(splash_state_t *st);
image_overlay_t* overlay_find(splash_state_t *st, int id);
image_overlay_t* overlay_alloc(splash_state_t *st);
rect_element_t* rect_find(splash_state_t *st, int id);
rect_element_t* rect_alloc(splash_state_t *st);
void clear_all_elements(splash_state_t *st);

/* utils.c */
void set_default_progress_colors(progress_bar_t *pb, int style);
int clamp(int val, int min, int max);
float fclamp(float val, float min, float max);

#endif /* SPLASH_H */
