/*
 * main.c - splash-drm daemon entry point
 */

#include "splash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>

static void print_usage(const char *prog) {
    fprintf(stderr,
        "splash-drm v%s - DRM/KMS bootsplash daemon\n\n"
        "Usage: %s <drm_device> [options]\n\n"
        "Arguments:\n"
        "  drm_device    DRM device path (e.g., /dev/dri/card0)\n\n"
        "Options:\n"
        "  --config <file|json>   Load configuration (fonts, defaults)\n"
        "  --cmds <file|json>     Execute initial commands on startup\n"
        "  -q, --quiet            Suppress all output\n"
        "  --debug                Enable debug output\n"
        "  -v, --version          Show version and exit\n"
        "  -h, --help             Show this help and exit\n\n"
        "Config JSON format:\n"
        "  {\"fonts\": [\n"
        "    {\"slot\": 0, \"path\": \"/path/to/font.ttf\", \"size\": 24}\n"
        "  ]}\n\n"
        "Commands JSON format (single):\n"
        "  {\"cmd\": \"text\", \"id\": 0, \"x\": 100, \"y\": 200, \"text\": \"Hello\"}\n\n"
        "Commands JSON format (batch):\n"
        "  [{\"cmd\": \"clear\"}, {\"cmd\": \"image\", \"path\": \"splash.png\"}]\n\n"
        "Examples:\n"
        "  %s /dev/dri/card0\n"
        "  %s /dev/dri/card0 --config '{\"fonts\":[{\"slot\":0,\"path\":\"font.ttf\",\"size\":24}]}'\n"
        "  %s /dev/dri/card0 --config /etc/splash.json --cmds '[{\"cmd\":\"image\",\"path\":\"boot.png\"}]'\n",
        SPLASH_VERSION, prog, prog, prog, prog);
}

static int is_json_string(const char *str) {
    if (!str || !*str) return 0;
    while (*str && (*str == ' ' || *str == '\t' || *str == '\n')) str++;
    return (*str == '{' || *str == '[');
}

static char* read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *buf = malloc(size + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    
    fread(buf, 1, size, f);
    buf[size] = '\0';
    fclose(f);
    return buf;
}

int load_config(splash_state_t *st, const char *config_str) {
    char *json_data = NULL;
    int needs_free = 0;
    
    if (is_json_string(config_str)) {
        json_data = (char*)config_str;
    } else {
        json_data = read_file(config_str);
        if (!json_data) {
            if (!st->quiet) fprintf(stderr, "Cannot read config file: %s\n", config_str);
            return -1;
        }
        needs_free = 1;
    }
    
    cJSON *root = cJSON_Parse(json_data);
    if (needs_free) free(json_data);
    
    if (!root) {
        if (!st->quiet) fprintf(stderr, "Invalid config JSON\n");
        return -1;
    }
    
    cJSON *fonts = cJSON_GetObjectItem(root, "fonts");
    if (fonts && cJSON_IsArray(fonts)) {
        int count = cJSON_GetArraySize(fonts);
        for (int i = 0; i < count; i++) {
            cJSON *font = cJSON_GetArrayItem(fonts, i);
            if (!cJSON_IsObject(font)) continue;
            
            int slot = get_int(font, "slot", -1);
            const char *path = get_string(font, "path", NULL);
            float size = get_float(font, "size", 24.0f);
            
            if (slot >= 0 && slot < MAX_FONTS && path) {
                if (font_load(path, size, slot) < 0 && !st->quiet) {
                    fprintf(stderr, "Warning: Could not load font %s to slot %d\n", path, slot);
                } else if (st->debug) {
                    fprintf(stderr, "[debug] Loaded font %s to slot %d (size %.1f)\n", path, slot, size);
                }
            }
        }
    }
    
    cJSON_Delete(root);
    return 0;
}

int process_startup_cmds(splash_state_t *st, const char *cmds_str) {
    char *json_data = NULL;
    int needs_free = 0;
    
    if (is_json_string(cmds_str)) {
        json_data = (char*)cmds_str;
    } else {
        json_data = read_file(cmds_str);
        if (!json_data) {
            if (!st->quiet) fprintf(stderr, "Cannot read commands file: %s\n", cmds_str);
            return -1;
        }
        needs_free = 1;
    }
    
    cJSON *root = cJSON_Parse(json_data);
    if (needs_free) free(json_data);
    
    if (!root) {
        if (!st->quiet) fprintf(stderr, "Invalid commands JSON\n");
        return -1;
    }
    
    process_json_batch(st, root, -1);
    
    cJSON_Delete(root);
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *device = argv[1];
    const char *config_arg = NULL;
    const char *cmds_arg = NULL;

    splash_state_t st = {0};
    st.bg_color = 0;
    st.server_fd = -1;
    for (int i = 0; i < MAX_SOCKET_CLIENTS; i++) {
        st.client_fds[i] = -1;
    }

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            config_arg = argv[++i];
        }
        else if (strcmp(argv[i], "--cmds") == 0 && i + 1 < argc) {
            cmds_arg = argv[++i];
        }
        else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
            st.quiet = 1;
        }
        else if (strcmp(argv[i], "--debug") == 0) {
            st.debug = 1;
        }
        else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("splash-drm v%s\n", SPLASH_VERSION);
            return 0;
        }
        else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        }
    }

    if (drm_init(&st.drm, device) < 0) {
        if (!st.quiet)
            fprintf(stderr, "Failed to initialize DRM on %s\n", device);
        return 1;
    }

    if (!st.quiet)
        printf("splash-drm v%s: DRM %dx%d @ %dHz\n", SPLASH_VERSION,
               st.drm.mode.hdisplay, st.drm.mode.vdisplay, st.drm.mode.vrefresh);

    if (config_arg) {
        if (load_config(&st, config_arg) < 0 && !st.quiet) {
            fprintf(stderr, "Warning: Failed to load config\n");
        }
    }

    if (socket_init(&st) < 0) {
        if (!st.quiet)
            fprintf(stderr, "Failed to create abstract socket\n");
        drm_cleanup(&st.drm);
        font_unload_all();
        return 1;
    }

    st.running = 1;
    st.needs_render = 1;
    st.frozen = 0;

    if (cmds_arg) {
        if (process_startup_cmds(&st, cmds_arg) < 0 && !st.quiet) {
            fprintf(stderr, "Warning: Failed to process startup commands\n");
        }
    }

    if (st.needs_render) {
        render_frame(&st);
    }

    while (st.running) {
        struct pollfd fds[1 + MAX_SOCKET_CLIENTS];
        int nfds = 0;
        
        socket_poll(&st, fds, &nfds);

        int timeout = st.frozen ? -1 : (1000 / RENDER_FPS);
        int ret = poll(fds, nfds, timeout);

        if (ret < 0) {
            if (errno == EINTR) continue;
            perror("poll");
            break;
        }

        socket_process(&st, fds, nfds);

        if (!st.frozen && st.needs_render) {
            render_frame(&st);
        }
    }

    socket_cleanup(&st);
    clear_all_elements(&st);
    free_image(&st.bg_image);
    font_unload_all();
    drm_cleanup(&st.drm);

    if (!st.quiet)
        printf("splash-drm exited cleanly\n");
    return 0;
}
