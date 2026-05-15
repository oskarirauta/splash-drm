/*
 * splash.h - Main header for splash-drm bootsplash daemon
 * 
 * This is the central header that defines all types, constants,
 * and function prototypes used across the project.
 */

#ifndef SPLASH_H
#define SPLASH_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <sys/time.h>
#include <math.h>
#include <linux/drm.h>
#include <linux/drm_mode.h>

/* ========================================================================
 * Build Configuration
 * ======================================================================== */

#define SPLASH_VERSION     "2.0.0"
#define MAX_TEXT_ELEMENTS  16
#define MAX_IMAGE_OVERLAYS 16
#define MAX_PROGRESS_BARS   8
#define MAX_RECTANGLES     16
#define CMD_MAX_LEN       4096
#define MAX_FONT_SIZE      72
#define PIPE_TIMEOUT_MS   100
#define RENDER_FPS         30

/* ========================================================================
 * Image Scaling Modes
 * ======================================================================== */

#define SCALE_COVER   0
#define SCALE_CONTAIN 1
#define SCALE_STRETCH 2
#define SCALE_NONE    3

/* ========================================================================
 * Alignment Constants
 * ======================================================================== */

#define ALIGN_LEFT    0
#define ALIGN_CENTER  1
#define ALIGN_RIGHT   2

#define VALIGN_TOP    0
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
 * DRM/KMS Types (minimal reimplementation of libdrm types)
 * ======================================================================== */

typedef struct {
    uint32_t clock;
    uint16_t hdisplay, hsync_start, hsync_end, htotal, hskew;
    uint16_t vdisplay, vsync_start, vsync_end, vtotal, vscan;
    uint32_t vrefresh;
    uint32_t flags;
    uint32_t type;
    char name[32];
} drm_mode_info_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t handle;
    uint32_t fb_id;
    uint64_t size;
    uint8_t *map;
} drm_buffer_t;

typedef struct {
    int fd;
    uint32_t conn_id;
    uint32_t crtc_id;
    drm_mode_info_t mode;
    drm_buffer_t buf[2];
    int front_buf;
    struct {
        uint32_t crtc_id;
        uint32_t buffer_id;
        uint32_t x, y;
        drm_mode_info_t mode;
        int mode_valid;
    } saved_crtc;
} drm_context_t;

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
    void *info;        /* stbtt_fontinfo (opaque to avoid header dependency) */
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
int text_width(const char *text);

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
    int blend;  /* 0 = replace, 1 = alpha blend */
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
    drm_context_t drm;
    
    /* Background */
    image_t bg_image;
    int bg_loaded;
    int bg_scale_mode;
    
    /* Elements */
    text_element_t texts[MAX_TEXT_ELEMENTS];
    image_overlay_t overlays[MAX_IMAGE_OVERLAYS];
    rect_element_t rects[MAX_RECTANGLES];
    progress_bar_t bars[MAX_PROGRESS_BARS];
    
    /* Pipe */
    int pipe_fd;
    char pipe_path[256];
    int running;
    int needs_render;
    
    /* Debug */
    int ready;  /* Set to 1 after first successful render */
} splash_state_t;

/* ========================================================================
 * Function Prototypes - Organized by Module
 * ======================================================================== */

/* drm.c - DRM/KMS interface */
int drm_init(drm_context_t *ctx, const char *device);
void drm_cleanup(drm_context_t *ctx);
void drm_flip(drm_context_t *ctx);

/* render.c - Graphics rendering */
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

/* cmd.c - Command parsing and handling */
int handle_command(splash_state_t *st, const char *cmdline);

/* pipe.c - Named pipe management */
int pipe_create(const char *path);
int pipe_reopen(splash_state_t *st, const char *new_path);
int pipe_read_command(splash_state_t *st, char *buf, int max_len);

/* elements.c - Element management */
text_element_t* text_find(splash_state_t *st, int id);
text_element_t* text_alloc(splash_state_t *st);
image_overlay_t* overlay_find(splash_state_t *st, int id);
image_overlay_t* overlay_alloc(splash_state_t *st);
rect_element_t* rect_find(splash_state_t *st, int id);
rect_element_t* rect_alloc(splash_state_t *st);
void clear_all_elements(splash_state_t *st);

/* utils.c - Utility functions */
void set_default_progress_colors(progress_bar_t *pb, int style);
int clamp(int val, int min, int max);
float fclamp(float val, float min, float max);

#endif /* SPLASH_H */
