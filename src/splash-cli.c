/*
 * splash-cli.c - Command-line client for splash-drm daemon
 * 
 * Usage: splash-cli <pipe_path> <command> [args...]
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>

#define CMD_MAX_LEN 4096
#define MAX_FONTS 4

static int debug_mode = 0;

static int send_cmd(const char *pipe_path, const char *cmd) {
    if (debug_mode) {
        fprintf(stderr, "[debug] Sending: %s\n", cmd);
    }

    int fd = open(pipe_path, O_WRONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Cannot open pipe %s: %s\n", pipe_path, strerror(errno));
        return 1;
    }

    size_t len = strlen(cmd);
    if (write(fd, cmd, len) != (ssize_t)len || write(fd, "\n", 1) != 1) {
        fprintf(stderr, "Failed to write command\n");
        close(fd);
        return 1;
    }

    close(fd);
    return 0;
}

static int is_number(const char *str) {
    if (!str || !*str) return 0;
    char *endptr;
    strtod(str, &endptr);
    return *endptr == '\0';
}

static int is_integer(const char *str) {
    if (!str || !*str) return 0;
    char *endptr;
    strtol(str, &endptr, 10);
    return *endptr == '\0';
}

static void print_usage(const char *prog) {
    fprintf(stderr,
        "splash-cli - Command-line client for splash-drm daemon\n\n"
        "Usage: %s <pipe_path> <command> [args...]\n\n"
        "Options:\n"
        "  --debug    Enable debug output\n\n"
        "Commands:\n"
        "  image <path> [mode] [scale]   - Set background image\n"
        "  text <id> <x> <y> <align> <color> [font_slot] [font_size] -- <text>\n"
        "  remove-text <id>              - Remove text element\n"
        "  rect <id> <x> <y> <w> <h> <color> [blend]\n"
        "  remove-rect <id>              - Remove rectangle\n"
        "  overlay <id> <x> <y> [w] [h] [align] [valign] <path>\n"
        "  remove-overlay <id>           - Remove image overlay\n"
        "  progress <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value> [font_slot] [font_size] -- [text]\n"
        "  update <id> <value> [text]    - Update progress bar\n"
        "  hide-progress <id>            - Hide progress bar\n"
        "  clear [color]                 - Clear screen\n"
        "  ready                         - Check if daemon is ready\n"
        "  relocate <new_path>           - Relocate pipe\n"
        "  exit                          - Terminate daemon\n\n"
        "Font params:\n"
        "  font_slot: 0-%d (optional, default 0)\n"
        "  font_size: any positive number (optional, default: loaded font size)\n"
        "  If only one number given and it's > %d, it's treated as font_size with slot 0\n\n"
        "Examples:\n"
        "  %s /run/splash.pipe image /boot/splash.png\n"
        "  %s /run/splash.pipe text 0 100 200 center #FFFFFF -- \"Hello World\"\n"
        "  %s /run/splash.pipe text 0 100 200 center #FFFFFF 1 -- \"Bold text\"\n"
        "  %s /run/splash.pipe text 0 100 200 center #FFFFFF 1 48 -- \"Big text\"\n"
        "  %s /run/splash.pipe text 0 100 200 center #FFFFFF 48 -- \"Big with slot 0\"\n"
        "  %s /run/splash.pipe text 0 100 200 center #FFFFFF -- \"Chapter 1 page 48\"\n"
        "  %s /run/splash.pipe progress 0 100 400 600 20 1 \"Loading: \" \"%%\" 0 -- \"Starting...\"\n"
        "  %s /run/splash.pipe progress 0 100 400 600 20 1 \"Loading: \" \"%%\" 0 1 24 -- \"Network\"\n"
        "  %s /run/splash.pipe update 0 45 \"Starting network...\"\n"
        "  %s /run/splash.pipe clear #000000\n"
        "  %s /run/splash.pipe exit\n",
        prog, MAX_FONTS - 1, MAX_FONTS - 1,
        prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* Find -- separator in argv, returns index or -1 if not found */
static int find_separator(int argc, char **argv, int start_idx) {
    for (int i = start_idx; i < argc; i++) {
        if (strcmp(argv[i], "--") == 0) {
            return i;
        }
    }
    return -1;
}

/* Parse font params: handles slot-only, size-only (with slot 0), or both */
static void parse_font_params(int argc, char **argv, int start_idx, int sep_idx,
                               int *font_slot, float *font_size) {
    *font_slot = -1;
    *font_size = 0;

    if (start_idx >= argc || (sep_idx >= 0 && start_idx >= sep_idx)) {
        return;
    }

    int max_idx = (sep_idx >= 0) ? sep_idx : argc;
    int count = max_idx - start_idx;

    if (count == 0) {
        return;
    }

    if (count == 1) {
        /* One number: could be slot or size */
        if (is_integer(argv[start_idx])) {
            int val = atoi(argv[start_idx]);
            if (val >= 0 && val < MAX_FONTS) {
                *font_slot = val;
            } else if (val > 0 && val <= 128) {
                *font_slot = 0;
                *font_size = (float)val;
            }
        }
    } else if (count >= 2) {
        /* Two numbers: first is slot, second is size */
        if (is_integer(argv[start_idx])) {
            int slot = atoi(argv[start_idx]);
            if (slot >= 0 && slot < MAX_FONTS && is_number(argv[start_idx + 1])) {
                *font_slot = slot;
                *font_size = atof(argv[start_idx + 1]);
            } else if (slot > 0 && slot <= 128) {
                /* First number is size, use slot 0 */
                *font_slot = 0;
                *font_size = (float)slot;
            }
        }
    }
}

int main(int argc, char **argv) {
    int arg_offset = 1;

    /* Check for --debug before other args */
    if (argc > 1 && strcmp(argv[1], "--debug") == 0) {
        debug_mode = 1;
        arg_offset = 2;
    }

    if (argc - arg_offset < 2) {
        print_usage(argv[0]);
        return 1;
    }

    const char *pipe_path = argv[arg_offset];
    const char *cmd = argv[arg_offset + 1];

    /* Check if pipe exists */
    struct stat st;
    if (stat(pipe_path, &st) < 0) {
        fprintf(stderr, "Pipe %s does not exist: %s\n", pipe_path, strerror(errno));
        fprintf(stderr, "Is the splash-drm daemon running?\n");
        return 1;
    }
    if (!S_ISFIFO(st.st_mode)) {
        fprintf(stderr, "%s is not a named pipe\n", pipe_path);
        return 1;
    }

    if (debug_mode) {
        fprintf(stderr, "[debug] Pipe: %s\n", pipe_path);
        fprintf(stderr, "[debug] Command: %s\n", cmd);
        fprintf(stderr, "[debug] argc: %d, arg_offset: %d\n", argc, arg_offset);
    }

    char buf[CMD_MAX_LEN];
    int pos = 0;

    if (strcmp(cmd, "image") == 0) {
        if (argc - arg_offset < 3) { 
            fprintf(stderr, "Usage: image <path> [mode] [scale]\n"); 
            return 1; 
        }
        pos = snprintf(buf, sizeof(buf), "IMAGE %s", argv[arg_offset + 2]);
        if (argc - arg_offset > 3) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[arg_offset + 3]);
        if (argc - arg_offset > 4) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[arg_offset + 4]);
    }
    else if (strcmp(cmd, "text") == 0) {
        /* text <id> <x> <y> <align> <color> [font_slot] [font_size] -- <text> */
        if (argc - arg_offset < 7) { 
            fprintf(stderr, "Usage: text <id> <x> <y> <align> <color> [font_slot] [font_size] -- <text>\n"); 
            return 1; 
        }

        int sep_idx = find_separator(argc, argv, arg_offset + 7);
        int text_start;
        int font_slot;
        float font_size;

        parse_font_params(argc, argv, arg_offset + 7, sep_idx, &font_slot, &font_size);

        if (sep_idx >= 0) {
            text_start = sep_idx + 1;
        } else {
            /* No -- separator - require it for text with spaces */
            fprintf(stderr, "Error: -- separator required before text. Use: text ... -- <text>\n");
            return 1;
        }

        if (text_start >= argc) {
            fprintf(stderr, "Error: No text provided after --\n");
            return 1;
        }

        /* Build command with -- separator included */
        pos = snprintf(buf, sizeof(buf), "TEXT %s %s %s %s %s",
            argv[arg_offset + 2], argv[arg_offset + 3], argv[arg_offset + 4], 
            argv[arg_offset + 5], argv[arg_offset + 6]);

        /* Append font params if specified */
        if (font_slot >= 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %d", font_slot);
            if (font_size > 0) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %.1f", font_size);
            }
        }

        /* Append -- separator and text */
        pos += snprintf(buf + pos, sizeof(buf) - pos, " --");
        for (int i = text_start; i < argc; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[i]);
        }
    }
    else if (strcmp(cmd, "remove-text") == 0) {
        if (argc - arg_offset < 3) { fprintf(stderr, "Usage: remove-text <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "REMOVE_TEXT %s", argv[arg_offset + 2]);
    }
    else if (strcmp(cmd, "rect") == 0) {
        if (argc - arg_offset < 8) { 
            fprintf(stderr, "Usage: rect <id> <x> <y> <w> <h> <color> [blend]\n"); 
            return 1; 
        }
        pos = snprintf(buf, sizeof(buf), "RECT %s %s %s %s %s %s",
            argv[arg_offset + 2], argv[arg_offset + 3], argv[arg_offset + 4], 
            argv[arg_offset + 5], argv[arg_offset + 6], argv[arg_offset + 7]);
        if (argc - arg_offset > 8) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[arg_offset + 8]);
    }
    else if (strcmp(cmd, "remove-rect") == 0) {
        if (argc - arg_offset < 3) { fprintf(stderr, "Usage: remove-rect <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "REMOVE_RECT %s", argv[arg_offset + 2]);
    }
    else if (strcmp(cmd, "overlay") == 0) {
        if (argc - arg_offset < 5) { 
            fprintf(stderr, "Usage: overlay <id> <x> <y> [w] [h] [align] [valign] <path>\n"); 
            return 1; 
        }
        pos = snprintf(buf, sizeof(buf), "OVERLAY");
        for (int i = arg_offset + 2; i < argc; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[i]);
        }
    }
    else if (strcmp(cmd, "remove-overlay") == 0) {
        if (argc - arg_offset < 3) { fprintf(stderr, "Usage: remove-overlay <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "REMOVE_OVERLAY %s", argv[arg_offset + 2]);
    }
    else if (strcmp(cmd, "progress") == 0) {
        /* progress <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value> [font_slot] [font_size] -- [text] */
        if (argc - arg_offset < 10) { 
            fprintf(stderr, "Usage: progress <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value> [font_slot] [font_size] -- [text]\n"); 
            return 1; 
        }

        int sep_idx = find_separator(argc, argv, arg_offset + 10);
        int text_start;
        int font_slot;
        float font_size;

        parse_font_params(argc, argv, arg_offset + 10, sep_idx, &font_slot, &font_size);

        if (sep_idx >= 0) {
            text_start = sep_idx + 1;
        } else {
            text_start = argc; /* no text */
        }

        /* Build command with -- separator if text exists */
        pos = snprintf(buf, sizeof(buf), "PROGRESS %s %s %s %s %s %s %s %s %s",
            argv[arg_offset + 2], argv[arg_offset + 3], argv[arg_offset + 4], 
            argv[arg_offset + 5], argv[arg_offset + 6], argv[arg_offset + 7], 
            argv[arg_offset + 8], argv[arg_offset + 9], argv[arg_offset + 10]);

        /* Append font params if specified */
        if (font_slot >= 0) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %d", font_slot);
            if (font_size > 0) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %.1f", font_size);
            }
        }

        /* Append -- separator and text if any */
        if (text_start < argc) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " --");
            for (int i = text_start; i < argc; i++) {
                pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[i]);
            }
        }
    }
    else if (strcmp(cmd, "update") == 0) {
        if (argc - arg_offset < 4) { fprintf(stderr, "Usage: update <id> <value> [text]\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "UPDATE_PROGRESS %s %s", argv[arg_offset + 2], argv[arg_offset + 3]);
        if (argc - arg_offset > 4) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
            for (int i = arg_offset + 4; i < argc; i++) {
                if (i > arg_offset + 4) pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", argv[i]);
            }
        }
    }
    else if (strcmp(cmd, "hide-progress") == 0) {
        if (argc - arg_offset < 3) { fprintf(stderr, "Usage: hide-progress <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "HIDE_PROGRESS %s", argv[arg_offset + 2]);
    }
    else if (strcmp(cmd, "clear") == 0) {
        pos = snprintf(buf, sizeof(buf), "CLEAR");
        if (argc - arg_offset > 2) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[arg_offset + 2]);
    }
    else if (strcmp(cmd, "ready") == 0) {
        pos = snprintf(buf, sizeof(buf), "READY?");
    }
    else if (strcmp(cmd, "relocate") == 0) {
        if (argc - arg_offset < 3) { fprintf(stderr, "Usage: relocate <new_path>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "RELOCATE_PIPE %s", argv[arg_offset + 2]);
    }
    else if (strcmp(cmd, "exit") == 0) {
        pos = snprintf(buf, sizeof(buf), "EXIT");
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }

    if (debug_mode) {
        fprintf(stderr, "[debug] Final command: %s\n", buf);
    }

    return send_cmd(pipe_path, buf);
}
