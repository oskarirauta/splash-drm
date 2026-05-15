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

#define CMD_MAX_LEN 4096

static int send_cmd(const char *pipe_path, const char *cmd) {
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

static void print_usage(const char *prog) {
    fprintf(stderr,
        "splash-cli - Control the splash-drm daemon\n\n"
        "Usage: %s <pipe_path> <command> [args...]\n\n"
        "Commands:\n"
        "  image <path> [mode]           - Set background image\n"
        "  text <id> <x> <y> <align> <color> <text>\n"
        "  remove-text <id>              - Remove text element\n"
        "  rect <id> <x> <y> <w> <h> <color> [blend]\n"
        "  remove-rect <id>              - Remove rectangle\n"
        "  overlay <id> <x> <y> [w] [h] [align] [valign] <path>\n"
        "  remove-overlay <id>           - Remove image overlay\n"
        "  progress <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value>\n"
        "  update <id> <value> [text]    - Update progress bar\n"
        "  hide-progress <id>            - Hide progress bar\n"
        "  clear [color]                 - Clear screen\n"
        "  ready                         - Check if daemon is ready\n"
        "  relocate <new_path>           - Relocate pipe\n"
        "  exit                          - Terminate daemon\n\n"
        "Examples:\n"
        "  %s /run/splash.pipe image /boot/splash.png\n"
        "  %s /run/splash.pipe text 0 100 200 center #FFFFFF \"Loading...\"\n"
        "  %s /run/splash.pipe progress 0 100 400 600 20 1 \"Loading: \" \"%%\" 0\n"
        "  %s /run/splash.pipe update 0 45 \"Starting network...\"\n"
        "  %s /run/splash.pipe clear #000000\n"
        "  %s /run/splash.pipe exit\n",
        prog, prog, prog, prog, prog, prog, prog);
}

int main(int argc, char **argv) {
    if (argc < 3) {
        print_usage(argv[0]);
        return 1;
    }
    
    const char *pipe_path = argv[1];
    const char *cmd = argv[2];
    
    char buf[CMD_MAX_LEN];
    int pos = 0;
    
    if (strcmp(cmd, "image") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: image <path> [mode]\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "IMAGE %s", argv[3]);
        if (argc > 4) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[4]);
    }
    else if (strcmp(cmd, "text") == 0) {
        if (argc < 8) { fprintf(stderr, "Usage: text <id> <x> <y> <align> <color> <text>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "TEXT %s %s %s %s %s ",
            argv[3], argv[4], argv[5], argv[6], argv[7]);
        for (int i = 8; i < argc; i++) {
            if (i > 8) pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
            pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", argv[i]);
        }
    }
    else if (strcmp(cmd, "remove-text") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: remove-text <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "REMOVE_TEXT %s", argv[3]);
    }
    else if (strcmp(cmd, "rect") == 0) {
        if (argc < 8) { fprintf(stderr, "Usage: rect <id> <x> <y> <w> <h> <color> [blend]\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "RECT %s %s %s %s %s %s",
            argv[3], argv[4], argv[5], argv[6], argv[7], argv[8]);
        if (argc > 9) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[9]);
    }
    else if (strcmp(cmd, "remove-rect") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: remove-rect <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "REMOVE_RECT %s", argv[3]);
    }
    else if (strcmp(cmd, "overlay") == 0) {
        if (argc < 5) { fprintf(stderr, "Usage: overlay <id> <x> <y> [w] [h] [align] [valign] <path>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "OVERLAY");
        for (int i = 3; i < argc; i++) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[i]);
        }
    }
    else if (strcmp(cmd, "remove-overlay") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: remove-overlay <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "REMOVE_OVERLAY %s", argv[3]);
    }
    else if (strcmp(cmd, "progress") == 0) {
        if (argc < 11) { 
            fprintf(stderr, "Usage: progress <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value>\n"); 
            return 1; 
        }
        pos = snprintf(buf, sizeof(buf), "PROGRESS %s %s %s %s %s %s \"%s\" \"%s\" %s",
            argv[3], argv[4], argv[5], argv[6], argv[7], argv[8], 
            argv[9], argv[10], argv[11]);
    }
    else if (strcmp(cmd, "update") == 0) {
        if (argc < 5) { fprintf(stderr, "Usage: update <id> <value> [text]\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "UPDATE_PROGRESS %s %s", argv[3], argv[4]);
        if (argc > 5) {
            pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
            for (int i = 5; i < argc; i++) {
                if (i > 5) pos += snprintf(buf + pos, sizeof(buf) - pos, " ");
                pos += snprintf(buf + pos, sizeof(buf) - pos, "%s", argv[i]);
            }
        }
    }
    else if (strcmp(cmd, "hide-progress") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: hide-progress <id>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "HIDE_PROGRESS %s", argv[3]);
    }
    else if (strcmp(cmd, "clear") == 0) {
        pos = snprintf(buf, sizeof(buf), "CLEAR");
        if (argc > 3) pos += snprintf(buf + pos, sizeof(buf) - pos, " %s", argv[3]);
    }
    else if (strcmp(cmd, "ready") == 0) {
        pos = snprintf(buf, sizeof(buf), "READY?");
    }
    else if (strcmp(cmd, "relocate") == 0) {
        if (argc < 4) { fprintf(stderr, "Usage: relocate <new_path>\n"); return 1; }
        pos = snprintf(buf, sizeof(buf), "RELOCATE_PIPE %s", argv[3]);
    }
    else if (strcmp(cmd, "exit") == 0) {
        pos = snprintf(buf, sizeof(buf), "EXIT");
    }
    else {
        fprintf(stderr, "Unknown command: %s\n", cmd);
        return 1;
    }
    
    return send_cmd(pipe_path, buf);
}
