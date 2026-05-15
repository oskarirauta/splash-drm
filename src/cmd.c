/*
 * cmd.c - Pipe command parser and handler
 */

#include "splash.h"
#include <ctype.h>

/* ========================================================================
 * Command Helpers
 * ======================================================================== */

static char* trim(char *str) {
    while (isspace((unsigned char)*str)) str++;
    if (!*str) return str;
    char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) *end-- = '\0';
    return str;
}

static char* next_token(char **ptr) {
    if (!ptr || !*ptr) return NULL;
    char *start = *ptr;
    while (*start && isspace((unsigned char)*start)) start++;
    if (!*start) return NULL;
    
    char *end = start;
    while (*end && !isspace((unsigned char)*end)) end++;
    if (*end) {
        *end = '\0';
        *ptr = end + 1;
    } else {
        *ptr = end;
    }
    return start;
}

static int parse_scale_mode_cmd(const char *mode_str, float *custom_scale) {
    if (!mode_str || strcmp(mode_str, "contain") == 0) return SCALE_CONTAIN;
    if (strcmp(mode_str, "cover") == 0) return SCALE_COVER;
    if (strcmp(mode_str, "stretch") == 0) return SCALE_STRETCH;
    if (strcmp(mode_str, "none") == 0) return SCALE_NONE;
    if (strcmp(mode_str, "custom") == 0) return SCALE_CUSTOM;
    return SCALE_CONTAIN;
}

/* ========================================================================
 * Command Handlers
 * ======================================================================== */

static int cmd_exit(splash_state_t *st, char *args) {
    (void)args;
    st->running = 0;
    return 0;
}

static int cmd_clear(splash_state_t *st, char *args) {
    uint32_t color = 0;
    if (args && *args) color = parse_color(trim(args));
    
    drm_buffer_t *buf = &st->drm.buf[st->drm.front_buf ^ 1];
    draw_filled_rect(buf, 0, 0, buf->width, buf->height, color);
    drm_flip(&st->drm);
    return 0;
}

static int cmd_bg_color(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    st->bg_color = parse_color(trim(args));
    st->needs_render = 1;
    return 0;
}

static int cmd_image(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    
    char *path = next_token(&args);
    if (!path) return -1;
    
    free_image(&st->bg_image);
    st->bg_loaded = 0;
    
    if (load_image(path, &st->bg_image) < 0) return -1;
    
    st->bg_loaded = 1;
    st->bg_scale_mode = SCALE_CONTAIN;
    st->bg_custom_scale = 1.0f;
    
    if (args && *args) {
        char *mode = next_token(&args);
        st->bg_scale_mode = parse_scale_mode_cmd(mode, &st->bg_custom_scale);
        if (st->bg_scale_mode == SCALE_CUSTOM && args && *args) {
            char *scale = next_token(&args);
            if (scale) st->bg_custom_scale = atof(scale);
        }
    }
    
    st->needs_render = 1;
    return 0;
}

static int cmd_text(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    
    char *id_str = next_token(&args);
    char *x_str = next_token(&args);
    char *y_str = next_token(&args);
    char *align_str = next_token(&args);
    char *color_str = next_token(&args);
    if (!id_str || !x_str || !y_str || !align_str || !color_str) return -1;
    
    int id = atoi(id_str);
    text_element_t *te = text_find(st, id);
    if (!te) te = text_alloc(st);
    if (!te) return -1;
    
    te->id = id;
    te->x = atoi(x_str);
    te->y = atoi(y_str);
    te->align = (align_str[0] == 'C' || align_str[0] == 'c') ? ALIGN_CENTER :
               (align_str[0] == 'R' || align_str[0] == 'r') ? ALIGN_RIGHT : ALIGN_LEFT;
    te->color = parse_color(color_str);
    
    /* The rest of the line is the text, optionally followed by font_slot and font_size */
    if (args && *args) {
        char *text_start = args;
        /* Check if there are font parameters at the end */
        char *last_token = strrchr(args, ' ');
        if (last_token) {
            /* Try to parse last two tokens as font_slot and font_size */
            char *saveptr;
            char *copy = strdup(args);
            char *tok = strtok_r(copy, " ", &saveptr);
            char *prev_tok = NULL;
            char *prev_prev_tok = NULL;
            while (tok) {
                prev_prev_tok = prev_tok;
                prev_tok = tok;
                tok = strtok_r(NULL, " ", &saveptr);
            }
            if (prev_prev_tok && prev_tok) {
                int maybe_slot = atoi(prev_prev_tok);
                float maybe_size = atof(prev_tok);
                if (maybe_slot >= 0 && maybe_slot < MAX_FONTS && maybe_size > 0) {
                    /* Found font parameters */
                    te->font_slot = maybe_slot;
                    te->font_size = maybe_size;
                    /* Remove font params from text */
                    size_t text_len = strlen(args);
                    size_t slot_len = strlen(prev_prev_tok);
                    size_t size_len = strlen(prev_tok);
                    if (text_len > slot_len + size_len + 2) {
                        args[text_len - slot_len - size_len - 2] = '\0';
                    }
                } else {
                    strncpy(te->text, trim(args), sizeof(te->text) - 1);
                }
            } else {
                strncpy(te->text, trim(args), sizeof(te->text) - 1);
            }
            free(copy);
        } else {
            strncpy(te->text, trim(args), sizeof(te->text) - 1);
        }
    }
    
    te->text[sizeof(te->text) - 1] = '\0';
    te->active = 1;
    st->needs_render = 1;
    return 0;
}

static int cmd_remove_text(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    int id = atoi(args);
    text_element_t *te = text_find(st, id);
    if (te) {
        te->active = 0;
        te->text[0] = '\0';
        st->needs_render = 1;
    }
    return 0;
}

static int cmd_rect(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    
    char *id_str = next_token(&args);
    char *x_str = next_token(&args);
    char *y_str = next_token(&args);
    char *w_str = next_token(&args);
    char *h_str = next_token(&args);
    char *color_str = next_token(&args);
    if (!id_str || !x_str || !y_str || !w_str || !h_str || !color_str) return -1;
    
    int id = atoi(id_str);
    rect_element_t *re = rect_find(st, id);
    if (!re) re = rect_alloc(st);
    if (!re) return -1;
    
    re->id = id;
    re->x = atoi(x_str);
    re->y = atoi(y_str);
    re->w = atoi(w_str);
    re->h = atoi(h_str);
    re->color = parse_color(color_str);
    re->blend = (args && strstr(args, "blend")) ? 1 : 0;
    re->active = 1;
    st->needs_render = 1;
    return 0;
}

static int cmd_remove_rect(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    int id = atoi(args);
    rect_element_t *re = rect_find(st, id);
    if (re) {
        re->active = 0;
        st->needs_render = 1;
    }
    return 0;
}

static int cmd_overlay(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    
    char *id_str = next_token(&args);
    char *x_str = next_token(&args);
    char *y_str = next_token(&args);
    if (!id_str || !x_str || !y_str) return -1;
    
    int id = atoi(id_str);
    image_overlay_t *ov = overlay_find(st, id);
    if (!ov) ov = overlay_alloc(st);
    if (!ov) return -1;
    
    ov->id = id;
    ov->x = atoi(x_str);
    ov->y = atoi(y_str);
    ov->w = 0;
    ov->h = 0;
    ov->align = ALIGN_LEFT;
    ov->valign = VALIGN_TOP;
    
    /* Parse optional args: [w] [h] [align] [valign] <path> */
    char *path = NULL;
    
    if (args && *args) {
        char *tok1 = next_token(&args);
        if (!tok1) return -1;
        
        /* Check if tok1 is a number (w) or path */
        int is_w = 1;
        for (char *p = tok1; *p; p++) {
            if (!isdigit((unsigned char)*p)) { is_w = 0; break; }
        }
        
        if (is_w) {
            ov->w = atoi(tok1);
            if (args && *args) {
                char *tok2 = next_token(&args);
                if (tok2) {
                    int is_h = 1;
                    for (char *p = tok2; *p; p++) {
                        if (!isdigit((unsigned char)*p)) { is_h = 0; break; }
                    }
                    if (is_h) {
                        ov->h = atoi(tok2);
                        if (args && *args) {
                            char *tok3 = next_token(&args);
                            if (tok3) {
                                if (tok3[0] == 'C' || tok3[0] == 'c') ov->align = ALIGN_CENTER;
                                else if (tok3[0] == 'R' || tok3[0] == 'r') ov->align = ALIGN_RIGHT;
                                
                                if (args && *args) {
                                    char *tok4 = next_token(&args);
                                    if (tok4) {
                                        if (tok4[0] == 'M' || tok4[0] == 'm') ov->valign = VALIGN_MIDDLE;
                                        else if (tok4[0] == 'B' || tok4[0] == 'b') ov->valign = VALIGN_BOTTOM;
                                        
                                        if (args && *args) path = trim(args);
                                    } else {
                                        path = tok3;
                                    }
                                }
                            }
                        }
                    } else {
                        path = tok2;
                    }
                }
            }
        } else {
            path = tok1;
        }
    }
    
    if (!path) return -1;
    
    free_image(&ov->img);
    if (load_image(path, &ov->img) < 0) return -1;
    
    ov->active = 1;
    st->needs_render = 1;
    return 0;
}

static int cmd_remove_overlay(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    int id = atoi(args);
    image_overlay_t *ov = overlay_find(st, id);
    if (ov) {
        free_image(&ov->img);
        ov->active = 0;
        st->needs_render = 1;
    }
    return 0;
}

static int cmd_progress(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    
    char *id_str = next_token(&args);
    char *x_str = next_token(&args);
    char *y_str = next_token(&args);
    char *w_str = next_token(&args);
    char *h_str = next_token(&args);
    char *style_str = next_token(&args);
    char *prefix = next_token(&args);
    char *suffix = next_token(&args);
    char *value_str = next_token(&args);
    if (!id_str || !x_str || !y_str || !w_str || !h_str || !style_str ||
        !prefix || !suffix || !value_str) return -1;
    
    int id = atoi(id_str);
    progress_bar_t *pb = NULL;
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active && st->bars[i].id == id) { pb = &st->bars[i]; break; }
    }
    if (!pb) {
        for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
            if (!st->bars[i].active) { pb = &st->bars[i]; break; }
        }
    }
    if (!pb) return -1;
    
    pb->id = id;
    pb->x = atoi(x_str);
    pb->y = atoi(y_str);
    pb->w = atoi(w_str);
    pb->h = atoi(h_str);
    pb->style = atoi(style_str);
    strncpy(pb->prefix, prefix, sizeof(pb->prefix) - 1);
    strncpy(pb->suffix, suffix, sizeof(pb->suffix) - 1);
    pb->value = atof(value_str);
    pb->inner[0] = '\0';
    pb->font_slot = 0;
    pb->font_size = 0;
    set_default_progress_colors(pb, pb->style);
    pb->active = 1;
    st->needs_render = 1;
    return 0;
}

static int cmd_update_progress(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    
    char *id_str = next_token(&args);
    char *value_str = next_token(&args);
    if (!id_str || !value_str) return -1;
    
    int id = atoi(id_str);
    progress_bar_t *pb = NULL;
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active && st->bars[i].id == id) { pb = &st->bars[i]; break; }
    }
    if (!pb) return -1;
    
    pb->value = atof(value_str);
    if (args && *args) {
        strncpy(pb->inner, trim(args), sizeof(pb->inner) - 1);
        pb->inner[sizeof(pb->inner) - 1] = '\0';
    }
    st->needs_render = 1;
    return 0;
}

static int cmd_hide_progress(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    int id = atoi(args);
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active && st->bars[i].id == id) {
            st->bars[i].active = 0;
            st->needs_render = 1;
            break;
        }
    }
    return 0;
}

static int cmd_relocate_pipe(splash_state_t *st, char *args) {
    if (!args || !*args) return -1;
    return pipe_reopen(st, trim(args));
}

static int cmd_ready(splash_state_t *st, char *args) {
    (void)args;
    /* Could write to pipe or set a flag */
    st->ready = 1;
    return 0;
}

/* ========================================================================
 * Command Dispatch
 * ======================================================================== */

typedef struct {
    const char *name;
    int (*handler)(splash_state_t *, char *);
} cmd_entry_t;

static const cmd_entry_t cmd_table[] = {
    {"EXIT", cmd_exit},
    {"CLEAR", cmd_clear},
    {"BG_COLOR", cmd_bg_color},
    {"IMAGE", cmd_image},
    {"TEXT", cmd_text},
    {"REMOVE_TEXT", cmd_remove_text},
    {"RECT", cmd_rect},
    {"REMOVE_RECT", cmd_remove_rect},
    {"OVERLAY", cmd_overlay},
    {"REMOVE_OVERLAY", cmd_remove_overlay},
    {"PROGRESS", cmd_progress},
    {"UPDATE_PROGRESS", cmd_update_progress},
    {"HIDE_PROGRESS", cmd_hide_progress},
    {"RELOCATE_PIPE", cmd_relocate_pipe},
    {"READY?", cmd_ready},
    {NULL, NULL}
};

int handle_command(splash_state_t *st, const char *cmdline) {
    char buf[CMD_MAX_LEN];
    strncpy(buf, cmdline, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = '\0';
    
    char *cmd = trim(buf);
    if (!*cmd) return 0;
    
    /* Find first space to separate command from args */
    char *args = strchr(cmd, ' ');
    if (args) {
        *args = '\0';
        args++;
        while (isspace((unsigned char)*args)) args++;
    }
    
    for (int i = 0; cmd_table[i].name; i++) {
        if (strcmp(cmd, cmd_table[i].name) == 0) {
            return cmd_table[i].handler(st, args);
        }
    }
    
    if (!st->quiet && st->debug)
        fprintf(stderr, "Unknown command: %s\n", cmd);
    return -1;
}
