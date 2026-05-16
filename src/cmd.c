/*
 * cmd.c - JSON command parser and dispatcher.
 *
 * Commands arrive as JSON objects (or arrays of objects) over the control
 * socket. Each is looked up in cmd_table and handed to its handler, which
 * mutates splash_state_t and sends a JSON reply. The same path also runs
 * the startup commands, with client_idx == -1 meaning "no client to
 * reply to".
 */

#include "splash.h"

/* ========================================================================
 * JSON Helpers
 * ======================================================================== */

cJSON *create_response(const char *status, const char *message) {
	cJSON *resp = cJSON_CreateObject();
	cJSON_AddStringToObject(resp, "status", status);
	if (message)
		cJSON_AddStringToObject(resp, "message", message);
	return resp;
}

/* Send a reply to a client, or just free it for startup commands. */
static void send_response(splash_state_t *st, int client_idx, cJSON *resp) {
	if (client_idx < 0) {
		cJSON_Delete(resp);
		return;
	}
	/* socket_reply_json serialises and writes the response itself. */
	socket_reply_json(st, client_idx, resp);
	cJSON_Delete(resp);
}

int get_int(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (item && cJSON_IsNumber(item))
		return item->valueint;
	return default_val;
}

float get_float(cJSON *obj, const char *key, float default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (item && cJSON_IsNumber(item))
		return (float)item->valuedouble;
	return default_val;
}

const char *get_string(cJSON *obj, const char *key, const char *default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (item && cJSON_IsString(item))
		return item->valuestring;
	return default_val;
}

static uint32_t get_color(cJSON *obj, const char *key, uint32_t default_val) {
	const char *str = get_string(obj, key, NULL);
	return str ? parse_color(str) : default_val;
}

static int get_align(cJSON *obj, const char *key, int default_val) {
	const char *str = get_string(obj, key, NULL);
	if (!str)
		return default_val;
	if (strcasecmp(str, "center") == 0 || strcasecmp(str, "c") == 0)
		return ALIGN_CENTER;
	if (strcasecmp(str, "right") == 0 || strcasecmp(str, "r") == 0)
		return ALIGN_RIGHT;
	return ALIGN_LEFT;
}

static int get_valign(cJSON *obj, const char *key, int default_val) {
	const char *str = get_string(obj, key, NULL);
	if (!str)
		return default_val;
	if (strcasecmp(str, "middle") == 0 || strcasecmp(str, "m") == 0)
		return VALIGN_MIDDLE;
	if (strcasecmp(str, "bottom") == 0 || strcasecmp(str, "b") == 0)
		return VALIGN_BOTTOM;
	return VALIGN_TOP;
}

static int get_scale_mode(cJSON *obj, const char *key, int default_val) {
	const char *str = get_string(obj, key, NULL);
	if (!str)
		return default_val;
	if (strcasecmp(str, "cover")   == 0) return SCALE_COVER;
	if (strcasecmp(str, "stretch") == 0) return SCALE_STRETCH;
	if (strcasecmp(str, "none")    == 0) return SCALE_NONE;
	if (strcasecmp(str, "custom")  == 0) return SCALE_CUSTOM;
	return SCALE_CONTAIN;
}

/* Gradient direction: "vertical" | "horizontal" | "diagonal" | "none". */
static int get_gradient(cJSON *obj, const char *key, int default_val) {
	const char *str = get_string(obj, key, NULL);
	if (!str)
		return default_val;
	if (strcasecmp(str, "vertical")   == 0 || strcasecmp(str, "v") == 0)
		return GRAD_VERTICAL;
	if (strcasecmp(str, "horizontal") == 0 || strcasecmp(str, "h") == 0)
		return GRAD_HORIZONTAL;
	if (strcasecmp(str, "diagonal")   == 0 || strcasecmp(str, "d") == 0)
		return GRAD_DIAGONAL;
	if (strcasecmp(str, "none")       == 0)
		return GRAD_NONE;
	return default_val;
}

/* Image resample quality: "nearest" | "bilinear" | "bicubic" | "lanczos". */
static int get_filter(cJSON *obj, const char *key, int default_val) {
	const char *str = get_string(obj, key, NULL);
	if (!str)
		return default_val;
	if (strcasecmp(str, "nearest")  == 0) return IMG_NEAREST;
	if (strcasecmp(str, "bilinear") == 0) return IMG_BILINEAR;
	if (strcasecmp(str, "bicubic")  == 0) return IMG_BICUBIC;
	if (strcasecmp(str, "lanczos")  == 0) return IMG_LANCZOS;
	return default_val;
}

/* Easing curve: "linear" | "ease_in" | "ease_out" | "ease_in_out". */
static int get_easing(cJSON *obj, const char *key, int default_val) {
	const char *str = get_string(obj, key, NULL);
	if (!str)
		return default_val;
	if (strcasecmp(str, "linear")      == 0) return EASE_LINEAR;
	if (strcasecmp(str, "ease_in")     == 0) return EASE_IN;
	if (strcasecmp(str, "ease_out")    == 0) return EASE_OUT;
	if (strcasecmp(str, "ease_in_out") == 0) return EASE_IN_OUT;
	return default_val;
}

/* ========================================================================
 * Lifecycle Commands
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
	if (st->debug)
		fprintf(stderr, "[debug] SUSPEND\n");
	return 0;
}

static int cmd_resume(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	st->frozen = 0;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	if (st->debug)
		fprintf(stderr, "[debug] RESUME\n");
	return 0;
}

static int cmd_status(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	cJSON *resp = create_response("ok", NULL);
	cJSON_AddStringToObject(resp, "state",
	                        st->frozen ? "suspended" : "running");
	cJSON_AddBoolToObject(resp, "ready", st->ready);
	send_response(st, client_idx, resp);
	return 0;
}

static int cmd_ready(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	st->ready = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/*
 * "clear" is a full reset: every element and the background are dropped,
 * then the backdrop colour is set (defaulting to black). Loaded fonts are
 * kept - they cannot be reloaded over the socket.
 */
static int cmd_clear(splash_state_t *st, cJSON *args, int client_idx) {
	clear_all_elements(st);
	st->bg_color = get_color(args, "color", 0);
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Background Commands
 * ======================================================================== */

static int cmd_bg_color(splash_state_t *st, cJSON *args, int client_idx) {
	st->bg_color = get_color(args, "color", 0);
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/*
 * Set the background image. With "crossfade" > 0 and a background already
 * loaded, the old image is kept as the outgoing layer and the new one
 * fades in over it; otherwise the swap is immediate.
 */
static int cmd_image(splash_state_t *st, cJSON *args, int client_idx) {
	const char *path = get_string(args, "path", NULL);
	if (!path) {
		send_response(st, client_idx,
		              create_response("error", "missing path"));
		return -1;
	}

	/* Load into a temporary first, so a failed load never destroys the
	 * background that is already on screen. */
	image_t newimg = {0};
	if (load_image(path, &newimg) < 0) {
		send_response(st, client_idx,
		              create_response("error", "failed to load image"));
		return -1;
	}

	int   mode      = get_scale_mode(args, "mode", SCALE_CONTAIN);
	float cscale    = get_float(args, "scale", 1.0f);
	int   filter    = get_filter(args, "filter", IMG_LANCZOS);
	int   crossfade = get_int(args, "crossfade", 0);

	if (crossfade > 0 && st->bg_loaded && st->bg_image.rgba) {
		/* Hand the current background off as the outgoing image and
		 * fade the new one in over it. */
		free_image(&st->bg_prev);
		st->bg_prev              = st->bg_image;   /* transfer ownership */
		st->bg_prev_loaded       = 1;
		st->bg_prev_scale_mode   = st->bg_scale_mode;
		st->bg_prev_custom_scale = st->bg_custom_scale;
		st->bg_prev_filter       = st->bg_filter;

		st->bg_image        = newimg;
		st->bg_scale_mode   = mode;
		st->bg_custom_scale = cscale;
		st->bg_filter       = filter;
		st->bg_loaded       = 1;

		st->bg_opacity            = 0.0f;
		st->bg_anim.active        = 1;
		st->bg_anim.from          = 0.0f;
		st->bg_anim.to            = 1.0f;
		st->bg_anim.start_ms      = now_ms();
		st->bg_anim.duration_ms   = (uint32_t)crossfade;
		st->bg_anim.easing        = EASE_IN_OUT;
		st->bg_anim.repeat        = 0;
		st->bg_anim.remove_on_end = 0;
	} else {
		/* Immediate swap. */
		free_image(&st->bg_image);
		free_image(&st->bg_prev);
		st->bg_prev_loaded  = 0;
		st->bg_anim.active  = 0;
		st->bg_image        = newimg;
		st->bg_scale_mode   = mode;
		st->bg_custom_scale = cscale;
		st->bg_filter       = filter;
		st->bg_loaded       = 1;
		st->bg_opacity      = 1.0f;
	}

	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Text Commands
 * ======================================================================== */

static int cmd_text(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	text_element_t *te = text_find(st, id);
	if (!te)
		te = text_alloc(st);
	if (!te) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	te->id        = id;
	te->x         = get_int(args, "x", -1);
	te->y         = get_int(args, "y", -1);
	te->align     = get_align(args, "align", ALIGN_CENTER);
	te->valign    = get_valign(args, "valign", VALIGN_MIDDLE);
	te->color     = get_color(args, "color", rgb(255, 255, 255));
	te->font_slot = get_int(args, "font", 0);
	te->font_size = get_float(args, "size", 0);

	/* Soft drop shadow. */
	te->shadow       = cJSON_IsTrue(cJSON_GetObjectItem(args, "shadow"));
	te->shadow_dx    = get_int(args, "shadow_dx", 2);
	te->shadow_dy    = get_int(args, "shadow_dy", 2);
	te->shadow_blur  = get_int(args, "shadow_blur", 4);
	te->shadow_color = get_color(args, "shadow_color", argb(160, 0, 0, 0));

	/* Master opacity; a fresh command cancels any running animation. */
	te->opacity     = get_float(args, "opacity", 1.0f);
	te->anim.active = 0;

	/* '\n' in the text splits it into multiple stacked lines. */
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
		te->active  = 0;
		te->text[0] = '\0';
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Rectangle Commands
 * ======================================================================== */

static int cmd_rect(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	rect_element_t *re = rect_find(st, id);
	if (!re)
		re = rect_alloc(st);
	if (!re) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	re->id           = id;
	re->x            = get_int(args, "x", -1);
	re->y            = get_int(args, "y", -1);
	re->w            = get_int(args, "w", 0);
	re->h            = get_int(args, "h", 0);
	re->align        = get_align(args, "align", ALIGN_CENTER);
	re->valign       = get_valign(args, "valign", VALIGN_MIDDLE);
	re->color        = get_color(args, "color", rgb(255, 255, 255));
	re->fill         = cJSON_IsTrue(cJSON_GetObjectItem(args, "fill"));
	re->radius       = get_int(args, "radius", 0);
	re->border_color = get_color(args, "border_color", re->color);
	re->border_width = get_int(args, "border_width", 0);

	/* Gradient fill. */
	re->grad_dir   = get_gradient(args, "gradient", GRAD_NONE);
	re->grad_color = get_color(args, "grad_color", re->color);

	/* Soft drop shadow. */
	re->shadow       = cJSON_IsTrue(cJSON_GetObjectItem(args, "shadow"));
	re->shadow_dx    = get_int(args, "shadow_dx", 4);
	re->shadow_dy    = get_int(args, "shadow_dy", 6);
	re->shadow_blur  = get_int(args, "shadow_blur", 10);
	re->shadow_color = get_color(args, "shadow_color", argb(130, 0, 0, 0));

	/* Master opacity; a fresh command cancels any running animation. */
	re->opacity     = get_float(args, "opacity", 1.0f);
	re->anim.active = 0;

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

/* ========================================================================
 * Image Overlay Commands
 * ======================================================================== */

static int cmd_overlay(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	image_overlay_t *ov = overlay_find(st, id);
	if (!ov)
		ov = overlay_alloc(st);
	if (!ov) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	ov->id     = id;
	ov->x      = get_int(args, "x", -1);
	ov->y      = get_int(args, "y", -1);
	ov->w      = get_int(args, "w", 0);
	ov->h      = get_int(args, "h", 0);
	ov->align  = get_align(args, "align", ALIGN_CENTER);
	ov->valign = get_valign(args, "valign", VALIGN_MIDDLE);
	ov->filter = get_filter(args, "filter", IMG_LANCZOS);

	/* Master opacity; a fresh command cancels any running animation. */
	ov->opacity     = get_float(args, "opacity", 1.0f);
	ov->anim.active = 0;

	const char *path = get_string(args, "path", NULL);
	if (!path) {
		send_response(st, client_idx,
		              create_response("error", "missing path"));
		return -1;
	}

	free_image(&ov->img);
	if (load_image(path, &ov->img) < 0) {
		send_response(st, client_idx,
		              create_response("error", "failed to load image"));
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

/* ========================================================================
 * Progress Bar Commands
 * ======================================================================== */

/* Locate an active progress bar by id, or NULL if there is none. */
static progress_bar_t *progress_find(splash_state_t *st, int id) {
	for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
		if (st->bars[i].active && st->bars[i].id == id)
			return &st->bars[i];
	}
	return NULL;
}

static int cmd_progress(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	progress_bar_t *pb = progress_find(st, id);
	if (!pb) {
		/* Claim the first free slot, wiped so no stale fields remain. */
		for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
			if (!st->bars[i].active) {
				pb = &st->bars[i];
				memset(pb, 0, sizeof(*pb));
				break;
			}
		}
	}
	if (!pb) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	pb->id           = id;
	pb->x            = get_int(args, "x", -1);
	pb->y            = get_int(args, "y", -1);
	pb->w            = get_int(args, "w", 100);
	pb->h            = get_int(args, "h", 20);
	pb->align        = get_align(args, "align", ALIGN_CENTER);
	pb->valign       = get_valign(args, "valign", VALIGN_MIDDLE);
	pb->style        = get_int(args, "style", 0);
	pb->value        = get_float(args, "value", 0);
	pb->borderless   = cJSON_IsTrue(cJSON_GetObjectItem(args, "borderless"));
	pb->border_width = get_int(args, "border_width", 2);
	pb->radius       = get_int(args, "radius", 0);
	pb->font_slot    = get_int(args, "font", 0);
	pb->font_size    = get_float(args, "size", 0);
	pb->show_percent = cJSON_IsTrue(cJSON_GetObjectItem(args, "show_percent"));

	/* Any explicit colour switches the bar to a custom style (-1). */
	cJSON *bg     = cJSON_GetObjectItem(args, "bg_color");
	cJSON *bar    = cJSON_GetObjectItem(args, "bar_color");
	cJSON *border = cJSON_GetObjectItem(args, "border_color");
	cJSON *text   = cJSON_GetObjectItem(args, "text_color");
	int has_custom_color = (bg || bar || border || text);
	if (has_custom_color)
		pb->style = -1;

	set_default_progress_colors(pb, pb->style);

	if (has_custom_color) {
		if (bg)     pb->bg_color     = parse_color(bg->valuestring);
		if (bar)    pb->bar_color    = parse_color(bar->valuestring);
		if (border) pb->border_color = parse_color(border->valuestring);
		if (text)   pb->text_color   = parse_color(text->valuestring);
	}

	/* Gradient for the fill. */
	pb->bar_gradient = get_gradient(args, "gradient", GRAD_NONE);
	pb->bar_color2   = get_color(args, "bar_color2", pb->bar_color);

	/* Indeterminate mode: a sweeping highlight instead of a fixed fill,
	 * for when the task has no measurable progress. */
	pb->indeterminate   = cJSON_IsTrue(cJSON_GetObjectItem(args, "indeterminate"));
	pb->indet_period_ms = (uint32_t)get_int(args, "indet_period", 1100);
	pb->indet_start_ms  = now_ms();

	/* Soft drop shadow of the whole bar. */
	pb->shadow       = cJSON_IsTrue(cJSON_GetObjectItem(args, "shadow"));
	pb->shadow_dx    = get_int(args, "shadow_dx", 0);
	pb->shadow_dy    = get_int(args, "shadow_dy", 4);
	pb->shadow_blur  = get_int(args, "shadow_blur", 12);
	pb->shadow_color = get_color(args, "shadow_color", argb(120, 0, 0, 0));

	/* Master opacity; a fresh command cancels any running animation. */
	pb->opacity     = get_float(args, "opacity", 1.0f);
	pb->anim.active = 0;

	pb->active = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_update_progress(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	progress_bar_t *pb = progress_find(st, id);
	if (!pb) {
		send_response(st, client_idx,
		              create_response("error", "not found"));
		return -1;
	}

	/* Only the value changes here - opacity and any running animation
	 * are deliberately left untouched, so a bar can fade while it fills. */
	pb->value = get_float(args, "value", 0);

	cJSON *sp = cJSON_GetObjectItem(args, "show_percent");
	if (sp)
		pb->show_percent = cJSON_IsTrue(sp);

	/* Optionally switch in/out of indeterminate mode - e.g. start a load
	 * indeterminate, then flip to a real bar once the size is known. */
	cJSON *ind = cJSON_GetObjectItem(args, "indeterminate");
	if (ind) {
		int was = pb->indeterminate;
		pb->indeterminate = cJSON_IsTrue(ind);
		if (pb->indeterminate && !was)
			pb->indet_start_ms = now_ms();
	}

	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_hide_progress(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	progress_bar_t *pb = progress_find(st, id);
	if (pb) {
		pb->active = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Animation Command
 *
 * Animate an element's opacity over time:
 *   {"cmd":"animate","type":"text","id":1,"property":"opacity",
 *    "from":0,"to":1,"duration":400,"easing":"ease_out",
 *    "remove_on_end":false,"repeat":false}
 *
 * `type` is text | rect | progress | overlay | spinner. `from` defaults
 * to the element's current opacity. With "repeat" the animation
 * ping-pongs forever; with "remove_on_end" the element is deactivated
 * once a (non-repeating) animation finishes.
 * ======================================================================== */

static int cmd_animate(splash_state_t *st, cJSON *args, int client_idx) {
	const char *type = get_string(args, "type", NULL);
	int id = get_int(args, "id", -1);
	if (!type || id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing type or id"));
		return -1;
	}

	const char *prop = get_string(args, "property", "opacity");
	if (strcasecmp(prop, "opacity") != 0) {
		send_response(st, client_idx,
		              create_response("error", "unknown property"));
		return -1;
	}

	/* Resolve the element's opacity and animation slots. */
	float  *opacity = NULL;
	anim_t *anim    = NULL;

	if (strcasecmp(type, "text") == 0) {
		text_element_t *te = text_find(st, id);
		if (te) {
			opacity = &te->opacity;
			anim    = &te->anim;
		}
	} else if (strcasecmp(type, "rect") == 0) {
		rect_element_t *re = rect_find(st, id);
		if (re) {
			opacity = &re->opacity;
			anim    = &re->anim;
		}
	} else if (strcasecmp(type, "progress") == 0) {
		progress_bar_t *pb = progress_find(st, id);
		if (pb) {
			opacity = &pb->opacity;
			anim    = &pb->anim;
		}
	} else if (strcasecmp(type, "overlay") == 0) {
		image_overlay_t *ov = overlay_find(st, id);
		if (ov) {
			opacity = &ov->opacity;
			anim    = &ov->anim;
		}
	} else if (strcasecmp(type, "spinner") == 0) {
		spinner_t *sp = spinner_find(st, id);
		if (sp) {
			opacity = &sp->opacity;
			anim    = &sp->anim;
		}
	} else {
		send_response(st, client_idx,
		              create_response("error", "unknown type"));
		return -1;
	}

	if (!anim) {
		send_response(st, client_idx,
		              create_response("error", "element not found"));
		return -1;
	}

	float from = fclamp(get_float(args, "from", *opacity), 0.0f, 1.0f);
	float to   = fclamp(get_float(args, "to",   1.0f),     0.0f, 1.0f);
	int   dur  = get_int(args, "duration", 300);
	if (dur < 0)
		dur = 0;
	int repeat = cJSON_IsTrue(cJSON_GetObjectItem(args, "repeat"));

	*opacity          = from;
	anim->active      = 1;
	anim->from        = from;
	anim->to          = to;
	anim->duration_ms = (uint32_t)dur;
	anim->start_ms    = now_ms();
	anim->easing      = get_easing(args, "easing", EASE_OUT);
	anim->repeat      = repeat;
	/* A repeating animation ping-pongs forever, so an end action would
	 * never fire - the two options are mutually exclusive. */
	anim->remove_on_end = repeat ? 0
	                    : cJSON_IsTrue(cJSON_GetObjectItem(args, "remove_on_end"));

	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Spinner Command
 *
 * An Apple-style rotating boot indicator:
 *   {"cmd":"spinner","id":0,"x":-1,"y":-1,"radius":40,"color":"#ffffff",
 *    "period":900}
 *   {"cmd":"spinner","id":0,"hidden":true}            configure, stay hidden
 *   {"cmd":"spinner","id":0,"action":"hide"}
 *   {"cmd":"spinner","id":0,"action":"show_animated","duration":300}
 *   {"cmd":"spinner","id":0,"action":"hide_animated","duration":300}
 *
 * A spinner's slot survives "hide": a later show that omits a field keeps
 * the previously configured value, so only what is given changes.
 * "hidden":true configures the spinner without displaying it, so it can
 * be set up once at startup and later revealed with show_animated.
 * ======================================================================== */

static int cmd_spinner(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	const char *action = get_string(args, "action", NULL);

	/* "hide": stop rendering immediately, keep the configuration. */
	if (action && strcasecmp(action, "hide") == 0) {
		spinner_t *sp = spinner_find(st, id);
		if (sp) {
			sp->active      = 0;
			sp->anim.active = 0;
		}
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* "hide_animated": fade opacity to 0, then stop rendering. */
	if (action && strcasecmp(action, "hide_animated") == 0) {
		spinner_t *sp = spinner_find(st, id);
		if (!sp || !sp->active) {
			if (sp)
				sp->active = 0;
			send_response(st, client_idx, create_response("ok", NULL));
			return 0;
		}
		int dur = get_int(args, "duration", 300);
		if (dur < 0)
			dur = 0;
		sp->anim.active        = 1;
		sp->anim.from          = sp->opacity;
		sp->anim.to            = 0.0f;
		sp->anim.duration_ms   = (uint32_t)dur;
		sp->anim.start_ms      = now_ms();
		sp->anim.easing        = get_easing(args, "easing", EASE_IN_OUT);
		sp->anim.repeat        = 0;
		sp->anim.remove_on_end = 1;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* show / show_animated / configure. */
	spinner_t *sp = spinner_find(st, id);
	int fresh = 0;
	if (!sp) {
		sp = spinner_alloc(st);
		fresh = 1;
	}
	if (!sp) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	if (fresh) {
		/* Brand-new spinner: JSON values, or sane defaults. */
		sp->id        = id;
		sp->x         = get_int(args, "x", -1);
		sp->y         = get_int(args, "y", -1);
		sp->radius    = get_int(args, "radius", 36);
		sp->spokes    = get_int(args, "spokes", 12);
		sp->color     = get_color(args, "color", rgb(255, 255, 255));
		sp->period_ms = (uint32_t)get_int(args, "period", 900);
	} else {
		/* Existing spinner: keep every field the caller omits. */
		sp->x         = get_int(args, "x", sp->x);
		sp->y         = get_int(args, "y", sp->y);
		sp->radius    = get_int(args, "radius", sp->radius);
		sp->spokes    = get_int(args, "spokes", sp->spokes);
		sp->color     = get_color(args, "color", sp->color);
		sp->period_ms = (uint32_t)get_int(args, "period",
		                                  (int)sp->period_ms);
	}

	sp->start_ms = now_ms();

	int animated = (action && strcasecmp(action, "show_animated") == 0);
	int hidden   = cJSON_IsTrue(cJSON_GetObjectItem(args, "hidden"));

	if (animated) {
		/* Fade in from nothing. */
		int dur = get_int(args, "duration", 300);
		if (dur < 0)
			dur = 0;
		sp->active             = 1;
		sp->opacity            = 0.0f;
		sp->anim.active        = 1;
		sp->anim.from          = 0.0f;
		sp->anim.to            = 1.0f;
		sp->anim.duration_ms   = (uint32_t)dur;
		sp->anim.start_ms      = now_ms();
		sp->anim.easing        = get_easing(args, "easing", EASE_IN_OUT);
		sp->anim.repeat        = 0;
		sp->anim.remove_on_end = 0;
	} else if (hidden) {
		/* Configured, but not displayed - reveal it later. */
		sp->active      = 0;
		sp->anim.active = 0;
		sp->opacity     = get_float(args, "opacity", 1.0f);
	} else {
		/* Plain show. */
		sp->active      = 1;
		sp->anim.active = 0;
		sp->opacity     = get_float(args, "opacity", 1.0f);
	}

	st->needs_render = 1;
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
	{ "exit",            cmd_exit            },
	{ "suspend",         cmd_suspend         },
	{ "resume",          cmd_resume          },
	{ "status",          cmd_status          },
	{ "ready",           cmd_ready           },
	{ "clear",           cmd_clear           },
	{ "bg_color",        cmd_bg_color        },
	{ "image",           cmd_image           },
	{ "text",            cmd_text            },
	{ "remove_text",     cmd_remove_text     },
	{ "rect",            cmd_rect            },
	{ "remove_rect",     cmd_remove_rect     },
	{ "overlay",         cmd_overlay         },
	{ "remove_overlay",  cmd_remove_overlay  },
	{ "progress",        cmd_progress        },
	{ "update_progress", cmd_update_progress },
	{ "hide_progress",   cmd_hide_progress   },
	{ "animate",         cmd_animate         },
	{ "spinner",         cmd_spinner         },
	{ NULL,              NULL                }
};

/* Dispatch a single command object to its handler. */
static int process_single_cmd(splash_state_t *st, cJSON *cmd_obj,
                               int client_idx) {
	const char *cmd_name = get_string(cmd_obj, "cmd", NULL);
	if (!cmd_name) {
		if (client_idx >= 0)
			send_response(st, client_idx,
			              create_response("error", "missing cmd"));
		return -1;
	}

	for (int i = 0; cmd_table[i].name; i++) {
		if (strcmp(cmd_name, cmd_table[i].name) == 0)
			return cmd_table[i].handler(st, cmd_obj, client_idx);
	}

	if (client_idx >= 0)
		send_response(st, client_idx,
		              create_response("error", "unknown command"));
	else if (st->debug)
		fprintf(stderr, "[debug] Unknown startup command: %s\n", cmd_name);
	return -1;
}

/*
 * Run a command batch (a JSON array) or a single command object. Used by
 * both the socket path and the startup commands.
 */
int process_json_batch(splash_state_t *st, cJSON *root, int client_idx) {
	if (cJSON_IsArray(root)) {
		int count  = cJSON_GetArraySize(root);
		int errors = 0;

		for (int i = 0; i < count; i++) {
			cJSON *cmd = cJSON_GetArrayItem(root, i);
			if (!cJSON_IsObject(cmd))
				continue;
			if (process_single_cmd(st, cmd, client_idx) < 0)
				errors++;
		}

		if (client_idx >= 0) {
			cJSON *resp = create_response(errors > 0 ? "partial" : "ok",
			                              NULL);
			cJSON_AddNumberToObject(resp, "total", count);
			cJSON_AddNumberToObject(resp, "errors", errors);
			send_response(st, client_idx, resp);
		}
		return errors > 0 ? -1 : 0;
	}

	if (cJSON_IsObject(root))
		return process_single_cmd(st, root, client_idx);

	if (client_idx >= 0)
		send_response(st, client_idx,
		              create_response("error", "invalid command format"));
	return -1;
}

/* Entry point from the socket: parse one JSON line and run it. */
int handle_json_command(splash_state_t *st, const char *json_str,
                        int client_idx) {
	/* Any command from any client pets the watchdog. */
	st->last_activity_ms = now_ms();

	cJSON *root = cJSON_Parse(json_str);
	if (!root) {
		send_response(st, client_idx,
		              create_response("error", "invalid json"));
		return -1;
	}

	int ret = process_json_batch(st, root, client_idx);
	cJSON_Delete(root);
	return ret;
}
