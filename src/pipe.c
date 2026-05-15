/*
 * pipe.c - Named pipe (FIFO) management
 * 
 * Handles pipe creation, reading, and relocation for initrd->rootfs transition.
 */

#include "splash.h"

/* ========================================================================
 * Pipe Creation
 * ======================================================================== */

int pipe_create(const char *path) {
    if (!path) return -1;
    
    unlink(path);
    if (mkfifo(path, 0600) < 0 && errno != EEXIST) {
        fprintf(stderr, "Failed to create pipe %s: %s\n", path, strerror(errno));
        return -1;
    }
    
    int fd = open(path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        fprintf(stderr, "Failed to open pipe %s: %s\n", path, strerror(errno));
        return -1;
    }
    
    return fd;
}

/* ========================================================================
 * Pipe Relocation (for initrd -> rootfs transition)
 * ======================================================================== */

int pipe_reopen(splash_state_t *st, const char *new_path) {
    if (!st || !new_path) return -1;
    
    /* Close old pipe */
    if (st->pipe_fd >= 0) {
        close(st->pipe_fd);
    }
    unlink(st->pipe_path);
    
    /* Create new pipe */
    strncpy(st->pipe_path, new_path, 255);
    st->pipe_path[255] = 0;
    
    st->pipe_fd = pipe_create(st->pipe_path);
    return st->pipe_fd;
}

/* ========================================================================
 * Command Reading
 * ======================================================================== */

int pipe_read_command(splash_state_t *st, char *buf, int max_len) {
    if (!st || !buf || max_len <= 0) return -1;
    
    static char cmdbuf[CMD_MAX_LEN];
    static int cmdpos = 0;
    
    char tmp[512];
    int n = read(st->pipe_fd, tmp, sizeof(tmp));
    
    if (n > 0) {
        for (int i = 0; i < n; i++) {
            if (tmp[i] == '\n') {
                cmdbuf[cmdpos] = 0;
                strncpy(buf, cmdbuf, max_len - 1);
                buf[max_len - 1] = 0;
                cmdpos = 0;
                return (int)strlen(buf);
            } else if (cmdpos < CMD_MAX_LEN - 1) {
                cmdbuf[cmdpos++] = tmp[i];
            }
        }
        return 0; /* Partial command, need more data */
    } else if (n == 0) {
        /* Writer closed - reopen to wait for next writer */
        close(st->pipe_fd);
        st->pipe_fd = open(st->pipe_path, O_RDONLY | O_NONBLOCK);
        return 0;
    }
    
    return n; /* Error or would block */
}
