/*
 * cmd.c - Command protocol implementation
 * 
 * Parses and executes all commands received via the named pipe.
 * Each command is a single line of text ending with newline.
 */

#include "splash.h"

/* ========================================================================
 * Command: EXIT
 * ======================================================================== */

static int cmd_exit(splash_state_t *st, char *args) {
    (void)args;
    st->running = 0;
    return 1;
}

/* ========================================================================
 * Command: RELOCATE_PIPE <new_path>
 * ======================================================================== */

static int cmd_relocate_pipe(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *newpath = args;
    /* Trim leading whitespace */
    while (*newpath == ' ' || *newpath == '\t') newpath++;
    
    if (pipe_reopen(st, newpath) < 0) {
        fprintf(stderr, "Failed to relocate pipe to %s\n", newpath);
    }
    return 0;
}

/* ========================================================================
 * Command: READY?
 * ======================================================================== */

static int cmd_ready(splash_state_t *st, char *args) {
    (void)args;
    /* Write response to a status file or stdout */
    /* For now, just set a flag that can be checked */
    st->ready = 1;
    return 0;
}

/* ========================================================================
 * Command: CLEAR [#RRGGBB]
 * ======================================================================== */

static int cmd_clear(splash_state_t *st, char *args) {
    uint32_t color = argb(255, 0, 0, 0); /* Default: black */
    
    if (args && *args) {
        while (*args == ' ' || *args == '\t') args++;
        color = parse_color(args);
    }
    
    /* Clear all elements */
    clear_all_elements(st);
    
    /* Fill background with color */
    drm_buffer_t *buf = &st->drm.buf[st->drm.front_buf ^ 1];
    for (uint32_t row = 0; row < buf->height; row++) {
        uint32_t *line = (uint32_t*)(buf->map + row * buf->pitch);
        for (uint32_t col = 0; col < buf->width; col++) {
            line[col] = color;
        }
    }
    drm_flip(&st->drm);
    st->needs_render = 0;
    return 0;
}

/* ========================================================================
 * Command: IMAGE <path> [mode]
 * ======================================================================== */

static int cmd_image(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *path = strtok(args, " \t");
    char *mode_str = strtok(NULL, " \t\n");
    
    if (!path) return 0;
    
    free_image(&st->bg_image);
    st->bg_loaded = 0;
    
    if (load_image(path, &st->bg_image) == 0) {
        st->bg_loaded = 1;
        st->bg_scale_mode = SCALE_COVER;
        
        if (mode_str) {
            if (strcmp(mode_str, "contain") == 0) st->bg_scale_mode = SCALE_CONTAIN;
            else if (strcmp(mode_str, "stretch") == 0) st->bg_scale_mode = SCALE_STRETCH;
            else if (strcmp(mode_str, "none") == 0) st->bg_scale_mode = SCALE_NONE;
        }
    }
    st->needs_render = 1;
    return 0;
}

/* ========================================================================
 * Command: TEXT <id> <x> <y> <align> <color> <text...>
 * ======================================================================== */

static int cmd_text(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t");
    char *sx = strtok(NULL, " \t");
    char *sy = strtok(NULL, " \t");
    char *align = strtok(NULL, " \t");
    char *color = strtok(NULL, " \t");
    char *text = strtok(NULL, "\n");
    
    if (!sid || !sx || !sy || !align || !color || !text) return 0;
    
    int id = atoi(sid);
    text_element_t *te = text_find(st, id);
    if (!te) te = text_alloc(st);
    
    if (te) {
        te->id = id;
        te->x = atoi(sx);
        te->y = atoi(sy);
        te->align = (align[0] == 'C' || align[0] == 'c') ? ALIGN_CENTER :
                   (align[0] == 'R' || align[0] == 'r') ? ALIGN_RIGHT : ALIGN_LEFT;
        te->color = parse_color(color);
        strncpy(te->text, text, 255);
        te->text[255] = 0;
        te->active = 1;
        st->needs_render = 1;
    }
    return 0;
}

/* ========================================================================
 * Command: REMOVE_TEXT <id>
 * ======================================================================== */

static int cmd_remove_text(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t\n");
    if (!sid) return 0;
    
    int id = atoi(sid);
    text_element_t *te = text_find(st, id);
    if (te) te->active = 0;
    st->needs_render = 1;
    return 0;
}

/* ========================================================================
 * Command: RECT <id> <x> <y> <w> <h> <color> [blend]
 * ======================================================================== */

static int cmd_rect(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t");
    char *sx = strtok(NULL, " \t");
    char *sy = strtok(NULL, " \t");
    char *sw = strtok(NULL, " \t");
    char *sh = strtok(NULL, " \t");
    char *color = strtok(NULL, " \t");
    char *blend_str = strtok(NULL, " \t\n");
    
    if (!sid || !sx || !sy || !sw || !sh || !color) return 0;
    
    int id = atoi(sid);
    rect_element_t *re = rect_find(st, id);
    if (!re) re = rect_alloc(st);
    
    if (re) {
        re->id = id;
        re->x = atoi(sx);
        re->y = atoi(sy);
        re->w = atoi(sw);
        re->h = atoi(sh);
        re->color = parse_color(color);
        re->blend = (blend_str && (blend_str[0] == '1' || blend_str[0] == 'y' || blend_str[0] == 'Y'));
        re->active = 1;
        st->needs_render = 1;
    }
    return 0;
}

/* ========================================================================
 * Command: REMOVE_RECT <id>
 * ======================================================================== */

static int cmd_remove_rect(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t\n");
    if (!sid) return 0;
    
    int id = atoi(sid);
    rect_element_t *re = rect_find(st, id);
    if (re) re->active = 0;
    st->needs_render = 1;
    return 0;
}

/* ========================================================================
 * Command: OVERLAY <id> <x> <y> [w] [h] [align] [valign] <path>
 * ======================================================================== */

static int cmd_overlay(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t");
    char *sx = strtok(NULL, " \t");
    char *sy = strtok(NULL, " \t");
    if (!sid || !sx || !sy) return 0;
    
    /* Parse optional args - remaining token is the path */
    char *sw = strtok(NULL, " \t");
    char *sh = NULL, *align = NULL, *valign = NULL, *path = NULL;
    
    if (sw) {
        /* Check if next token is a number (width) or path */
        char *next = strtok(NULL, " \t");
        if (next) {
            /* If sw starts with / or ., it's actually the path */
            if (sw[0] == '/' || sw[0] == '.') {
                path = sw;
                sw = NULL;
            } else {
                sh = next;
                align = strtok(NULL, " \t");
                if (align) {
                    valign = strtok(NULL, " \t");
                    if (valign) {
                        path = strtok(NULL, "\n");
                    } else {
                        /* align was actually path */
                        path = align;
                        align = NULL;
                    }
                }
            }
        }
    }
    
    if (!path) {
        /* Reconstruct - path is everything after x y */
        char *rest = args;
        /* Skip id x y */
        strtok(rest, " \t"); /* id */
        strtok(NULL, " \t"); /* x */
        strtok(NULL, " \t"); /* y */
        path = strtok(NULL, "\n");
    }
    
    if (!path) return 0;
    
    int id = atoi(sid);
    image_overlay_t *ov = overlay_find(st, id);
    if (!ov) ov = overlay_alloc(st);
    
    if (ov) {
        free_image(&ov->img);
        if (load_image(path, &ov->img) == 0) {
            ov->id = id;
            ov->x = atoi(sx);
            ov->y = atoi(sy);
            ov->w = sw ? atoi(sw) : -1;
            ov->h = sh ? atoi(sh) : -1;
            ov->align = align ? ((align[0] == 'C' || align[0] == 'c') ? ALIGN_CENTER :
                                (align[0] == 'R' || align[0] == 'r') ? ALIGN_RIGHT : ALIGN_LEFT) : ALIGN_LEFT;
            ov->valign = valign ? ((valign[0] == 'M' || valign[0] == 'm') ? VALIGN_MIDDLE :
                                  (valign[0] == 'B' || valign[0] == 'b') ? VALIGN_BOTTOM : VALIGN_TOP) : VALIGN_TOP;
            ov->active = 1;
            st->needs_render = 1;
        }
    }
    return 0;
}

/* ========================================================================
 * Command: REMOVE_OVERLAY <id>
 * ======================================================================== */

static int cmd_remove_overlay(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t\n");
    if (!sid) return 0;
    
    int id = atoi(sid);
    image_overlay_t *ov = overlay_find(st, id);
    if (ov) {
        free_image(&ov->img);
        ov->active = 0;
    }
    st->needs_render = 1;
    return 0;
}

/* ========================================================================
 * Command: PROGRESS <id> <x> <y> <w> <h> <style> <prefix> <suffix> <value>
 * ======================================================================== */

static int cmd_progress(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t");
    if (!sid) return 0;
    
    int id = atoi(sid);
    if (id < 0 || id >= MAX_PROGRESS_BARS) return 0;
    
    progress_bar_t *pb = &st->bars[id];
    memset(pb, 0, sizeof(*pb));
    pb->active = 1;
    pb->id = id;
    
    char *sx = strtok(NULL, " \t");
    char *sy = strtok(NULL, " \t");
    char *sw = strtok(NULL, " \t");
    char *sh = strtok(NULL, " \t");
    char *sstyle = strtok(NULL, " \t");
    char *prefix = strtok(NULL, " \t");
    char *suffix = strtok(NULL, " \t");
    char *sval = strtok(NULL, " \t\n");
    
    if (sx) pb->x = atoi(sx);
    if (sy) pb->y = atoi(sy);
    if (sw) pb->w = atoi(sw);
    if (sh) pb->h = atoi(sh);
    if (sstyle) pb->style = atoi(sstyle);
    if (prefix) { strncpy(pb->prefix, prefix, 127); pb->prefix[127] = 0; }
    if (suffix) { strncpy(pb->suffix, suffix, 15); pb->suffix[15] = 0; }
    if (sval) pb->value = fclamp(atof(sval), 0.0f, 100.0f);
    
    set_default_progress_colors(pb, pb->style);
    st->needs_render = 1;
    return 0;
}

/* ========================================================================
 * Command: UPDATE_PROGRESS <id> <value> [inner_text]
 * ======================================================================== */

static int cmd_update_progress(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t");
    char *sval = strtok(NULL, " \t");
    char *newtext = strtok(NULL, "\n");
    
    if (!sid || !sval) return 0;
    
    int id = atoi(sid);
    if (id < 0 || id >= MAX_PROGRESS_BARS || !st->bars[id].active) return 0;
    
    st->bars[id].value = fclamp(atof(sval), 0.0f, 100.0f);
    if (newtext) {
        while (*newtext == ' ' || *newtext == '\t') newtext++;
        strncpy(st->bars[id].inner, newtext, 127);
        st->bars[id].inner[127] = 0;
    }
    st->needs_render = 1;
    return 0;
}

/* ========================================================================
 * Command: HIDE_PROGRESS <id>
 * ======================================================================== */

static int cmd_hide_progress(splash_state_t *st, char *args) {
    if (!args || !*args) return 0;
    
    char *sid = strtok(args, " \t\n");
    if (!sid) return 0;
    
    int id = atoi(sid);
    if (id >= 0 && id < MAX_PROGRESS_BARS) {
        st->bars[id].active = 0;
        st->needs_render = 1;
    }
    return 0;
}

/* ========================================================================
 * Command Router
 * ======================================================================== */

typedef struct {
    const char *name;
    int (*handler)(splash_state_t *st, char *args);
} cmd_entry_t;

static const cmd_entry_t cmd_table[] = {
    {"EXIT",             cmd_exit},
    {"RELOCATE_PIPE",    cmd_relocate_pipe},
    {"READY?",           cmd_ready},
    {"CLEAR",            cmd_clear},
    {"IMAGE",            cmd_image},
    {"TEXT",             cmd_text},
    {"REMOVE_TEXT",      cmd_remove_text},
    {"RECT",             cmd_rect},
    {"REMOVE_RECT",      cmd_remove_rect},
    {"OVERLAY",          cmd_overlay},
    {"REMOVE_OVERLAY",   cmd_remove_overlay},
    {"PROGRESS",         cmd_progress},
    {"UPDATE_PROGRESS",  cmd_update_progress},
    {"HIDE_PROGRESS",    cmd_hide_progress},
    {NULL, NULL}
};

int handle_command(splash_state_t *st, const char *cmdline) {
    if (!st || !cmdline || !*cmdline) return 0;
    
    char cmd[CMD_MAX_LEN];
    strncpy(cmd, cmdline, CMD_MAX_LEN - 1);
    cmd[CMD_MAX_LEN - 1] = 0;
    
    /* Extract command name */
    char *name = strtok(cmd, " \t");
    if (!name) return 0;
    
    /* Find handler */
    for (int i = 0; cmd_table[i].name; i++) {
        if (strcmp(name, cmd_table[i].name) == 0) {
            /* Pass remaining args */
            char *args = strtok(NULL, "\n");
            return cmd_table[i].handler(st, args);
        }
    }
    
    /* Unknown command */
    fprintf(stderr, "Unknown command: %s\n", name);
    return 0;
}
