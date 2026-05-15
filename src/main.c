/*
 * main.c - splash-drm daemon entry point
 * 
 * Usage: splash-drm <drm_device> <pipe_path> [options]
 */

#include "splash.h"

/* ========================================================================
 * Print Usage
 * ======================================================================== */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "splash-drm v%s - Self-contained DRM/KMS bootsplash daemon\n\n"
        "Usage: %s <drm_device> <pipe_path> [options]\n\n"
        "Arguments:\n"
        "  drm_device    DRM device path (e.g., /dev/dri/card0)\n"
        "  pipe_path     Command pipe path (e.g., /run/splash.pipe)\n\n"
        "Options:\n"
        "  -font <slot> <path> [size]  Load font to slot (0-3), default size 24\n"
        "  -bg <path> [mode] [scale]   Initial background image\n"
        "                              mode: cover, contain (default), stretch, none, custom\n"
        "                              scale: required for custom mode (e.g., 0.5)\n"
        "  -bg-color <#RRGGBB>          Initial background color (default: black)\n"
        "  -text <id> <x> <y> <align> <color> <text>\n"
        "                              Initial text element\n"
        "  -v, --version               Show version and exit\n"
        "  -h, --help                  Show this help and exit\n"
        "  -q, --quiet                 Suppress all output\n"
        "  --debug                     Enable debug output\n\n"
        "Commands via pipe (one per line):\n"
        "  EXIT                          - Terminate daemon\n"
        "  RELOCATE_PIPE <new_path>      - Move pipe to new rootfs\n"
        "  READY?                        - Check if daemon is ready\n"
        "  CLEAR [#RRGGBB]               - Clear screen (optional color)\n"
        "  IMAGE <path> [mode] [scale]   - Set background image\n"
        "  BG_COLOR <#RRGGBB>            - Set background color\n"
        "  TEXT <id> <x> <y> <L|C|R> <#RRGGBB> <text> [font_slot] [font_size]\n"
        "  REMOVE_TEXT <id>              - Remove text element\n"
        "  RECT <id> <x> <y> <w> <h> <#RRGGBB> [blend]\n"
        "  REMOVE_RECT <id>              - Remove rectangle\n"
        "  OVERLAY <id> <x> <y> [w] [h] [align] [valign] <path>\n"
        "  REMOVE_OVERLAY <id>           - Remove image overlay\n"
        "  PROGRESS <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value>\n"
        "  UPDATE_PROGRESS <id> <value> [inner_text]\n"
        "  HIDE_PROGRESS <id>            - Hide progress bar\n\n"
        "Progress bar styles:\n"
        "  0=Blue  1=Green  2=Amber  3=Red  4=Purple  5=Cyan\n\n"
        "Examples:\n"
        "  %s /dev/dri/card0 /run/splash.pipe\n"
        "  %s /dev/dri/card0 /run/splash.pipe -font 0 ./fonts/regular.ttf\n"
        "  %s /dev/dri/card0 /run/splash.pipe -font 0 font.ttf -bg splash.png contain\n"
        "  %s /dev/dri/card0 /run/splash.pipe -font 0 font.ttf -font 1 bold.ttf 32\n",
        SPLASH_VERSION, prog, prog, prog, prog, prog);
}

/* ========================================================================
 * Parse Command Line Arguments
 * ======================================================================== */

static int parse_scale_mode(const char *mode_str, float *custom_scale) {
    if (!mode_str || strcmp(mode_str, "contain") == 0) return SCALE_CONTAIN;
    if (strcmp(mode_str, "cover") == 0) return SCALE_COVER;
    if (strcmp(mode_str, "stretch") == 0) return SCALE_STRETCH;
    if (strcmp(mode_str, "none") == 0) return SCALE_NONE;
    if (strcmp(mode_str, "custom") == 0) return SCALE_CUSTOM;
    return SCALE_CONTAIN;
}

static int parse_args(splash_state_t *st, int argc, char **argv) {
    for (int i = 3; i < argc; i++) {
        if (strcmp(argv[i], "-bg") == 0 && i + 1 < argc) {
            if (load_image(argv[++i], &st->bg_image) == 0) {
                st->bg_loaded = 1;
                st->bg_scale_mode = SCALE_CONTAIN;
                st->bg_custom_scale = 1.0f;
                if (i + 1 < argc && argv[i+1][0] != '-') {
                    char *mode = argv[++i];
                    st->bg_scale_mode = parse_scale_mode(mode, &st->bg_custom_scale);
                    if (st->bg_scale_mode == SCALE_CUSTOM && i + 1 < argc && argv[i+1][0] != '-') {
                        st->bg_custom_scale = atof(argv[++i]);
                    }
                }
            }
        }
        else if (strcmp(argv[i], "-bg-color") == 0 && i + 1 < argc) {
            st->bg_color = parse_color(argv[++i]);
        }
        else if (strcmp(argv[i], "-text") == 0 && i + 5 < argc) {
            int id = atoi(argv[++i]);
            text_element_t *te = text_alloc(st);
            if (te) {
                te->id = id;
                te->x = atoi(argv[++i]);
                te->y = atoi(argv[++i]);
                char *align = argv[++i];
                te->align = (align[0] == 'C' || align[0] == 'c') ? ALIGN_CENTER :
                           (align[0] == 'R' || align[0] == 'r') ? ALIGN_RIGHT : ALIGN_LEFT;
                te->color = parse_color(argv[++i]);
                strncpy(te->text, argv[++i], sizeof(te->text) - 1);
                te->text[sizeof(te->text) - 1] = '\0';
                te->active = 1;
                te->font_slot = 0;
                te->font_size = 0;
            }
        }
        else if (strcmp(argv[i], "-font") == 0 && i + 2 < argc) {
            int slot = atoi(argv[++i]);
            const char *path = argv[++i];
            float size = 24.0f;
            if (i + 1 < argc && argv[i+1][0] != '-') {
                size = atof(argv[++i]);
            }
            if (slot >= 0 && slot < MAX_FONTS) {
                if (font_load(path, size, slot) < 0 && !st->quiet) {
                    fprintf(stderr, "Warning: Could not load font %s to slot %d\n", path, slot);
                }
            }
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("splash-drm v%s\n", SPLASH_VERSION);
            exit(0);
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            exit(0);
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            st->quiet = 1;
        }
        else if (strcmp(argv[i], "--debug") == 0) {
            st->debug = 1;
        }
    }
    return 0;
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *device = argv[1];
    const char *pipe_path = argv[2];
    
    splash_state_t st = {0};
    st.bg_color = 0;
    strncpy(st.pipe_path, pipe_path, sizeof(st.pipe_path) - 1);
    st.pipe_path[sizeof(st.pipe_path) - 1] = '\0';
    
    /* Parse args early to get quiet/debug flags and fonts */
    parse_args(&st, argc, argv);
    
    /* Initialize DRM */
    if (drm_init(&st.drm, device) < 0) {
        if (!st.quiet)
            fprintf(stderr, "Failed to initialize DRM on %s\n", device);
        font_unload_all();
        return 1;
    }
    
    if (!st.quiet)
        printf("splash-drm v%s: DRM %dx%d @ %dHz\n", SPLASH_VERSION,
               st.drm.mode.hdisplay, st.drm.mode.vdisplay, st.drm.mode.vrefresh);
    
    /* Create command pipe */
    st.pipe_fd = pipe_create(st.pipe_path);
    if (st.pipe_fd < 0) {
        if (!st.quiet)
            fprintf(stderr, "Failed to create pipe %s\n", st.pipe_path);
        drm_cleanup(&st.drm);
        font_unload_all();
        return 1;
    }
    
    st.running = 1;
    st.needs_render = 1;
    
    /* Initial render */
    render_frame(&st);
    
    /* Main loop */
    char cmdbuf[CMD_MAX_LEN];
    
    while (st.running) {
        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(st.pipe_fd, &fds);
        
        struct timeval tv = {.tv_sec = 0, .tv_usec = 1000000 / RENDER_FPS};
        int ret = select(st.pipe_fd + 1, &fds, NULL, NULL, &tv);
        
        if (ret > 0 && FD_ISSET(st.pipe_fd, &fds)) {
            int len = pipe_read_command(&st, cmdbuf, sizeof(cmdbuf));
            if (len > 0) {
                handle_command(&st, cmdbuf);
            }
        }
        
        /* Render if needed */
        if (st.needs_render) {
            render_frame(&st);
        }
    }
    
    /* Cleanup */
    close(st.pipe_fd);
    unlink(st.pipe_path);
    
    clear_all_elements(&st);
    free_image(&st.bg_image);
    font_unload_all();
    drm_cleanup(&st.drm);
    
    if (!st.quiet)
        printf("splash-drm exited cleanly\n");
    return 0;
}
