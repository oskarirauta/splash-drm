/*
 * splash.h - Main header for splash-drm bootsplash daemon
 */

#ifndef SPLASH_H
#define SPLASH_H

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <poll.h>
#include <time.h>
#include <math.h>
#include <libdrm/drm.h>
#include <xf86drmMode.h>

#include "cJSON.h"

/* ========================================================================
 * Build Configuration
 * ======================================================================== */

#define SPLASH_VERSION "3.0.0"
#define MAX_TEXT_ELEMENTS 32
#define MAX_IMAGE_OVERLAYS 16
#define MAX_PROGRESS_BARS 8
#define MAX_RECTANGLES 16
#define MAX_FONTS 5
#define CMD_MAX_LEN 8192
#define MAX_FONT_SIZE 128
#define RENDER_FPS 30
#define MAX_SOCKET_CLIENTS 4

#define SOCKET_NAME "\0splash-drm"

/* ========================================================================
 * Image Scaling Modes
 * ======================================================================== */

#define SCALE_COVER 0
#define SCALE_CONTAIN 1
#define SCALE_STRETCH 2
#define SCALE_NONE 3
#define SCALE_CUSTOM 4

/* ========================================================================
 * Image Filtering (resample quality)
 * ======================================================================== */

#define IMG_NEAREST  0
#define IMG_BILINEAR 1
#define IMG_BICUBIC  2   /* Mitchell-Netravali */
#define IMG_LANCZOS  3   /* Lanczos-3 (default) */

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
 * Gradient Directions
 * ======================================================================== */

#define GRAD_NONE       0
#define GRAD_VERTICAL   1
#define GRAD_HORIZONTAL 2
#define GRAD_DIAGONAL   3   /* top-left -> bottom-right */

/* ========================================================================
 * Color Utilities
 * ======================================================================== */

static inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return argb(255, r, g, b);
}

uint32_t parse_color(const char *str);

/* ========================================================================
 * Pixel Blending
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

/* A fill paint: a solid colour, or a 2-stop linear gradient. */
typedef struct {
    uint32_t color0;     /* solid colour, or gradient start */
    uint32_t color1;     /* gradient end (ignored when gradient == GRAD_NONE) */
    int      gradient;   /* GRAD_NONE / GRAD_VERTICAL / GRAD_HORIZONTAL / GRAD_DIAGONAL */
} paint_t;

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
    char path[256];
} font_t;

int font_load(const char *path, float pixel_height, int slot);
void font_unload(int slot);
void font_unload_all(void);
int font_is_loaded(int slot);
int font_count_loaded(void);
int text_width_font(const char *text, int font_slot);
int text_height_font(int font_slot);
int text_baseline_font(int font_slot);
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
    int valign;
    uint32_t color;
    int font_slot;
    float font_size;

    /* Drop shadow (optional) */
    int      shadow;
    int      shadow_dx, shadow_dy;
    int      shadow_blur;
    uint32_t shadow_color;
} text_element_t;

typedef struct {
    int active;
    int id;
    image_t img;
    int x, y;
    int w, h;
    int align;
    int valign;
    int filter;
} image_overlay_t;

typedef struct {
    int active;
    int id;
    int x, y, w, h;
    uint32_t color;
    int blend;
    int fill;
    int radius;
    uint32_t border_color;
    int border_width;

    /* Gradient fill (optional) */
    uint32_t grad_color;     /* second gradient stop; first stop is `color` */
    int      grad_dir;       /* GRAD_NONE = solid */

    /* Drop shadow (optional) */
    int      shadow;         /* enabled */
    int      shadow_dx, shadow_dy;
    int      shadow_blur;
    uint32_t shadow_color;
} rect_element_t;

typedef struct {
    int active;
    int id;
    int x, y, w, h;
    int align;
    int valign;
    int style;
    float value;

    /* Custom colors (used when style == -1 or override) */
    uint32_t bg_color;      /* Background (empty) color */
    uint32_t bar_color;     /* Fill color */
    uint32_t border_color;  /* Border color */
    uint32_t text_color;    /* Percent text color */

    /* Gradient for the fill (optional) */
    uint32_t bar_color2;     /* second gradient stop; first stop is `bar_color` */
    int      bar_gradient;   /* GRAD_NONE = solid */

    /* Drop shadow of the whole bar (optional) */
    int      shadow;
    int      shadow_dx, shadow_dy;
    int      shadow_blur;
    uint32_t shadow_color;

    int borderless;         /* No border */
    int border_width;       /* Border thickness */
    int radius;             /* Corner radius */

    int font_slot;
    float font_size;
    int show_percent;
} progress_bar_t;

/* ========================================================================
 * Main State
 * ======================================================================== */

typedef struct {
    splash_drm_t drm;
    image_t bg_image;
    int bg_loaded;
    int bg_scale_mode;
    float bg_custom_scale;
    int bg_filter;          /* resample quality for the background image */
    uint32_t bg_color;

    text_element_t texts[MAX_TEXT_ELEMENTS];
    image_overlay_t overlays[MAX_IMAGE_OVERLAYS];
    rect_element_t rects[MAX_RECTANGLES];
    progress_bar_t bars[MAX_PROGRESS_BARS];
    
    int server_fd;
    int client_fds[MAX_SOCKET_CLIENTS];
    size_t client_cmd_len[MAX_SOCKET_CLIENTS];
    char client_cmd_buf[MAX_SOCKET_CLIENTS][CMD_MAX_LEN];
    
    int running;
    int needs_render;
    int ready;
    int frozen;
    int quiet;
    int debug;
} splash_state_t;

/* ========================================================================
 * Function Prototypes - NÄMÄ splash_state_t:n JÄLKEEN!
 * ======================================================================== */

/* drm.c */
int drm_init(splash_drm_t *ctx, const char *device);
void drm_cleanup(splash_drm_t *ctx);
void drm_flip(splash_drm_t *ctx);

/* render.c */
void render_frame(splash_state_t *st);
void draw_filled_rect(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color);
void draw_rect_blend(drm_buffer_t *buf, int x, int y, int w, int h, uint32_t color);
void draw_round_rect(drm_buffer_t *buf, float x, float y, float w, float h,
                     float radius, const paint_t *paint);
void draw_round_rect_outline(drm_buffer_t *buf, float x, float y, float w, float h,
                             float radius, float border_width, uint32_t color);
void draw_round_rect_progress(drm_buffer_t *buf, float x, float y, float w, float h,
                              float radius, float fill_w, const paint_t *paint);
void draw_round_rect_shadow(drm_buffer_t *buf, float x, float y, float w, float h,
                            float radius, float blur, uint32_t color);
void draw_image(drm_buffer_t *buf, int x, int y, int w, int h,
    const uint8_t *rgba, int img_w, int img_h, int filter);
void draw_text_element(drm_buffer_t *buf, text_element_t *te);
void draw_progress_bar(drm_buffer_t *buf, progress_bar_t *pb);
void draw_rect_element(drm_buffer_t *buf, rect_element_t *re);
void calculate_scaled_rect(int buf_w, int buf_h, int img_w, int img_h,
    int mode, float custom_scale, int *out_x, int *out_y, int *out_w, int *out_h);

/* cmd.c */
int handle_json_command(splash_state_t *st, const char *json_str, int client_idx);
cJSON* create_response(const char *status, const char *message);
int process_json_batch(splash_state_t *st, cJSON *root, int client_idx);

/* socket.c */
int socket_init(splash_state_t *st);
void socket_cleanup(splash_state_t *st);
int socket_poll(splash_state_t *st, struct pollfd *fds, int *nfds);
void socket_process(splash_state_t *st, struct pollfd *fds, int nfds);
void socket_reply_json(splash_state_t *st, int client_idx, cJSON *response);

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

/* main.c helpers */
int load_config(splash_state_t *st, const char *config_str);
int process_startup_cmds(splash_state_t *st, const char *cmds_str);

/* JSON helpers (defined in cmd.c but used in main.c) */
int get_int(cJSON *obj, const char *key, int default_val);
float get_float(cJSON *obj, const char *key, float default_val);
const char* get_string(cJSON *obj, const char *key, const char *default_val);

#endif /* SPLASH_H */
