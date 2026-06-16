/*
 * splash.h - Shared declarations for the splash-drm bootsplash daemon:
 *            build configuration, colour helpers, the DRM and element
 *            types, the global splash_state_t, and every cross-file
 *            function prototype.
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
#include <limits.h>
#include <libdrm/drm.h>
#include <xf86drmMode.h>

#include "cJSON/cJSON.h"

/* ========================================================================
 * Build Configuration
 * ======================================================================== */

#define SPLASH_VERSION      "3.5.0"
#define MAX_TEXT_ELEMENTS   32
#define MAX_IMAGE_OVERLAYS  16
#define MAX_PROGRESS_BARS   8
#define MAX_RECTANGLES      16
#define MAX_FONTS           5
#define MAX_SPINNERS        4
#define MAX_CONSOLES        2
#define CONSOLE_MAX_LINES   64
#define CONSOLE_LINE_LEN    256
#define MAX_ARC_BARS        8
#define MAX_QR_ELEMENTS     4
#define CMD_MAX_LEN         8192
#define MAX_FONT_SIZE       128
#define RENDER_FPS          30
#define MAX_SOCKET_CLIENTS  4

/* Abstract-socket name: a leading NUL keeps it out of the filesystem. */
#define SOCKET_NAME "\0splash-drm"

/* ========================================================================
 * Image Scaling Modes
 * ======================================================================== */

#define SCALE_COVER    0
#define SCALE_CONTAIN  1
#define SCALE_STRETCH  2
#define SCALE_NONE     3
#define SCALE_CUSTOM   4

/* ========================================================================
 * Image Filtering (resample quality)
 * ======================================================================== */

#define IMG_NEAREST   0
#define IMG_BILINEAR  1
#define IMG_BICUBIC   2   /* Mitchell-Netravali */
#define IMG_LANCZOS   3   /* Lanczos-3 (default) */

/* ========================================================================
 * Easing Curves
 * ======================================================================== */

#define EASE_LINEAR   0
#define EASE_IN       1
#define EASE_OUT      2
#define EASE_IN_OUT   3

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
 * Gradient Directions
 * ======================================================================== */

#define GRAD_NONE        0
#define GRAD_VERTICAL    1
#define GRAD_HORIZONTAL  2
#define GRAD_DIAGONAL    3   /* top-left -> bottom-right */

/* ========================================================================
 * Colour Utilities
 * ======================================================================== */

static inline uint32_t argb(uint8_t a, uint8_t r, uint8_t g, uint8_t b) {
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) |
	       ((uint32_t)g << 8) | b;
}

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
	return argb(255, r, g, b);
}

uint32_t parse_color(const char *str);

/* Scale a colour's alpha channel by a 0..1 opacity factor. */
static inline uint32_t apply_opacity(uint32_t color, float opacity) {
	if (opacity >= 1.0f)
		return color;
	if (opacity <= 0.0f)
		return color & 0x00FFFFFFu;
	uint32_t a = (uint32_t)((float)(color >> 24) * opacity + 0.5f);
	return (color & 0x00FFFFFFu) | (a << 24);
}

/* ========================================================================
 * Pixel Blending
 * ======================================================================== */

/* Source-over blend of a straight-alpha ARGB pixel onto the framebuffer. */
static inline void blend_pixel(uint32_t *dst, uint32_t src) {
	uint8_t sa = src >> 24;
	if (sa == 0)
		return;
	if (sa == 255) {
		*dst = src;
		return;
	}

	uint32_t d  = *dst;
	uint8_t  da = d >> 24;
	uint8_t  sr = (src >> 16) & 0xFF, sg = (src >> 8) & 0xFF, sb = src & 0xFF;
	uint8_t  dr = (d   >> 16) & 0xFF, dg = (d   >> 8) & 0xFF, db = d   & 0xFF;

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
	int      gradient;   /* GRAD_NONE / VERTICAL / HORIZONTAL / DIAGONAL */
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
	int             fd;
	uint32_t        conn_id;
	uint32_t        crtc_id;
	drmModeModeInfo mode;
	drm_buffer_t    buf[2];
	int             front_buf;
	struct {
		uint32_t        crtc_id;
		uint32_t        buffer_id;
		uint32_t        x, y;
		drmModeModeInfo mode;
		int             mode_valid;
	} saved_crtc;
} splash_drm_t;

/* ========================================================================
 * Image Type
 * ======================================================================== */

typedef struct {
	uint8_t *rgba;
	int      w, h;
} image_t;

int  load_image(const char *path, image_t *img);
void free_image(image_t *img);

/* ========================================================================
 * Font Type
 * ======================================================================== */

typedef struct {
	void    *info;
	uint8_t *data;
	size_t   data_size;
	float    scale;
	int      baseline;
	int      ascent;
	int      descent;
	int      line_gap;
	float    pixel_height;
	int      loaded;
} font_t;

int  font_load(const char *path, float pixel_height, int slot);
void font_unload(int slot);
void font_unload_all(void);

/* ========================================================================
 * Animation
 * ======================================================================== */

/* A time-based opacity animation (0..1). */
typedef struct {
	int      active;
	int      easing;          /* EASE_* */
	float    from, to;
	uint64_t start_ms;
	uint32_t duration_ms;
	int      remove_on_end;   /* deactivate the element when the anim ends */
	int      repeat;          /* loop forever, ping-ponging from <-> to */
} anim_t;

/* ========================================================================
 * Element Types
 * ======================================================================== */

typedef struct {
	int      active;
	int      id;
	char     text[256];
	int      x, y;
	int      align;
	int      valign;
	uint32_t color;
	int      font_slot;
	float    font_size;

	/* Word wrap (optional) */
	int      wrap;            /* non-zero: wrap long lines at word boundaries */
	int      wrap_width;      /* max line width in pixels; 0 = buffer width */

	/* Drop shadow (optional) */
	int      shadow;
	int      shadow_dx, shadow_dy;
	int      shadow_blur;
	uint32_t shadow_color;

	float    opacity;         /* 0..1 master alpha */
	anim_t   anim;            /* optional opacity animation */
} text_element_t;

typedef struct {
	int      active;
	int      id;
	image_t  img;
	int      x, y;
	int      w, h;
	int      align;
	int      valign;
	int      filter;

	float    opacity;         /* 0..1 master alpha */
	anim_t   anim;            /* optional opacity animation */
} image_overlay_t;

typedef struct {
	int      active;
	int      id;
	int      x, y, w, h;
	int      align;           /* ALIGN_*  - how the x anchor is interpreted */
	int      valign;          /* VALIGN_* - how the y anchor is interpreted */
	uint32_t color;
	int      fill;
	int      radius;
	uint32_t border_color;
	int      border_width;

	/* Gradient fill (optional) */
	uint32_t grad_color;      /* second gradient stop; first stop is `color` */
	int      grad_dir;        /* GRAD_NONE = solid */

	/* Drop shadow (optional) */
	int      shadow;
	int      shadow_dx, shadow_dy;
	int      shadow_blur;
	uint32_t shadow_color;

	float    opacity;         /* 0..1 master alpha */
	anim_t   anim;            /* optional opacity animation */
} rect_element_t;

typedef struct {
	int      active;
	int      id;
	int      x, y, w, h;
	int      align;
	int      valign;
	int      style;
	float    value;

	/* Custom colours (used when style == -1) */
	uint32_t bg_color;        /* background (empty) colour */
	uint32_t bar_color;       /* fill colour */
	uint32_t border_color;    /* border colour */
	uint32_t text_color;      /* percent-text colour */

	/* Gradient for the fill (optional) */
	uint32_t bar_color2;      /* second gradient stop; first stop is `bar_color` */
	int      bar_gradient;    /* GRAD_NONE = solid */

	/* Drop shadow of the whole bar (optional) */
	int      shadow;
	int      shadow_dx, shadow_dy;
	int      shadow_blur;
	uint32_t shadow_color;

	int      borderless;      /* no border */
	int      border_width;    /* border thickness */
	int      radius;          /* corner radius */

	int      font_slot;
	float    font_size;
	int      show_percent;

	float    opacity;         /* 0..1 master alpha */
	anim_t   anim;            /* optional opacity animation */

	/* Indeterminate mode: a sweeping highlight instead of a fixed fill */
	int      indeterminate;
	uint32_t indet_period_ms; /* time for one sweep */
	uint64_t indet_start_ms;
} progress_bar_t;

typedef struct {
	int      active;          /* currently visible / rendering */
	int      used;            /* slot configured (survives "hide") */
	int      id;
	int      x, y;            /* centre; <0 = screen centre on that axis */
	int      radius;          /* outer radius, px */
	int      spokes;          /* spoke count (0 -> 12) */
	uint32_t color;
	uint32_t period_ms;       /* one full rotation */
	uint64_t start_ms;
	float    opacity;
	anim_t   anim;
} spinner_t;

typedef struct {
	int      active;
	int      id;
	int      x, y, w, h;     /* position and size in pixels */
	int      font_slot;
	float    font_size;
	uint32_t color;           /* text colour */
	uint32_t bg_color;        /* background fill; alpha 0 = transparent */
	int      padding;         /* inner margin in pixels */
	int      max_lines;       /* ring buffer capacity (≤ CONSOLE_MAX_LINES) */
	int      line_count;      /* lines currently stored (≤ max_lines) */
	int      head;            /* next write index in lines[] */
	char     lines[CONSOLE_MAX_LINES][CONSOLE_LINE_LEN];
	float    opacity;
	anim_t   anim;
} console_t;

/* Circular / arc progress bar. Centre is at (x, y). */
typedef struct {
	int      active;
	int      id;
	int      x, y;           /* centre; <0 = screen centre on that axis */
	int      radius;         /* outer radius in px */
	int      thickness;      /* stroke width in px; 0 = radius/4 */
	float    value;          /* 0.0 .. 1.0 */
	float    start_angle;    /* degrees, 0=right (3 o'clock), CW; default -90 (top) */
	float    sweep;          /* total arc degrees; 0 or ≥360 = full circle */
	uint32_t bg_color;       /* background (unfilled) arc; alpha 0 = hidden */
	uint32_t bar_color;      /* filled arc colour */
	uint32_t bar_color2;     /* gradient second stop; ignored when bar_gradient==GRAD_NONE */
	int      bar_gradient;   /* GRAD_NONE = solid */
	int      cap;            /* 0 = flat ends, 1 = round end caps */
	int      font_slot;
	float    font_size;
	uint32_t text_color;
	int      show_percent;
	float    opacity;
	anim_t   anim;
	int      indeterminate;
	uint32_t indet_period_ms;
	uint64_t indet_start_ms;
} arc_bar_t;

/* QR code element: encodes text and renders as a grid of modules. */
typedef struct {
	int      active;
	int      id;
	char     text[512];       /* payload to encode */
	int      x, y;            /* position of top-left corner (after alignment) */
	int      align;           /* ALIGN_*  - how x is interpreted */
	int      valign;          /* VALIGN_* - how y is interpreted */
	int      module_px;       /* pixels per module; 0 = auto (fill 1/4 screen) */
	int      border;          /* quiet-zone width in modules (default 4) */
	uint32_t color;           /* dark module colour (default opaque black) */
	uint32_t bg_color;        /* light module colour; alpha 0 = transparent bg */
	int      ecc;             /* error correction: 0=LOW 1=MED 2=QRTL 3=HIGH */
	float    opacity;
	anim_t   anim;
} qr_element_t;

/* ========================================================================
 * Main State
 * ======================================================================== */

typedef struct {
	splash_drm_t drm;

	image_t  bg_image;
	int      bg_loaded;
	int      bg_scale_mode;
	float    bg_custom_scale;
	int      bg_filter;          /* resample quality for the background */
	uint32_t bg_color;

	/* Background crossfade: the outgoing image and the fade animation */
	image_t  bg_prev;
	int      bg_prev_loaded;
	int      bg_prev_scale_mode;
	float    bg_prev_custom_scale;
	int      bg_prev_filter;
	anim_t   bg_anim;
	float    bg_opacity;

	int      anim_running;       /* cached: any animation/spinner active */

	text_element_t  texts[MAX_TEXT_ELEMENTS];
	image_overlay_t overlays[MAX_IMAGE_OVERLAYS];
	rect_element_t  rects[MAX_RECTANGLES];
	progress_bar_t  bars[MAX_PROGRESS_BARS];
	spinner_t       spinners[MAX_SPINNERS];
	console_t       consoles[MAX_CONSOLES];
	arc_bar_t       arcs[MAX_ARC_BARS];
	qr_element_t    qrs[MAX_QR_ELEMENTS];

	int      server_fd;
	int      client_fds[MAX_SOCKET_CLIENTS];
	size_t   client_cmd_len[MAX_SOCKET_CLIENTS];
	char     client_cmd_buf[MAX_SOCKET_CLIENTS][CMD_MAX_LEN];

	int      kbd_fd;             /* evdev keyboard fd, or -1 if none found */
	int      hidden;             /* non-zero: splash blanked by ESC toggle */

	int      running;
	int      needs_render;
	int      ready;
	int      frozen;
	int      quiet;
	int      debug;

	uint64_t last_activity_ms;   /* monotonic time of the last command */
	uint32_t watchdog_ms;        /* idle timeout, 0 = disabled */
} splash_state_t;

/* ========================================================================
 * Function Prototypes
 *
 * Anything taking a splash_state_t* must be declared after the struct
 * definition above.
 * ======================================================================== */

/* drm.c */
int  drm_init(splash_drm_t *ctx, const char *device);
void drm_cleanup(splash_drm_t *ctx);
void drm_flip(splash_drm_t *ctx);

/* anim.c */
uint64_t now_ms(void);
int      anim_tick(splash_state_t *st, uint64_t now);

/* render.c */
void render_frame(splash_state_t *st);
void draw_filled_rect(drm_buffer_t *buf, int x, int y, int w, int h,
                      uint32_t color);
void draw_round_rect(drm_buffer_t *buf, float x, float y, float w, float h,
                     float radius, const paint_t *paint);
void draw_round_rect_outline(drm_buffer_t *buf, float x, float y,
                             float w, float h, float radius,
                             float border_width, uint32_t color);
void draw_round_rect_progress(drm_buffer_t *buf, float x, float y,
                              float w, float h, float radius,
                              float fill_w, const paint_t *paint);
void draw_round_rect_shadow(drm_buffer_t *buf, float x, float y,
                            float w, float h, float radius,
                            float blur, uint32_t color);
void draw_round_rect_sweep(drm_buffer_t *buf, float x, float y,
                           float w, float h, float radius,
                           const paint_t *paint, float phase);
void draw_image(drm_buffer_t *buf, int x, int y, int w, int h,
                const uint8_t *rgba, int img_w, int img_h,
                int filter, float opacity);
void draw_text_element(drm_buffer_t *buf, text_element_t *te);
void draw_console_element(drm_buffer_t *buf, console_t *con);
void draw_qr_element(drm_buffer_t *buf, qr_element_t *qr);
void draw_progress_bar(drm_buffer_t *buf, progress_bar_t *pb);
void draw_arc_bar(drm_buffer_t *buf, arc_bar_t *ab, uint64_t now);
void draw_rect_element(drm_buffer_t *buf, rect_element_t *re);
void draw_spinner(drm_buffer_t *buf, spinner_t *sp, uint64_t now);
void calculate_scaled_rect(int buf_w, int buf_h, int img_w, int img_h,
                           int mode, float custom_scale,
                           int *out_x, int *out_y, int *out_w, int *out_h);

/* cmd.c */
int    handle_json_command(splash_state_t *st, const char *json_str,
                            int client_idx);
cJSON *create_response(const char *status, const char *message);
int    process_json_batch(splash_state_t *st, cJSON *root, int client_idx);

/* socket.c */
int  socket_init(splash_state_t *st);
void socket_cleanup(splash_state_t *st);
int  socket_poll(splash_state_t *st, struct pollfd *fds, int *nfds);
void socket_process(splash_state_t *st, struct pollfd *fds, int nfds);
void socket_reply_json(splash_state_t *st, int client_idx, cJSON *response);

/* elements.c */
text_element_t  *text_find(splash_state_t *st, int id);
text_element_t  *text_alloc(splash_state_t *st);
image_overlay_t *overlay_find(splash_state_t *st, int id);
image_overlay_t *overlay_alloc(splash_state_t *st);
rect_element_t  *rect_find(splash_state_t *st, int id);
rect_element_t  *rect_alloc(splash_state_t *st);
spinner_t       *spinner_find(splash_state_t *st, int id);
spinner_t       *spinner_alloc(splash_state_t *st);
console_t       *console_find(splash_state_t *st, int id);
console_t       *console_alloc(splash_state_t *st);
progress_bar_t  *progress_find(splash_state_t *st, int id);
arc_bar_t       *arc_find(splash_state_t *st, int id);
arc_bar_t       *arc_alloc(splash_state_t *st);
qr_element_t    *qr_find(splash_state_t *st, int id);
qr_element_t    *qr_alloc(splash_state_t *st);
void             clear_all_elements(splash_state_t *st);

/* utils.c */
void  set_default_progress_colors(progress_bar_t *pb, int style);
int   clamp(int val, int min, int max);
float fclamp(float val, float min, float max);
int   resolve_font_path(const char *path, char *out, size_t outsz);
int   resolve_image_path(const char *path, char *out, size_t outsz);
int   get_coord(cJSON *obj, const char *key, int dim, int default_val);

/* kbd.c */
void kbd_init(splash_state_t *st);
void kbd_cleanup(splash_state_t *st);
void kbd_process(splash_state_t *st);

/* main.c helpers */
int load_config(splash_state_t *st, const char *config_str);
int process_startup_cmds(splash_state_t *st, const char *cmds_str);

/* JSON helpers (defined in cmd.c, also used in main.c) */
int         get_int(cJSON *obj, const char *key, int default_val);
float       get_float(cJSON *obj, const char *key, float default_val);
const char *get_string(cJSON *obj, const char *key, const char *default_val);

#endif /* SPLASH_H */
