/*
 * cmd.c - JSON command parser and handler
 */

#include "splash.h"

/* ========================================================================
 * JSON Helpers
 * ======================================================================== */

cJSON* create_response(const char *status, const char *message) {
    cJSON *resp = cJSON_CreateObject();
    cJSON_AddStringToObject(resp, "status", status);
    if (message) {
        cJSON_AddStringToObject(resp, "message", message);
    }
    return resp;
}

static void send_response(splash_state_t *st, int client_idx, cJSON *resp) {
    if (client_idx < 0) {
        /* Startup commands, no client to reply to */
        cJSON_Delete(resp);
        return;
    }
    /* socket_reply_json serialises and writes the response itself. */
    socket_reply_json(st, client_idx, resp);
    cJSON_Delete(resp);
}

int get_int(cJSON *obj, const char *key, int default_val) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) return item->valueint;
    return default_val;
}

float get_float(cJSON *obj, const char *key, float default_val) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsNumber(item)) return (float)item->valuedouble;
    return default_val;
}

const char* get_string(cJSON *obj, const char *key, const char *default_val) {
    cJSON *item = cJSON_GetObjectItem(obj, key);
    if (item && cJSON_IsString(item)) return item->valuestring;
    return default_val;
}

uint32_t get_color(cJSON *obj, const char *key, uint32_t default_val) {
    const char *str = get_string(obj, key, NULL);
    if (str) return parse_color(str);
    return default_val;
}

int get_align(cJSON *obj, const char *key, int default_val) {
    const char *str = get_string(obj, key, NULL);
    if (!str) return default_val;
    if (strcasecmp(str, "center") == 0 || strcasecmp(str, "c") == 0) return ALIGN_CENTER;
    if (strcasecmp(str, "right") == 0 || strcasecmp(str, "r") == 0) return ALIGN_RIGHT;
    return ALIGN_LEFT;
}

int get_valign(cJSON *obj, const char *key, int default_val) {
    const char *str = get_string(obj, key, NULL);
    if (!str) return default_val;
    if (strcasecmp(str, "middle") == 0 || strcasecmp(str, "m") == 0) return VALIGN_MIDDLE;
    if (strcasecmp(str, "bottom") == 0 || strcasecmp(str, "b") == 0) return VALIGN_BOTTOM;
    return VALIGN_TOP;
}

int get_scale_mode(cJSON *obj, const char *key, int default_val) {
    const char *str = get_string(obj, key, NULL);
    if (!str) return default_val;
    if (strcasecmp(str, "cover") == 0) return SCALE_COVER;
    if (strcasecmp(str, "stretch") == 0) return SCALE_STRETCH;
    if (strcasecmp(str, "none") == 0) return SCALE_NONE;
    if (strcasecmp(str, "custom") == 0) return SCALE_CUSTOM;
    return SCALE_CONTAIN;
}

/* Gradient direction: "vertical" | "horizontal" | "diagonal" | "none" */
static int get_gradient(cJSON *obj, const char *key, int default_val) {
    const char *str = get_string(obj, key, NULL);
    if (!str) return default_val;
    if (strcasecmp(str, "vertical")   == 0 || strcasecmp(str, "v") == 0) return GRAD_VERTICAL;
    if (strcasecmp(str, "horizontal") == 0 || strcasecmp(str, "h") == 0) return GRAD_HORIZONTAL;
    if (strcasecmp(str, "diagonal")   == 0 || strcasecmp(str, "d") == 0) return GRAD_DIAGONAL;
    if (strcasecmp(str, "none")       == 0) return GRAD_NONE;
    return default_val;
}

/* Image resample quality: "nearest" | "bilinear" | "bicubic" | "lanczos" */
static int get_filter(cJSON *obj, const char *key, int default_val) {
    const char *str = get_string(obj, key, NULL);
    if (!str) return default_val;
    if (strcasecmp(str, "nearest")  == 0) return IMG_NEAREST;
    if (strcasecmp(str, "bilinear") == 0) return IMG_BILINEAR;
    if (strcasecmp(str, "bicubic")  == 0) return IMG_BICUBIC;
    if (strcasecmp(str, "lanczos")  == 0) return IMG_LANCZOS;
    return default_val;
}

/* ========================================================================
 * Command Handlers
 * ======================================================================== */

static int cmd_exit(splash_state_t *st, cJSON *args, int client_idx) {
    (void)args;
    send_response(st, client_idx, create_response("ok", NULL));
    st->running = 0;
    return 0;
}

static int cmd_suspend(splash_state_t *st, cJSON *args, int client_idx) {
    (void)args;
    st->frozen = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    if (st->debug) fprintf(stderr, "[debug] SUSPEND\n");
    return 0;
}

static int cmd_resume(splash_state_t *st, cJSON *args, int client_idx) {
    (void)args;
    st->frozen = 0;
    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    if (st->debug) fprintf(stderr, "[debug] RESUME\n");
    return 0;
}

static int cmd_status(splash_state_t *st, cJSON *args, int client_idx) {
    (void)args;
    cJSON *resp = create_response("ok", NULL);
    cJSON_AddStringToObject(resp, "state", st->frozen ? "suspended" : "running");
    cJSON_AddBoolToObject(resp, "ready", st->ready);
    send_response(st, client_idx, resp);
    return 0;
}

static int cmd_clear(splash_state_t *st, cJSON *args, int client_idx) {
    uint32_t color = get_color(args, "color", 0);
    drm_buffer_t *buf = &st->drm.buf[st->drm.front_buf ^ 1];
    draw_filled_rect(buf, 0, 0, buf->width, buf->height, color);
    drm_flip(&st->drm);
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_bg_color(splash_state_t *st, cJSON *args, int client_idx) {
    st->bg_color = get_color(args, "color", 0);
    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_image(splash_state_t *st, cJSON *args, int client_idx) {
    const char *path = get_string(args, "path", NULL);
    if (!path) {
        send_response(st, client_idx, create_response("error", "missing path"));
        return -1;
    }

    free_image(&st->bg_image);
    st->bg_loaded = 0;

    if (load_image(path, &st->bg_image) < 0) {
        send_response(st, client_idx, create_response("error", "failed to load image"));
        return -1;
    }

    st->bg_loaded = 1;
    st->bg_scale_mode = get_scale_mode(args, "mode", SCALE_CONTAIN);
    st->bg_custom_scale = get_float(args, "scale", 1.0f);
    st->bg_filter = get_filter(args, "filter", IMG_LANCZOS);

    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_text(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    if (id < 0) {
        send_response(st, client_idx, create_response("error", "missing id"));
        return -1;
    }

    text_element_t *te = text_find(st, id);
    if (!te) te = text_alloc(st);
    if (!te) {
        send_response(st, client_idx, create_response("error", "no slots"));
        return -1;
    }

    te->id = id;
    te->x = get_int(args, "x", 0);
    te->y = get_int(args, "y", 0);
    te->align = get_align(args, "align", ALIGN_LEFT);
    te->valign = get_valign(args, "valign", VALIGN_TOP);
    te->color = get_color(args, "color", rgb(255, 255, 255));
    te->font_slot = get_int(args, "font", 0);
    te->font_size = get_float(args, "size", 0);

    /* Soft drop shadow: enabled with "shadow": true. */
    te->shadow = cJSON_IsTrue(cJSON_GetObjectItem(args, "shadow"));
    te->shadow_dx = get_int(args, "shadow_dx", 2);
    te->shadow_dy = get_int(args, "shadow_dy", 2);
    te->shadow_blur = get_int(args, "shadow_blur", 4);
    te->shadow_color = get_color(args, "shadow_color", argb(160, 0, 0, 0));

    const char *text = get_string(args, "text", "");
    strncpy(te->text, text, sizeof(te->text) - 1);
    te->text[sizeof(te->text) - 1] = '\0';

    te->active = 1;
    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_remove_text(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    text_element_t *te = text_find(st, id);
    if (te) {
        te->active = 0;
        te->text[0] = '\0';
        st->needs_render = 1;
    }
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_rect(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    if (id < 0) {
        send_response(st, client_idx, create_response("error", "missing id"));
        return -1;
    }

    rect_element_t *re = rect_find(st, id);
    if (!re) re = rect_alloc(st);
    if (!re) {
        send_response(st, client_idx, create_response("error", "no slots"));
        return -1;
    }

    re->id = id;
    re->x = get_int(args, "x", 0);
    re->y = get_int(args, "y", 0);
    re->w = get_int(args, "w", 0);
    re->h = get_int(args, "h", 0);
    re->color = get_color(args, "color", rgb(255, 255, 255));
    re->blend = cJSON_IsTrue(cJSON_GetObjectItem(args, "blend"));
    re->fill = cJSON_IsTrue(cJSON_GetObjectItem(args, "fill"));
    re->radius = get_int(args, "radius", 0);
    re->border_color = get_color(args, "border_color", re->color);
    re->border_width = get_int(args, "border_width", 0);

    /* Gradient fill: "gradient" picks the direction, "grad_color" the
     * second stop. The first stop is always "color". */
    re->grad_dir = get_gradient(args, "gradient", GRAD_NONE);
    re->grad_color = get_color(args, "grad_color", re->color);

    /* Soft drop shadow: enabled with "shadow": true. */
    re->shadow = cJSON_IsTrue(cJSON_GetObjectItem(args, "shadow"));
    re->shadow_dx = get_int(args, "shadow_dx", 4);
    re->shadow_dy = get_int(args, "shadow_dy", 6);
    re->shadow_blur = get_int(args, "shadow_blur", 10);
    re->shadow_color = get_color(args, "shadow_color", argb(130, 0, 0, 0));

    re->active = 1;
    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_remove_rect(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    rect_element_t *re = rect_find(st, id);
    if (re) {
        re->active = 0;
        st->needs_render = 1;
    }
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_overlay(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    if (id < 0) {
        send_response(st, client_idx, create_response("error", "missing id"));
        return -1;
    }

    image_overlay_t *ov = overlay_find(st, id);
    if (!ov) ov = overlay_alloc(st);
    if (!ov) {
        send_response(st, client_idx, create_response("error", "no slots"));
        return -1;
    }

    ov->id = id;
    ov->x = get_int(args, "x", 0);
    ov->y = get_int(args, "y", 0);
    ov->w = get_int(args, "w", 0);
    ov->h = get_int(args, "h", 0);
    ov->align = get_align(args, "align", ALIGN_LEFT);
    ov->valign = get_valign(args, "valign", VALIGN_TOP);
    ov->filter = get_filter(args, "filter", IMG_LANCZOS);

    const char *path = get_string(args, "path", NULL);
    if (!path) {
        send_response(st, client_idx, create_response("error", "missing path"));
        return -1;
    }

    free_image(&ov->img);
    if (load_image(path, &ov->img) < 0) {
        send_response(st, client_idx, create_response("error", "failed to load image"));
        return -1;
    }

    ov->active = 1;
    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_remove_overlay(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    image_overlay_t *ov = overlay_find(st, id);
    if (ov) {
        free_image(&ov->img);
        ov->active = 0;
        st->needs_render = 1;
    }
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_progress(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    if (id < 0) {
        send_response(st, client_idx, create_response("error", "missing id"));
        return -1;
    }

    progress_bar_t *pb = NULL;
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active && st->bars[i].id == id) { pb = &st->bars[i]; break; }
    }
    if (!pb) {
        for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
            if (!st->bars[i].active) {
                pb = &st->bars[i];
                memset(pb, 0, sizeof(*pb));   /* no stale fields on a reused slot */
                break;
            }
        }
    }
    if (!pb) {
        send_response(st, client_idx, create_response("error", "no slots"));
        return -1;
    }

    pb->id = id;
    pb->x = get_int(args, "x", -1);
    pb->y = get_int(args, "y", -1);
    pb->w = get_int(args, "w", 100);
    pb->h = get_int(args, "h", 20);
    pb->align = get_align(args, "align", ALIGN_CENTER);
    pb->valign = get_valign(args, "valign", VALIGN_MIDDLE);
    pb->style = get_int(args, "style", 0);
    pb->value = get_float(args, "value", 0);
    pb->borderless = cJSON_IsTrue(cJSON_GetObjectItem(args, "borderless"));
    pb->border_width = get_int(args, "border_width", 2);
    pb->radius = get_int(args, "radius", 0);
    pb->font_slot = get_int(args, "font", 0);
    pb->font_size = get_float(args, "size", 0);
    pb->show_percent = cJSON_IsTrue(cJSON_GetObjectItem(args, "show_percent"));

    /* Colors: if provided, set style to -1 (custom) */
    int has_custom_color = 0;
    cJSON *bg = cJSON_GetObjectItem(args, "bg_color");
    cJSON *bar = cJSON_GetObjectItem(args, "bar_color");
    cJSON *border = cJSON_GetObjectItem(args, "border_color");
    cJSON *text = cJSON_GetObjectItem(args, "text_color");

    if (bg || bar || border || text) {
        has_custom_color = 1;
        pb->style = -1;
    }

    set_default_progress_colors(pb, pb->style);

    /* Override with custom colors if provided */
    if (has_custom_color) {
        if (bg) pb->bg_color = parse_color(bg->valuestring);
        if (bar) pb->bar_color = parse_color(bar->valuestring);
        if (border) pb->border_color = parse_color(border->valuestring);
        if (text) pb->text_color = parse_color(text->valuestring);
    }

    /* Gradient for the fill: "gradient" picks the direction, "bar_color2"
     * the second stop. The first stop is the resolved bar_color. */
    pb->bar_gradient = get_gradient(args, "gradient", GRAD_NONE);
    pb->bar_color2 = get_color(args, "bar_color2", pb->bar_color);

    /* Soft drop shadow of the whole bar */
    pb->shadow = cJSON_IsTrue(cJSON_GetObjectItem(args, "shadow"));
    pb->shadow_dx = get_int(args, "shadow_dx", 0);
    pb->shadow_dy = get_int(args, "shadow_dy", 4);
    pb->shadow_blur = get_int(args, "shadow_blur", 12);
    pb->shadow_color = get_color(args, "shadow_color", argb(120, 0, 0, 0));

    pb->active = 1;
    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_update_progress(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    progress_bar_t *pb = NULL;
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active && st->bars[i].id == id) { pb = &st->bars[i]; break; }
    }
    if (!pb) {
        send_response(st, client_idx, create_response("error", "not found"));
        return -1;
    }

    pb->value = get_float(args, "value", 0);

    /* Update show_percent if provided */
    cJSON *sp = cJSON_GetObjectItem(args, "show_percent");
    if (sp) pb->show_percent = cJSON_IsTrue(sp);

    st->needs_render = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_hide_progress(splash_state_t *st, cJSON *args, int client_idx) {
    int id = get_int(args, "id", -1);
    for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
        if (st->bars[i].active && st->bars[i].id == id) {
            st->bars[i].active = 0;
            st->needs_render = 1;
            break;
        }
    }
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

static int cmd_ready(splash_state_t *st, cJSON *args, int client_idx) {
    (void)args;
    st->ready = 1;
    send_response(st, client_idx, create_response("ok", NULL));
    return 0;
}

/* ========================================================================
 * Command Dispatch
 * ======================================================================== */

typedef struct {
    const char *name;
    int (*handler)(splash_state_t *, cJSON *, int);
} cmd_entry_t;

static const cmd_entry_t cmd_table[] = {
    {"exit", cmd_exit},
    {"suspend", cmd_suspend},
    {"resume", cmd_resume},
    {"status", cmd_status},
    {"clear", cmd_clear},
    {"bg_color", cmd_bg_color},
    {"image", cmd_image},
    {"text", cmd_text},
    {"remove_text", cmd_remove_text},
    {"rect", cmd_rect},
    {"remove_rect", cmd_remove_rect},
    {"overlay", cmd_overlay},
    {"remove_overlay", cmd_remove_overlay},
    {"progress", cmd_progress},
    {"update_progress", cmd_update_progress},
    {"hide_progress", cmd_hide_progress},
    {"ready", cmd_ready},
    {NULL, NULL}
};

/* Process a single command object */
static int process_single_cmd(splash_state_t *st, cJSON *cmd_obj, int client_idx) {
    const char *cmd_name = get_string(cmd_obj, "cmd", NULL);
    if (!cmd_name) {
        if (client_idx >= 0) {
            send_response(st, client_idx, create_response("error", "missing cmd"));
        }
        return -1;
    }

    for (int i = 0; cmd_table[i].name; i++) {
        if (strcmp(cmd_name, cmd_table[i].name) == 0) {
            return cmd_table[i].handler(st, cmd_obj, client_idx);
        }
    }

    if (client_idx >= 0) {
        send_response(st, client_idx, create_response("error", "unknown command"));
    } else if (st->debug) {
        fprintf(stderr, "[debug] Unknown startup command: %s\n", cmd_name);
    }
    return -1;
}

/* Process batch (array or single object) - used by both socket and startup */
int process_json_batch(splash_state_t *st, cJSON *root, int client_idx) {
    if (cJSON_IsArray(root)) {
        /* Batch mode */
        int count = cJSON_GetArraySize(root);
        int errors = 0;

        for (int i = 0; i < count; i++) {
            cJSON *cmd = cJSON_GetArrayItem(root, i);
            if (!cJSON_IsObject(cmd)) continue;
            if (process_single_cmd(st, cmd, client_idx) < 0) errors++;
        }

        if (client_idx >= 0) {
            cJSON *resp = create_response(errors > 0 ? "partial" : "ok", NULL);
            cJSON_AddNumberToObject(resp, "total", count);
            cJSON_AddNumberToObject(resp, "errors", errors);
            send_response(st, client_idx, resp);
        }
        return errors > 0 ? -1 : 0;

    } else if (cJSON_IsObject(root)) {
        /* Single command */
        return process_single_cmd(st, root, client_idx);
    }

    if (client_idx >= 0) {
        send_response(st, client_idx, create_response("error", "invalid command format"));
    }
    return -1;
}

/* Entry point from socket */
int handle_json_command(splash_state_t *st, const char *json_str, int client_idx) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) {
        send_response(st, client_idx, create_response("error", "invalid json"));
        return -1;
    }

    int ret = process_json_batch(st, root, client_idx);
    cJSON_Delete(root);
    return ret;
}
