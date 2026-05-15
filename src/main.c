/*
 * main.c - splash-drm daemon entry point
 * 
 * Usage: splash-drm <drm_device> <pipe_path> <font_path> [options]
 */

#include "splash.h"

/* ========================================================================
 * Print Usage
 * ======================================================================== */

static void print_usage(const char *prog) {
    fprintf(stderr,
        "splash-drm v%s - Self-contained DRM/KMS bootsplash daemon\n\n"
        "Usage: %s <drm_device> <pipe_path> <font_path> [options]\n\n"
        "Arguments:\n"
        "  drm_device    DRM device path (e.g., /dev/dri/card0)\n"
        "  pipe_path     Command pipe path (e.g., /run/splash.pipe)\n"
        "  font_path     TrueType font file\n\n"
        "Options:\n"
        "  -bg <path> [mode]       Initial background image\n"
        "                          mode: cover (default), contain, stretch, none\n"
        "  -text <id> <x> <y> <align> <color> <text>  Initial text element\n"
        "  -v, --version           Show version and exit\n"
        "  -h, --help              Show this help and exit\n\n"
        "Commands via pipe (one per line):\n"
        "  EXIT                          - Terminate daemon\n"
        "  RELOCATE_PIPE <new_path>      - Move pipe to new rootfs\n"
        "  READY?                        - Check if daemon is ready\n"
        "  CLEAR [#RRGGBB]               - Clear screen (optional color)\n"
        "  IMAGE <path> [mode]           - Set background image\n"
        "  TEXT <id> <x> <y> <L|C|R> <#RRGGBB> <text>\n"
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
        "  %s /dev/dri/card0 /run/splash.pipe /usr/share/fonts/TTF/DejaVuSans.ttf\n"
        "  %s /dev/dri/card0 /run/splash.pipe font.ttf -bg /boot/splash.png contain\n"
        "  %s /dev/dri/card0 /run/splash.pipe font.ttf -text 0 100 200 L #FFFFFF \"Loading...\"\n",
        SPLASH_VERSION, prog, prog, prog, prog);
}

/* ========================================================================
 * Parse Command Line Arguments
 * ======================================================================== */

static int parse_args(splash_state_t *st, int argc, char **argv, int *opt_idx) {
    for (int i = 4; i < argc; i++) {
        if (strcmp(argv[i], "-bg") == 0 && i + 1 < argc) {
            if (load_image(argv[++i], &st->bg_image) == 0) {
                st->bg_loaded = 1;
                st->bg_scale_mode = SCALE_COVER;
                if (i + 1 < argc && argv[i+1][0] != '-') {
                    char *mode = argv[++i];
                    if (strcmp(mode, "contain") == 0) st->bg_scale_mode = SCALE_CONTAIN;
                    else if (strcmp(mode, "stretch") == 0) st->bg_scale_mode = SCALE_STRETCH;
                    else if (strcmp(mode, "none") == 0) st->bg_scale_mode = SCALE_NONE;
                }
            }
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
                strncpy(te->text, argv[++i], 255);
                te->text[255] = 0;
                te->active = 1;
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
    }
    return 0;
}

/* ========================================================================
 * Main
 * ======================================================================== */

int main(int argc, char **argv) {
    if (argc < 4) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *device = argv[1];
    const char *pipe_path = argv[2];
    const char *font_path = argv[3];
    
    /* Load font */
    if (font_load(font_path, 24.0f) < 0) {
        fprintf(stderr, "Warning: Could not load font %s, text rendering disabled\n", font_path);
    }
    
    /* Initialize DRM */
    splash_state_t st = {0};
    strncpy(st.pipe_path, pipe_path, 255);
    st.pipe_path[255] = 0;
    
    if (drm_init(&st.drm, device) < 0) {
        fprintf(stderr, "Failed to initialize DRM on %s\n", device);
        font_unload();
        return 1;
    }
    
    printf("splash-drm v%s: DRM %dx%d @ %dHz\n", SPLASH_VERSION,
           st.drm.mode.hdisplay, st.drm.mode.vdisplay, st.drm.mode.vrefresh);
    
    /* Parse optional arguments */
    parse_args(&st, argc, argv, NULL);
    
    /* Create command pipe */
    st.pipe_fd = pipe_create(st.pipe_path);
    if (st.pipe_fd < 0) {
        fprintf(stderr, "Failed to create pipe %s\n", st.pipe_path);
        drm_cleanup(&st.drm);
        font_unload();
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
    font_unload();
    drm_cleanup(&st.drm);
    
    printf("splash-drm exited cleanly\n");
    return 0;
}
