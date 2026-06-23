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

/* Safe display-dimension accessors (fall back to 1920×1080 before DRM init). */
static int sw(splash_state_t *st) {
	return st->drm.buf[0].width  > 0 ? (int)st->drm.buf[0].width  : 1920;
}
static int sh(splash_state_t *st) {
	return st->drm.buf[0].height > 0 ? (int)st->drm.buf[0].height : 1080;
}

static uint32_t get_color(cJSON *obj, const char *key, uint32_t default_val) {
	const char *str = get_string(obj, key, NULL);
	return str ? parse_color(str) : default_val;
}

static int get_align(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		return (v >= ALIGN_LEFT && v <= ALIGN_RIGHT) ? v : default_val;
	}
	if (!cJSON_IsString(item))
		return default_val;
	const char *str = item->valuestring;
	if (strcasecmp(str, "center") == 0 || strcasecmp(str, "c") == 0)
		return ALIGN_CENTER;
	if (strcasecmp(str, "right") == 0 || strcasecmp(str, "r") == 0)
		return ALIGN_RIGHT;
	return ALIGN_LEFT;
}

static int get_valign(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		return (v >= VALIGN_TOP && v <= VALIGN_BOTTOM) ? v : default_val;
	}
	if (!cJSON_IsString(item))
		return default_val;
	const char *str = item->valuestring;
	if (strcasecmp(str, "middle") == 0 || strcasecmp(str, "m") == 0)
		return VALIGN_MIDDLE;
	if (strcasecmp(str, "bottom") == 0 || strcasecmp(str, "b") == 0)
		return VALIGN_BOTTOM;
	return VALIGN_TOP;
}

static int get_scale_mode(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		return (v >= SCALE_COVER && v <= SCALE_CUSTOM) ? v : default_val;
	}
	if (!cJSON_IsString(item))
		return default_val;
	const char *str = item->valuestring;
	if (strcasecmp(str, "cover")   == 0) return SCALE_COVER;
	if (strcasecmp(str, "contain") == 0) return SCALE_CONTAIN;
	if (strcasecmp(str, "stretch") == 0) return SCALE_STRETCH;
	if (strcasecmp(str, "none")    == 0) return SCALE_NONE;
	if (strcasecmp(str, "custom")  == 0) return SCALE_CUSTOM;
	return SCALE_CONTAIN;
}

/* Gradient direction: "vertical" | "horizontal" | "diagonal" | "none". */
static int get_gradient(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		return (v >= GRAD_NONE && v <= GRAD_DIAGONAL) ? v : default_val;
	}
	if (!cJSON_IsString(item))
		return default_val;
	const char *str = item->valuestring;
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

/*
 * Boolean field that accepts JSON true/false as well as 1/0 (some elements
 * historically read these with get_int, so a documented {"x":true} was
 * silently dropped). Missing key returns default_val, which lets callers use a
 * sentinel like -1 for "omitted = keep current".
 */
static int get_bool(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsBool(item))
		return cJSON_IsTrue(item) ? 1 : 0;
	if (cJSON_IsNumber(item))
		return item->valueint != 0 ? 1 : 0;
	return default_val;
}

/* Image resample quality: "nearest" | "bilinear" | "bicubic" | "lanczos". */
static int get_filter(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		return (v >= IMG_NEAREST && v <= IMG_LANCZOS) ? v : default_val;
	}
	if (!cJSON_IsString(item))
		return default_val;
	const char *str = item->valuestring;
	if (strcasecmp(str, "nearest")  == 0) return IMG_NEAREST;
	if (strcasecmp(str, "bilinear") == 0) return IMG_BILINEAR;
	if (strcasecmp(str, "bicubic")  == 0) return IMG_BICUBIC;
	if (strcasecmp(str, "lanczos")  == 0) return IMG_LANCZOS;
	return default_val;
}

/* Easing curve: "linear" | "ease_in" | "ease_out" | "ease_in_out". */
static int get_easing(cJSON *obj, const char *key, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsNumber(item)) {
		int v = item->valueint;
		return (v >= EASE_LINEAR && v <= EASE_IN_OUT) ? v : default_val;
	}
	if (!cJSON_IsString(item))
		return default_val;
	const char *str = item->valuestring;
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
	int delay_s = get_int(args, "delay", 0);
	send_response(st, client_idx, create_response("ok", NULL));
	if (delay_s > 0) {
		st->exit_at_ms = now_ms() + (uint64_t)delay_s * 1000u;
		LOGD("exit scheduled in %d s", delay_s);
	} else {
		st->running = 0;
	}
	return 0;
}

static int cmd_suspend(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	st->frozen = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	LOGD("SUSPEND");
	return 0;
}

static int cmd_resume(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	st->frozen = 0;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	LOGD("RESUME");
	return 0;
}

static int cmd_status(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	cJSON *resp = create_response("ok", NULL);
	cJSON_AddStringToObject(resp, "version", SPLASH_VERSION);
	cJSON_AddStringToObject(resp, "state",
	                        st->frozen ? "suspended" : "running");
	cJSON_AddBoolToObject(resp, "ready",  st->ready);
	cJSON_AddBoolToObject(resp, "hidden", st->hidden);
	cJSON_AddNumberToObject(resp, "width",
	                        (double)st->drm.buf[0].width);
	cJSON_AddNumberToObject(resp, "height",
	                        (double)st->drm.buf[0].height);
	send_response(st, client_idx, resp);
	return 0;
}

/*
 * Report the running daemon's version. Cheap but valuable: a stale initramfs
 * can keep an old daemon alive after the on-disk binaries were upgraded, so a
 * command that "should work" silently does nothing. This lets the operator ask
 * the live daemon over the socket (e.g. via SSH, when the screen is blocked)
 * what version is actually running. Added in 4.0.1; older daemons answer this
 * with "unknown command", which is itself the diagnostic.
 */
static int cmd_version(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	cJSON *resp = create_response("ok", NULL);
	cJSON_AddStringToObject(resp, "version", SPLASH_VERSION);
	send_response(st, client_idx, resp);
	return 0;
}

/*
 * Liveness probe. A reply can only come from a live daemon, so this always
 * answers {"status":"ok","running":true}; the "not running" case is observed by
 * the client when the connection fails. splash-ctl uses this for `--running`
 * and answers a bare {"cmd":"running"} with a clean boolean even when the daemon
 * is down, so scripts get JSON instead of a connection error.
 */
static int cmd_running(splash_state_t *st, cJSON *args, int client_idx) {
	(void)args;
	cJSON *resp = create_response("ok", NULL);
	cJSON_AddBoolToObject(resp, "running", 1);
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
		st->bg_anim.target        = NULL;	/* opacity crossfade: float path */
		st->bg_anim.kind          = ANIM_FLOAT;
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
		st->bg_anim.active  = 0;
		st->bg_image        = newimg;
		st->bg_scale_mode   = mode;
		st->bg_custom_scale = cscale;
		st->bg_filter       = filter;
		st->bg_loaded       = 1;
		st->bg_opacity      = 1.0f;
	}

	st->bg_cache_dirty = 1;		/* background changed: rebuild the resample cache */
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
	int fresh = (te == NULL);
	if (fresh) {
		te = text_alloc(st);
		if (!te) {
			send_response(st, client_idx,
			              create_response("error", "no slots"));
			return -1;
		}
	}

	/* remove:true deletes the element in place (same as remove_text). */
	if (get_bool(args, "remove", 0)) {
		te->active  = 0;
		te->text[0] = '\0';
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults before applying the
	 * given fields. Otherwise an existing id MERGES: any field not supplied
	 * keeps its current value (so `{id:0, text:"world"}` redraws "world" with
	 * the previous position, colour and font instead of resetting them). */
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(te, 0, sizeof(*te));
		fresh = 1;
	}

	te->id        = id;
	te->x         = get_coord(args, "x", sw(st), fresh ? -1 : te->x);
	te->y         = get_coord(args, "y", sh(st), fresh ? -1 : te->y);
	te->align     = get_align(args, "align", fresh ? ALIGN_CENTER : te->align);
	te->valign    = get_valign(args, "valign", fresh ? VALIGN_MIDDLE : te->valign);
	te->color     = get_color(args, "color", fresh ? rgb(255, 255, 255) : te->color);
	te->font_slot = get_int(args, "font", fresh ? 0 : te->font_slot);
	te->font_size = get_float(args, "size", fresh ? 0 : te->font_size);

	/* Word wrap (optional; default off = clip at buffer edge). */
	te->wrap       = get_bool(args, "wrap", fresh ? 0 : te->wrap);
	te->wrap_width = get_int(args, "wrap_width", fresh ? 0 : te->wrap_width);

	/* Soft drop shadow. */
	te->shadow       = get_bool(args, "shadow", fresh ? 0 : te->shadow);
	te->shadow_dx    = get_int(args, "shadow_dx", fresh ? 2 : te->shadow_dx);
	te->shadow_dy    = get_int(args, "shadow_dy", fresh ? 2 : te->shadow_dy);
	te->shadow_blur  = get_int(args, "shadow_blur", fresh ? 4 : te->shadow_blur);
	te->shadow_color = get_color(args, "shadow_color",
	                             fresh ? argb(160, 0, 0, 0) : te->shadow_color);

	/* Outline / stroke (optional). */
	te->outline       = get_int(args, "outline", fresh ? 0 : te->outline);
	te->outline_color = get_color(args, "outline_color",
	                              fresh ? argb(255, 0, 0, 0) : te->outline_color);

	/* Master opacity. Only a fresh element or an explicit opacity cancels a
	 * running fade, so a merge update can change the text mid-animation. */
	te->opacity = get_float(args, "opacity", fresh ? 1.0f : te->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		te->anim.active = 0;

	/* '\n' in the text splits it into multiple stacked lines. On a merge
	 * update with no "text" key the current string is kept. */
	const char *text = get_string(args, "text", fresh ? "" : NULL);
	if (text) {
		strncpy(te->text, text, sizeof(te->text) - 1);
		te->text[sizeof(te->text) - 1] = '\0';
	}

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
	int fresh = (re == NULL);
	if (fresh)
		re = rect_alloc(st);
	if (!re) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	/* remove:true deletes the element in place (same as remove_rect). */
	if (get_bool(args, "remove", 0)) {
		re->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults; otherwise an
	 * existing id MERGES, keeping any field the caller omits. */
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(re, 0, sizeof(*re));
		fresh = 1;
	}

	re->id           = id;
	re->x            = get_coord(args, "x", sw(st), fresh ? -1 : re->x);
	re->y            = get_coord(args, "y", sh(st), fresh ? -1 : re->y);
	re->w            = get_coord(args, "w", sw(st), fresh ? 0 : re->w);
	re->h            = get_coord(args, "h", sh(st), fresh ? 0 : re->h);
	re->align        = get_align(args, "align", fresh ? ALIGN_CENTER : re->align);
	re->valign       = get_valign(args, "valign", fresh ? VALIGN_MIDDLE : re->valign);
	re->color        = get_color(args, "color", fresh ? rgb(255, 255, 255) : re->color);
	re->fill         = get_bool(args, "fill", fresh ? 0 : re->fill);
	re->radius       = get_int(args, "radius", fresh ? 0 : re->radius);
	re->border_color = get_color(args, "border_color", fresh ? re->color : re->border_color);
	re->border_width = get_int(args, "border_width", fresh ? 0 : re->border_width);

	/* Gradient fill. Documented key is grad_dir; "gradient" stays a legacy alias. */
	re->grad_dir   = get_gradient(args, "grad_dir",
	                              get_gradient(args, "gradient",
	                                           fresh ? GRAD_NONE : re->grad_dir));
	re->grad_color = get_color(args, "grad_color", fresh ? re->color : re->grad_color);

	/* Soft drop shadow. */
	re->shadow       = get_bool(args, "shadow", fresh ? 0 : re->shadow);
	re->shadow_dx    = get_int(args, "shadow_dx", fresh ? 4 : re->shadow_dx);
	re->shadow_dy    = get_int(args, "shadow_dy", fresh ? 6 : re->shadow_dy);
	re->shadow_blur  = get_int(args, "shadow_blur", fresh ? 10 : re->shadow_blur);
	re->shadow_color = get_color(args, "shadow_color",
	                             fresh ? argb(130, 0, 0, 0) : re->shadow_color);

	/* Master opacity. Only a fresh element or an explicit opacity cancels a
	 * running fade, so a merge update can change the rect mid-animation. */
	re->opacity = get_float(args, "opacity", fresh ? 1.0f : re->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
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
 * Ellipse / Circle Commands
 * ======================================================================== */

static int cmd_ellipse(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	ellipse_t *e = ellipse_find(st, id);
	int fresh = (e == NULL);
	if (fresh) {
		e = ellipse_alloc(st);
		if (!e) {
			send_response(st, client_idx,
			              create_response("error", "no slots"));
			return -1;
		}
	}

	if (get_bool(args, "remove", 0)) {
		e->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(e, 0, sizeof(*e));
		fresh = 1;
	}

	e->id        = id;
	/* x/y are the centre; a negative value (the default) means screen centre. */
	e->cx        = get_coord(args, "x", sw(st), fresh ? -1 : e->cx);
	e->cy        = get_coord(args, "y", sh(st), fresh ? -1 : e->cy);
	/* `radius` is a circle shorthand for rx; ry defaults to 0 = mirror rx. */
	e->rx        = get_int(args, "rx",
	                       get_int(args, "radius", fresh ? 40 : e->rx));
	e->ry        = get_int(args, "ry", fresh ? 0 : e->ry);
	e->thickness = get_int(args, "thickness", fresh ? 0 : e->thickness);
	e->color     = get_color(args, "color", fresh ? rgb(255, 255, 255) : e->color);

	e->opacity = get_float(args, "opacity", fresh ? 1.0f : e->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		e->anim.active = 0;

	e->active = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_ellipse(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	ellipse_t *e = ellipse_find(st, id);
	if (e) {
		e->active = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Line Commands
 * ======================================================================== */

static int cmd_line(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	line_t *l = line_find(st, id);
	int fresh = (l == NULL);
	if (fresh) {
		l = line_alloc(st);
		if (!l) {
			send_response(st, client_idx,
			              create_response("error", "no slots"));
			return -1;
		}
	}

	if (get_bool(args, "remove", 0)) {
		l->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(l, 0, sizeof(*l));
		fresh = 1;
	}

	l->id        = id;
	l->x1        = get_coord(args, "x1", sw(st), fresh ? 0 : l->x1);
	l->y1        = get_coord(args, "y1", sh(st), fresh ? 0 : l->y1);
	l->x2        = get_coord(args, "x2", sw(st), fresh ? 0 : l->x2);
	l->y2        = get_coord(args, "y2", sh(st), fresh ? 0 : l->y2);
	l->thickness = get_int(args, "thickness", fresh ? 2 : l->thickness);
	l->cap       = get_int(args, "cap", fresh ? 0 : l->cap);
	l->color     = get_color(args, "color", fresh ? rgb(255, 255, 255) : l->color);

	l->opacity = get_float(args, "opacity", fresh ? 1.0f : l->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		l->anim.active = 0;

	l->active = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_line(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	line_t *l = line_find(st, id);
	if (l) {
		l->active = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Stepper Commands
 * ======================================================================== */

static int cmd_stepper(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	stepper_t *s = stepper_find(st, id);
	int fresh = (s == NULL);
	if (fresh) {
		s = stepper_alloc(st);
		if (!s) {
			send_response(st, client_idx,
			              create_response("error", "no slots"));
			return -1;
		}
	}

	if (get_bool(args, "remove", 0)) {
		s->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(s, 0, sizeof(*s));
		fresh = 1;
	}

	s->id         = id;
	s->x          = get_coord(args, "x", sw(st), fresh ? -1 : s->x);
	s->y          = get_coord(args, "y", sh(st), fresh ? -1 : s->y);
	s->align      = get_align(args, "align", fresh ? ALIGN_CENTER : s->align);
	s->valign     = get_valign(args, "valign", fresh ? VALIGN_MIDDLE : s->valign);
	s->count      = get_int(args, "count", fresh ? 3 : s->count);
	s->current    = get_int(args, "current", fresh ? 0 : s->current);
	s->style      = get_int(args, "style", fresh ? 0 : s->style);
	s->size       = get_int(args, "size", fresh ? 12 : s->size);
	s->length     = get_int(args, "length", fresh ? 0 : s->length);
	s->gap        = get_int(args, "gap", fresh ? 0 : s->gap);
	s->thickness  = get_int(args, "thickness", fresh ? 0 : s->thickness);
	s->color_done = get_color(args, "color_done",
	                          fresh ? rgb(255, 255, 255) : s->color_done);
	s->color_todo = get_color(args, "color_todo",
	                          fresh ? rgb(80, 80, 80) : s->color_todo);

	if (s->count < 0)   s->count = 0;
	if (s->current < 0) s->current = 0;
	if (s->current > s->count) s->current = s->count;

	s->opacity = get_float(args, "opacity", fresh ? 1.0f : s->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		s->anim.active = 0;

	s->active = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_stepper(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	stepper_t *s = stepper_find(st, id);
	if (s) {
		s->active = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Marquee Commands
 * ======================================================================== */

static int cmd_marquee(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	marquee_t *m = marquee_find(st, id);
	int fresh = (m == NULL);
	if (fresh) {
		m = marquee_alloc(st);
		if (!m) {
			send_response(st, client_idx,
			              create_response("error", "no slots"));
			return -1;
		}
	}

	if (get_bool(args, "remove", 0)) {
		marquee_free_cache(m);
		m->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}
	if (!fresh && get_bool(args, "replace", 0)) {
		marquee_free_cache(m);		/* free before memset drops the ptr */
		memset(m, 0, sizeof(*m));
		fresh = 1;
	}

	m->id        = id;
	m->x         = get_coord(args, "x", sw(st), fresh ? -1 : m->x);
	m->y         = get_coord(args, "y", sh(st), fresh ? -1 : m->y);
	m->w         = get_coord(args, "w", sw(st), fresh ? 400 : m->w);
	m->h         = get_coord(args, "h", sh(st), fresh ? 40 : m->h);
	m->font_slot = get_int(args, "font", fresh ? 0 : m->font_slot);
	m->font_size = get_float(args, "size", fresh ? 0 : m->font_size);
	m->color     = get_color(args, "color", fresh ? rgb(255, 255, 255) : m->color);
	m->speed     = get_int(args, "speed", fresh ? 60 : m->speed);
	m->gap       = get_int(args, "gap", fresh ? 60 : m->gap);

	const char *text = get_string(args, "text", fresh ? "" : NULL);
	if (text) {
		strncpy(m->text, text, sizeof(m->text) - 1);
		m->text[sizeof(m->text) - 1] = '\0';
	}

	m->opacity = get_float(args, "opacity", fresh ? 1.0f : m->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		m->anim.active = 0;

	/* Invalidate the coverage cache only when a glyph-determining field
	 * changed; the per-frame scroll (anim_tick) never comes through here. */
	if (cJSON_GetObjectItem(args, "text") ||
	    cJSON_GetObjectItem(args, "font") ||
	    cJSON_GetObjectItem(args, "size"))
		m->cov_dirty = 1;

	m->active = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_marquee(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	marquee_t *m = marquee_find(st, id);
	if (m) {
		m->active = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Sprite (frame animation) Commands
 * ======================================================================== */

static int cmd_sprite(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	sprite_t *sp = sprite_find(st, id);
	int fresh = (sp == NULL);
	if (fresh) {
		sp = sprite_alloc(st);
		if (!sp) {
			send_response(st, client_idx,
			              create_response("error", "no slots"));
			return -1;
		}
	}

	if (get_bool(args, "remove", 0)) {
		sprite_clear_frames(sp);
		sp->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}
	if (!fresh && get_bool(args, "replace", 0)) {
		sprite_clear_frames(sp);		/* free before the memset loses ptrs */
		memset(sp, 0, sizeof(*sp));
		fresh = 1;
	}

	sp->id     = id;
	sp->x      = get_coord(args, "x", sw(st), fresh ? -1 : sp->x);
	sp->y      = get_coord(args, "y", sh(st), fresh ? -1 : sp->y);
	sp->w      = get_coord(args, "w", sw(st), fresh ? 0 : sp->w);
	sp->h      = get_coord(args, "h", sh(st), fresh ? 0 : sp->h);
	sp->align  = get_align(args, "align", fresh ? ALIGN_CENTER : sp->align);
	sp->valign = get_valign(args, "valign", fresh ? VALIGN_MIDDLE : sp->valign);
	sp->filter = get_filter(args, "filter", fresh ? IMG_LANCZOS : sp->filter);
	sp->fps    = get_int(args, "fps", fresh ? 12 : sp->fps);
	sp->loop   = get_bool(args, "loop", fresh ? 1 : sp->loop);

	/* Frame list: (re)load only when "frames" is supplied, so a merge update
	 * without it keeps the current frames running. */
	cJSON *frames = cJSON_GetObjectItem(args, "frames");
	if (frames && cJSON_IsArray(frames)) {
		sprite_clear_frames(sp);
		int n = cJSON_GetArraySize(frames);
		if (n > MAX_SPRITE_FRAMES)
			n = MAX_SPRITE_FRAMES;
		for (int i = 0; i < n; i++) {
			cJSON *p = cJSON_GetArrayItem(frames, i);
			const char *path = cJSON_IsString(p) ? p->valuestring : NULL;
			if (path && load_image(path, &sp->frames[sp->frame_count]) == 0)
				sp->frame_count++;
			else if (path)
				LOGW("sprite %d: could not load frame %s", id, path);
		}
		sp->current = 0;
		sp->last_ms = 0;
	}

	sp->opacity = get_float(args, "opacity", fresh ? 1.0f : sp->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		sp->anim.active = 0;

	sp->active = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_sprite(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	sprite_t *sp = sprite_find(st, id);
	if (sp) {
		sprite_clear_frames(sp);
		sp->active = 0;
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
	int fresh = (ov == NULL);
	if (!ov)
		ov = overlay_alloc(st);
	if (!ov) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	/* remove:true deletes the element in place (same as remove_overlay). */
	if (get_bool(args, "remove", 0)) {
		free_image(&ov->img);
		ov->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults; otherwise an
	 * existing id MERGES, keeping any field the caller omits. */
	if (!fresh && get_bool(args, "replace", 0)) {
		free_image(&ov->img);
		memset(ov, 0, sizeof(*ov));
		fresh = 1;
	}

	ov->id     = id;
	ov->x      = get_coord(args, "x", sw(st), fresh ? -1 : ov->x);
	ov->y      = get_coord(args, "y", sh(st), fresh ? -1 : ov->y);
	ov->w      = get_coord(args, "w", sw(st), fresh ? 0 : ov->w);
	ov->h      = get_coord(args, "h", sh(st), fresh ? 0 : ov->h);
	ov->align  = get_align(args, "align", fresh ? ALIGN_CENTER : ov->align);
	ov->valign = get_valign(args, "valign", fresh ? VALIGN_MIDDLE : ov->valign);
	ov->filter = get_filter(args, "filter", fresh ? IMG_LANCZOS : ov->filter);
	ov->radius = get_int(args, "radius", fresh ? 0 : ov->radius);
	ov->angle  = get_float(args, "angle", fresh ? 0.0f : ov->angle);
	ov->tint   = get_color(args, "tint", fresh ? 0 : ov->tint);

	/* Master opacity; only a fresh element or an explicit opacity cancels a
	 * running fade. */
	ov->opacity = get_float(args, "opacity", fresh ? 1.0f : ov->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		ov->anim.active = 0;

	/* The image (re)loads only when a path is supplied; a merge update without
	 * one keeps the current image, so e.g. just the angle can be changed. */
	const char *path = get_string(args, "path", NULL);
	if (path) {
		free_image(&ov->img);
		if (load_image(path, &ov->img) < 0) {
			if (fresh) ov->active = 0;	/* release the slot we just claimed */
			send_response(st, client_idx,
			              create_response("error", "failed to load image"));
			return -1;
		}
	} else if (fresh || !ov->img.rgba) {
		if (fresh) ov->active = 0;
		send_response(st, client_idx,
		              create_response("error", "missing path"));
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

static int cmd_progress(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	progress_bar_t *pb = progress_find(st, id);
	int fresh = (pb == NULL);
	if (fresh)
		pb = progress_alloc(st);
	if (!pb) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	/* remove:true deletes the element in place (same as remove_progress). */
	if (get_bool(args, "remove", 0)) {
		memset(pb, 0, sizeof(*pb));
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults; otherwise an
	 * existing id MERGES, keeping any field the caller omits. */
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(pb, 0, sizeof(*pb));
		fresh = 1;
	}

	pb->id           = id;
	pb->x            = get_coord(args, "x", sw(st), fresh ? -1 : pb->x);
	pb->y            = get_coord(args, "y", sh(st), fresh ? -1 : pb->y);
	pb->w            = get_coord(args, "w", sw(st), fresh ? 100 : pb->w);
	pb->h            = get_coord(args, "h", sh(st), fresh ? 20 : pb->h);
	pb->align        = get_align(args, "align", fresh ? ALIGN_CENTER : pb->align);
	pb->valign       = get_valign(args, "valign", fresh ? VALIGN_MIDDLE : pb->valign);
	pb->style        = get_int(args, "style", fresh ? 0 : pb->style);
	pb->value        = fclamp(get_float(args, "value", fresh ? 0 : pb->value), 0.0f, 1.0f);
	/* `border` is an undocumented CSS-like shorthand for border_width: a
	 * number, or false (0) / true (1px). The explicit border_width wins when
	 * both are present. */
	int border_def = fresh ? 2 : pb->border_width;
	cJSON *border_j = cJSON_GetObjectItem(args, "border");
	if (border_j) {
		if (cJSON_IsBool(border_j))
			border_def = cJSON_IsTrue(border_j) ? 1 : 0;
		else if (cJSON_IsNumber(border_j))
			border_def = border_j->valueint;
	}
	pb->border_width = get_int(args, "border_width", border_def);
	pb->radius       = get_int(args, "radius", fresh ? 0 : pb->radius);
	/* Documented keys are font_slot/font_size; font/size remain legacy aliases. */
	pb->font_slot    = get_int(args, "font_slot",
	                           get_int(args, "font", fresh ? 0 : pb->font_slot));
	pb->font_size    = get_float(args, "font_size",
	                             get_float(args, "size", fresh ? 0 : pb->font_size));
	pb->show_percent = get_bool(args, "show_percent", fresh ? 0 : pb->show_percent);

	/* Any explicit colour switches the bar to a custom style (-1). */
	int has_custom_color = (
		cJSON_GetObjectItem(args, "bg_color")     ||
		cJSON_GetObjectItem(args, "bar_color")    ||
		cJSON_GetObjectItem(args, "border_color") ||
		cJSON_GetObjectItem(args, "text_color")
	);
	if (has_custom_color)
		pb->style = -1;

	if (fresh || cJSON_GetObjectItem(args, "style") || has_custom_color)
		set_default_progress_colors(pb, pb->style);

	/* Use get_color() so non-string JSON values fall back to the current
	 * colour rather than silently producing white via parse_color(NULL). */
	pb->bg_color     = get_color(args, "bg_color",     pb->bg_color);
	pb->bar_color    = get_color(args, "bar_color",    pb->bar_color);
	pb->border_color = get_color(args, "border_color", pb->border_color);
	pb->text_color   = get_color(args, "text_color",   pb->text_color);

	/* Gradient for the fill. Documented key is bar_gradient (matching arc);
	 * "gradient" stays a legacy alias. */
	pb->bar_gradient = get_gradient(args, "bar_gradient",
	                                get_gradient(args, "gradient",
	                                             fresh ? GRAD_NONE : pb->bar_gradient));
	pb->bar_color2   = get_color(args, "bar_color2", fresh ? pb->bar_color : pb->bar_color2);

	/* Indeterminate mode: a sweeping highlight instead of a fixed fill,
	 * for when the task has no measurable progress. */
	pb->indeterminate   = get_bool(args, "indeterminate", fresh ? 0 : pb->indeterminate);
	pb->indet_period_ms = (uint32_t)get_int(args, "indet_period_ms",
	                                        fresh ? 1100 : (int)pb->indet_period_ms);
	pb->indet_start_ms  = now_ms();

	/* Soft drop shadow of the whole bar. */
	pb->shadow       = get_bool(args, "shadow", fresh ? 0 : pb->shadow);
	pb->shadow_dx    = get_int(args, "shadow_dx", fresh ? 0 : pb->shadow_dx);
	pb->shadow_dy    = get_int(args, "shadow_dy", fresh ? 4 : pb->shadow_dy);
	pb->shadow_blur  = get_int(args, "shadow_blur", fresh ? 12 : pb->shadow_blur);
	pb->shadow_color = get_color(args, "shadow_color",
	                             fresh ? argb(120, 0, 0, 0) : pb->shadow_color);

	/* Master opacity. Only a fresh element or an explicit opacity cancels a
	 * running fade, so a merge update can change the bar mid-animation. */
	pb->opacity = get_float(args, "opacity", fresh ? 1.0f : pb->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
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
	pb->value = fclamp(get_float(args, "value", 0), 0.0f, 1.0f);

	pb->show_percent = get_bool(args, "show_percent", pb->show_percent);

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

static int cmd_remove_progress(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	progress_bar_t *pb = progress_find(st, id);
	if (pb) {
		memset(pb, 0, sizeof(*pb));
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

/*
 * For a non-opacity animate property, point *ip (an int field: x/y/w/h) or *cp
 * (a colour field) at the element's field, leaving both NULL if the property is
 * not animatable for that element type.
 */
static void resolve_anim_field(splash_state_t *st, const char *type, int id,
                               const char *prop, int **ip, uint32_t **cp) {
	*ip = NULL;
	*cp = NULL;
	int x = !strcasecmp(prop, "x"), y = !strcasecmp(prop, "y");
	int w = !strcasecmp(prop, "w"), h = !strcasecmp(prop, "h");
	int c = !strcasecmp(prop, "color");

	if (!strcasecmp(type, "text")) {
		text_element_t *e = text_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (c) *cp=&e->color;
	} else if (!strcasecmp(type, "rect")) {
		rect_element_t *e = rect_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (w) *ip=&e->w;
		else if (h) *ip=&e->h; else if (c) *cp=&e->color;
	} else if (!strcasecmp(type, "ellipse") || !strcasecmp(type, "circle")) {
		ellipse_t *e = ellipse_find(st, id); if (!e) return;
		if (x) *ip=&e->cx; else if (y) *ip=&e->cy; else if (w) *ip=&e->rx;
		else if (h) *ip=&e->ry; else if (c) *cp=&e->color;
	} else if (!strcasecmp(type, "line")) {
		line_t *e = line_find(st, id); if (!e) return;
		if (c) *cp=&e->color;
	} else if (!strcasecmp(type, "stepper")) {
		stepper_t *e = stepper_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y;
	} else if (!strcasecmp(type, "marquee")) {
		marquee_t *e = marquee_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (w) *ip=&e->w;
		else if (h) *ip=&e->h; else if (c) *cp=&e->color;
	} else if (!strcasecmp(type, "sprite")) {
		sprite_t *e = sprite_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (w) *ip=&e->w;
		else if (h) *ip=&e->h;
	} else if (!strcasecmp(type, "progress")) {
		progress_bar_t *e = progress_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (w) *ip=&e->w;
		else if (h) *ip=&e->h;
	} else if (!strcasecmp(type, "overlay")) {
		image_overlay_t *e = overlay_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (w) *ip=&e->w;
		else if (h) *ip=&e->h;
	} else if (!strcasecmp(type, "spinner")) {
		spinner_t *e = spinner_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (c) *cp=&e->color;
	} else if (!strcasecmp(type, "arc")) {
		arc_bar_t *e = arc_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y;
	} else if (!strcasecmp(type, "console")) {
		console_t *e = console_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (w) *ip=&e->w;
		else if (h) *ip=&e->h; else if (c) *cp=&e->color;
	} else if (!strcasecmp(type, "qr")) {
		qr_element_t *e = qr_find(st, id); if (!e) return;
		if (x) *ip=&e->x; else if (y) *ip=&e->y; else if (c) *cp=&e->color;
	}
}

static int cmd_animate(splash_state_t *st, cJSON *args, int client_idx) {
	const char *type = get_string(args, "type", NULL);
	int id = get_int(args, "id", -1);
	if (!type || id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing type or id"));
		return -1;
	}

	const char *prop = get_string(args, "property", "opacity");

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
	} else if (strcasecmp(type, "ellipse") == 0 ||
	           strcasecmp(type, "circle") == 0) {
		ellipse_t *e = ellipse_find(st, id);
		if (e) {
			opacity = &e->opacity;
			anim    = &e->anim;
		}
	} else if (strcasecmp(type, "line") == 0) {
		line_t *l = line_find(st, id);
		if (l) {
			opacity = &l->opacity;
			anim    = &l->anim;
		}
	} else if (strcasecmp(type, "stepper") == 0) {
		stepper_t *s = stepper_find(st, id);
		if (s) {
			opacity = &s->opacity;
			anim    = &s->anim;
		}
	} else if (strcasecmp(type, "marquee") == 0) {
		marquee_t *m = marquee_find(st, id);
		if (m) {
			opacity = &m->opacity;
			anim    = &m->anim;
		}
	} else if (strcasecmp(type, "sprite") == 0) {
		sprite_t *sp = sprite_find(st, id);
		if (sp) {
			opacity = &sp->opacity;
			anim    = &sp->anim;
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
	} else if (strcasecmp(type, "arc") == 0) {
		arc_bar_t *ab = arc_find(st, id);
		if (ab) {
			opacity = &ab->opacity;
			anim    = &ab->anim;
		}
	} else if (strcasecmp(type, "console") == 0) {
		console_t *con = console_find(st, id);
		if (con) {
			opacity = &con->opacity;
			anim    = &con->anim;
		}
	} else if (strcasecmp(type, "qr") == 0) {
		qr_element_t *qr = qr_find(st, id);
		if (qr) {
			opacity = &qr->opacity;
			anim    = &qr->anim;
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

	/* Resolve which field the property drives. "opacity" stays the legacy
	 * float path (target NULL -> anim_tick writes the element's opacity); x/y/
	 * w/h are ints; "color" lerps per channel. */
	void    *target = NULL;
	int      kind   = ANIM_FLOAT;
	float    from_f = 0.0f, to_f = 0.0f;
	uint32_t from_c = 0,    to_c = 0;

	if (strcasecmp(prop, "opacity") == 0) {
		from_f   = fclamp(get_float(args, "from", *opacity), 0.0f, 1.0f);
		to_f     = fclamp(get_float(args, "to",   1.0f),     0.0f, 1.0f);
		*opacity = from_f;
	} else {
		int      *ip = NULL;
		uint32_t *cp = NULL;
		resolve_anim_field(st, type, id, prop, &ip, &cp);
		if (ip) {
			kind   = ANIM_INT; target = ip;
			from_f = (float)get_int(args, "from", *ip);
			to_f   = (float)get_int(args, "to",   *ip);
			*ip    = (int)from_f;
		} else if (cp) {
			kind   = ANIM_COLOR; target = cp;
			from_c = get_color(args, "from", *cp);
			to_c   = get_color(args, "to",   *cp);
			*cp    = from_c;
		} else {
			send_response(st, client_idx,
			              create_response("error",
			                              "unsupported property for type"));
			return -1;
		}
	}

	int dur = get_int(args, "duration", 300);
	if (dur < 0)
		dur = 0;
	int repeat = cJSON_IsTrue(cJSON_GetObjectItem(args, "repeat"));

	anim->active        = 1;
	anim->kind          = kind;
	anim->target        = target;
	anim->from          = from_f;
	anim->to            = to_f;
	anim->from_color    = from_c;
	anim->to_color      = to_c;
	anim->duration_ms   = (uint32_t)dur;
	anim->start_ms      = now_ms();
	anim->easing        = get_easing(args, "easing", EASE_OUT);
	anim->repeat        = repeat;
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
		sp->anim.target        = NULL;	/* opacity fade: legacy float path */
		sp->anim.kind          = ANIM_FLOAT;
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

	/* remove:true deletes the element in place (same as remove_spinner). */
	if (get_bool(args, "remove", 0)) {
		sp->active      = 0;
		sp->anim.active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults; otherwise an
	 * existing id MERGES, keeping any field the caller omits. The spinner
	 * keeps its slot via `used`, which the memset clears, so restore it. */
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(sp, 0, sizeof(*sp));
		sp->used = 1;
		fresh = 1;
	}

	if (fresh) {
		/* Brand-new spinner: JSON values, or sane defaults. */
		sp->id        = id;
		sp->x         = get_coord(args, "x", sw(st), -1);
		sp->y         = get_coord(args, "y", sh(st), -1);
		sp->radius    = get_int(args, "radius", 36);
		sp->spokes    = get_int(args, "spokes", 12);
		sp->color     = get_color(args, "color", rgb(255, 255, 255));
		sp->period_ms = (uint32_t)get_int(args, "period", 900);
	} else {
		/* Existing spinner: keep every field the caller omits. */
		sp->x         = get_coord(args, "x", sw(st), sp->x);
		sp->y         = get_coord(args, "y", sh(st), sp->y);
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
		sp->anim.target        = NULL;	/* opacity fade: legacy float path */
		sp->anim.kind          = ANIM_FLOAT;
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

static int cmd_remove_spinner(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	spinner_t *sp = spinner_find(st, id);
	if (sp) {
		sp->active      = 0;
		sp->anim.active = 0;
		st->needs_render = 1;
	}
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

/* ========================================================================
 * Console Commands
 * ======================================================================== */

static int cmd_console(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	console_t *con   = console_find(st, id);
	int        fresh = (con == NULL);
	if (!con)
		con = console_alloc(st);
	if (!con) {
		send_response(st, client_idx,
		              create_response("error", "no slots"));
		return -1;
	}

	/* remove:true deletes the element in place (same as remove_console). */
	if (get_bool(args, "remove", 0)) {
		con->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults; otherwise an
	 * existing id MERGES, keeping any field the caller omits. */
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(con, 0, sizeof(*con));
		fresh = 1;
	}

	con->id        = id;
	con->x         = get_coord(args, "x", sw(st), fresh ? 0 : con->x);
	con->y         = get_coord(args, "y", sh(st), fresh ? 0 : con->y);
	con->w         = get_coord(args, "w", sw(st), fresh ? 400 : con->w);
	con->h         = get_coord(args, "h", sh(st), fresh ? 200 : con->h);
	con->font_slot = get_int(args, "font", fresh ? 0 : con->font_slot);
	con->font_size = get_float(args, "size", fresh ? 0 : con->font_size);
	con->color     = get_color(args, "color", fresh ? rgb(255, 255, 255) : con->color);
	con->bg_color  = get_color(args, "bg_color", fresh ? 0 : con->bg_color);
	con->padding   = get_int(args, "padding", fresh ? 4 : con->padding);
	con->autofit   = get_bool(args, "autofit", fresh ? 0 : con->autofit);
	con->opacity   = get_float(args, "opacity", fresh ? 1.0f : con->opacity);
	con->active    = 1;

	/* max_lines: settable on creation; changing it resets the buffer. The
	 * lines[] ring is always CONSOLE_MAX_LINES wide, so defaulting to the full
	 * capacity costs nothing and lets a tall console fill completely instead of
	 * capping the visible history below what the box can show. */
	int new_max = get_int(args, "max_lines", fresh ? CONSOLE_MAX_LINES : con->max_lines);
	new_max = clamp(new_max, 1, CONSOLE_MAX_LINES);
	if (fresh || new_max != con->max_lines) {
		con->max_lines  = new_max;
		con->line_count = 0;
		con->head       = 0;
		memset(con->lines, 0, sizeof(con->lines));
	}

	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_console_write(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	console_t *con = console_find(st, id);
	if (!con) {
		send_response(st, client_idx,
		              create_response("error", "console not found"));
		return -1;
	}

	const char *text = get_string(args, "text", NULL);
	if (!text) {
		send_response(st, client_idx,
		              create_response("error", "missing text"));
		return -1;
	}

	/* Optional per-line colour (absent = use the console's default colour). */
	uint32_t line_col = get_color(args, "color", 0);

	/* Split on '\n' and push each segment as a separate line. */
	const char *p = text;
	while (*p) {
		const char *end = p;
		while (*end && *end != '\n')
			end++;

		int len = (int)(end - p);
		if (len >= CONSOLE_LINE_LEN)
			len = CONSOLE_LINE_LEN - 1;
		memcpy(con->lines[con->head], p, (size_t)len);
		con->lines[con->head][len] = '\0';
		con->line_color[con->head] = line_col;

		con->head = (con->head + 1) % con->max_lines;
		if (con->line_count < con->max_lines)
			con->line_count++;

		p = (*end == '\n') ? end + 1 : end;
	}

	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_console(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	console_t *con = console_find(st, id);
	if (con) {
		con->active      = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * Query – inspect current element state
 * ======================================================================== */

static int cmd_query(splash_state_t *st, cJSON *args, int client_idx) {
	const char *type = get_string(args, "type", NULL);
	int id = get_int(args, "id", -1);
	if (!type || id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing type or id"));
		return -1;
	}

	cJSON *resp = create_response("ok", NULL);

	if (strcasecmp(type, "text") == 0) {
		text_element_t *e = text_find(st, id);
		if (!e) goto not_found;
		cJSON_AddStringToObject(resp, "text", e->text);
		cJSON_AddNumberToObject(resp, "x",    e->x);
		cJSON_AddNumberToObject(resp, "y",    e->y);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "rect") == 0) {
		rect_element_t *e = rect_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "x", e->x);
		cJSON_AddNumberToObject(resp, "y", e->y);
		cJSON_AddNumberToObject(resp, "w", e->w);
		cJSON_AddNumberToObject(resp, "h", e->h);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "ellipse") == 0 ||
	           strcasecmp(type, "circle") == 0) {
		ellipse_t *e = ellipse_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "x", e->cx);
		cJSON_AddNumberToObject(resp, "y", e->cy);
		cJSON_AddNumberToObject(resp, "rx", e->rx);
		cJSON_AddNumberToObject(resp, "ry", e->ry);
		cJSON_AddNumberToObject(resp, "thickness", e->thickness);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "line") == 0) {
		line_t *e = line_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "x1", e->x1);
		cJSON_AddNumberToObject(resp, "y1", e->y1);
		cJSON_AddNumberToObject(resp, "x2", e->x2);
		cJSON_AddNumberToObject(resp, "y2", e->y2);
		cJSON_AddNumberToObject(resp, "thickness", e->thickness);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "stepper") == 0) {
		stepper_t *e = stepper_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "x", e->x);
		cJSON_AddNumberToObject(resp, "y", e->y);
		cJSON_AddNumberToObject(resp, "count", e->count);
		cJSON_AddNumberToObject(resp, "current", e->current);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "marquee") == 0) {
		marquee_t *e = marquee_find(st, id);
		if (!e) goto not_found;
		cJSON_AddStringToObject(resp, "text", e->text);
		cJSON_AddNumberToObject(resp, "x", e->x);
		cJSON_AddNumberToObject(resp, "y", e->y);
		cJSON_AddNumberToObject(resp, "w", e->w);
		cJSON_AddNumberToObject(resp, "h", e->h);
		cJSON_AddNumberToObject(resp, "speed", e->speed);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "sprite") == 0) {
		sprite_t *e = sprite_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "x", e->x);
		cJSON_AddNumberToObject(resp, "y", e->y);
		cJSON_AddNumberToObject(resp, "frames", e->frame_count);
		cJSON_AddNumberToObject(resp, "current", e->current);
		cJSON_AddNumberToObject(resp, "fps", e->fps);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "overlay") == 0) {
		image_overlay_t *e = overlay_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "x", e->x);
		cJSON_AddNumberToObject(resp, "y", e->y);
		cJSON_AddNumberToObject(resp, "w", e->w);
		cJSON_AddNumberToObject(resp, "h", e->h);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "progress") == 0) {
		progress_bar_t *e = progress_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "value",   (double)e->value);
		cJSON_AddNumberToObject(resp, "x",       e->x);
		cJSON_AddNumberToObject(resp, "y",       e->y);
		cJSON_AddNumberToObject(resp, "w",       e->w);
		cJSON_AddNumberToObject(resp, "h",       e->h);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "arc") == 0) {
		arc_bar_t *e = arc_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "value",   (double)e->value);
		cJSON_AddNumberToObject(resp, "x",       e->x);
		cJSON_AddNumberToObject(resp, "y",       e->y);
		cJSON_AddNumberToObject(resp, "radius",  e->radius);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "spinner") == 0) {
		spinner_t *e = spinner_find(st, id);
		if (!e) goto not_found;
		cJSON_AddBoolToObject  (resp, "active",  e->active);
		cJSON_AddNumberToObject(resp, "x",       e->x);
		cJSON_AddNumberToObject(resp, "y",       e->y);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else if (strcasecmp(type, "console") == 0) {
		console_t *e = console_find(st, id);
		if (!e) goto not_found;
		cJSON_AddNumberToObject(resp, "x",          e->x);
		cJSON_AddNumberToObject(resp, "y",          e->y);
		cJSON_AddNumberToObject(resp, "w",          e->w);
		cJSON_AddNumberToObject(resp, "h",          e->h);
		cJSON_AddNumberToObject(resp, "line_count", e->line_count);
		cJSON_AddNumberToObject(resp, "opacity",    (double)e->opacity);

	} else if (strcasecmp(type, "qr") == 0) {
		qr_element_t *e = qr_find(st, id);
		if (!e) goto not_found;
		cJSON_AddStringToObject(resp, "text", e->text);
		cJSON_AddNumberToObject(resp, "x",    e->x);
		cJSON_AddNumberToObject(resp, "y",    e->y);
		cJSON_AddNumberToObject(resp, "opacity", (double)e->opacity);

	} else {
		cJSON_Delete(resp);
		send_response(st, client_idx,
		              create_response("error", "unknown type"));
		return -1;
	}

	send_response(st, client_idx, resp);
	return 0;

not_found:
	cJSON_Delete(resp);
	send_response(st, client_idx, create_response("error", "not found"));
	return -1;
}

/* ========================================================================
 * Arc Progress Bar Commands
 * ======================================================================== */

static int cmd_arc(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	arc_bar_t *ab    = arc_find(st, id);
	int        fresh = (ab == NULL);
	if (!ab)
		ab = arc_alloc(st);
	if (!ab) {
		send_response(st, client_idx,
		              create_response("error", "no free arc slots"));
		return -1;
	}

	/* remove:true deletes the element in place (same as remove_arc). */
	if (get_bool(args, "remove", 0)) {
		memset(ab, 0, sizeof(*ab));
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults; otherwise an
	 * existing id MERGES, keeping any field the caller omits. */
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(ab, 0, sizeof(*ab));
		fresh = 1;
	}

	ab->id          = id;
	ab->x           = get_coord(args, "x", sw(st), ab->x);
	ab->y           = get_coord(args, "y", sh(st), ab->y);
	ab->radius      = get_int(args, "radius",    fresh ? 80  : ab->radius);
	ab->thickness   = get_int(args, "thickness", fresh ? 0   : ab->thickness);
	ab->start_angle = get_float(args, "start_angle", fresh ? -90.0f : ab->start_angle);
	ab->sweep       = get_float(args, "sweep",        fresh ? 360.0f : ab->sweep);
	ab->value       = fclamp(get_float(args, "value", ab->value), 0.0f, 1.0f);
	ab->cap         = get_int(args, "cap",         ab->cap);
	ab->bar_gradient = get_gradient(args, "bar_gradient", ab->bar_gradient);
	ab->font_slot   = get_int(args, "font_slot",   fresh ? -1   : ab->font_slot);
	ab->font_size   = get_float(args, "font_size", fresh ? 0.0f : ab->font_size);
	ab->show_percent = get_bool(args, "show_percent", ab->show_percent);
	ab->opacity     = get_float(args, "opacity", fresh ? 1.0f : ab->opacity);

	const char *bg  = get_string(args, "bg_color",    NULL);
	const char *bar = get_string(args, "bar_color",   NULL);
	const char *bar2 = get_string(args, "bar_color2", NULL);
	const char *tc  = get_string(args, "text_color",  NULL);
	if (bg)   ab->bg_color   = parse_color(bg);
	else if (fresh) ab->bg_color = 0x80808080u;
	if (bar)  ab->bar_color  = parse_color(bar);
	else if (fresh) ab->bar_color = 0xFFFFFFFFu;
	if (bar2) ab->bar_color2 = parse_color(bar2);
	if (tc)   ab->text_color = parse_color(tc);
	else if (fresh) ab->text_color = 0xFFFFFFFFu;

	int indet = get_bool(args, "indeterminate", -1);
	if (indet >= 0) {
		if (indet && !ab->indeterminate)
			ab->indet_start_ms = now_ms();
		ab->indeterminate = indet;
	} else if (fresh) {
		ab->indeterminate = 0;
	}
	ab->indet_period_ms = (uint32_t)get_int(args, "indet_period_ms",
	                                        fresh ? 1200 : (int)ab->indet_period_ms);

	ab->active       = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_update_arc(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	arc_bar_t *ab = arc_find(st, id);
	if (!ab) {
		send_response(st, client_idx,
		              create_response("error", "arc not found"));
		return -1;
	}

	ab->value = fclamp(get_float(args, "value", ab->value), 0.0f, 1.0f);

	const char *bar = get_string(args, "bar_color", NULL);
	if (bar) ab->bar_color = parse_color(bar);

	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_hide_arc(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	arc_bar_t *ab = arc_find(st, id);
	if (ab) {
		ab->active       = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_arc(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	arc_bar_t *ab = arc_find(st, id);
	if (ab) {
		memset(ab, 0, sizeof(*ab));
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

/* ========================================================================
 * QR Code Commands
 * ======================================================================== */

static int cmd_qr(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	qr_element_t *qr = qr_find(st, id);
	int fresh = (qr == NULL);
	if (fresh)
		qr = qr_alloc(st);
	if (!qr) {
		send_response(st, client_idx,
		              create_response("error", "no free QR slots"));
		return -1;
	}

	/* remove:true deletes the element in place (same as remove_qr). */
	if (get_bool(args, "remove", 0)) {
		qr->active = 0;
		st->needs_render = 1;
		send_response(st, client_idx, create_response("ok", NULL));
		return 0;
	}

	/* replace:true resets an existing element to defaults; otherwise an
	 * existing id MERGES, keeping any field the caller omits. */
	if (!fresh && get_bool(args, "replace", 0)) {
		memset(qr, 0, sizeof(*qr));
		fresh = 1;
	}

	/* A fresh QR needs text; a merge update may keep its existing string. */
	const char *text = get_string(args, "text", NULL);
	if (fresh && (!text || !text[0])) {
		qr->active = 0;		/* release the slot we just claimed */
		send_response(st, client_idx,
		              create_response("error", "missing text"));
		return -1;
	}

	qr->id        = id;
	qr->x         = get_coord(args, "x", sw(st), fresh ? 0 : qr->x);
	qr->y         = get_coord(args, "y", sh(st), fresh ? 0 : qr->y);
	qr->align     = get_align(args, "align",  fresh ? ALIGN_LEFT : qr->align);
	qr->valign    = get_valign(args, "valign", fresh ? VALIGN_TOP : qr->valign);
	qr->module_px = get_int(args, "module_px", fresh ? 0 : qr->module_px);
	qr->border    = get_int(args, "border",    fresh ? 4 : qr->border);
	qr->ecc       = get_int(args, "ecc",       fresh ? 1 : qr->ecc);  /* MEDIUM */

	const char *col    = get_string(args, "color",    NULL);
	const char *bg_col = get_string(args, "bg_color", NULL);
	if (col)            qr->color    = parse_color(col);
	else if (fresh)     qr->color    = parse_color("#000000ff");
	if (bg_col)         qr->bg_color = parse_color(bg_col);
	else if (fresh)     qr->bg_color = parse_color("#ffffffff");

	if (text) {
		strncpy(qr->text, text, sizeof(qr->text) - 1);
		qr->text[sizeof(qr->text) - 1] = '\0';
	}

	/* Master opacity. Only a fresh element or an explicit opacity cancels a
	 * running fade, so a merge update can change the QR mid-animation. */
	qr->opacity = get_float(args, "opacity", fresh ? 1.0f : qr->opacity);
	if (fresh || cJSON_GetObjectItem(args, "opacity"))
		qr->anim.active = 0;

	qr->active       = 1;
	st->needs_render = 1;
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static int cmd_remove_qr(splash_state_t *st, cJSON *args, int client_idx) {
	int id = get_int(args, "id", -1);
	if (id < 0) {
		send_response(st, client_idx,
		              create_response("error", "missing id"));
		return -1;
	}

	qr_element_t *qr = qr_find(st, id);
	if (qr) {
		qr->active       = 0;
		st->needs_render = 1;
	}
	send_response(st, client_idx, create_response("ok", NULL));
	return 0;
}

static const cmd_entry_t cmd_table[] = {
	{ "exit",            cmd_exit            },
	{ "suspend",         cmd_suspend         },
	{ "resume",          cmd_resume          },
	{ "status",          cmd_status          },
	{ "version",         cmd_version         },
	{ "running",         cmd_running         },
	{ "ready",           cmd_ready           },
	{ "clear",           cmd_clear           },
	{ "bg_color",        cmd_bg_color        },
	{ "image",           cmd_image           },
	{ "text",            cmd_text            },
	{ "remove_text",     cmd_remove_text     },
	{ "rect",            cmd_rect            },
	{ "remove_rect",     cmd_remove_rect     },
	{ "ellipse",         cmd_ellipse         },
	{ "remove_ellipse",  cmd_remove_ellipse  },
	{ "circle",          cmd_ellipse         },
	{ "remove_circle",   cmd_remove_ellipse  },
	{ "line",            cmd_line            },
	{ "remove_line",     cmd_remove_line     },
	{ "stepper",         cmd_stepper         },
	{ "remove_stepper",  cmd_remove_stepper  },
	{ "marquee",         cmd_marquee         },
	{ "remove_marquee",  cmd_remove_marquee  },
	{ "sprite",          cmd_sprite          },
	{ "remove_sprite",   cmd_remove_sprite   },
	{ "overlay",         cmd_overlay         },
	{ "remove_overlay",  cmd_remove_overlay  },
	{ "progress",        cmd_progress        },
	{ "update_progress", cmd_update_progress },
	{ "hide_progress",   cmd_hide_progress   },
	{ "remove_progress", cmd_remove_progress },
	{ "animate",         cmd_animate         },
	{ "spinner",         cmd_spinner         },
	{ "remove_spinner",  cmd_remove_spinner  },
	{ "console",         cmd_console         },
	{ "console_write",   cmd_console_write   },
	{ "remove_console",  cmd_remove_console  },
	{ "query",           cmd_query           },
	{ "arc",             cmd_arc             },
	{ "update_arc",      cmd_update_arc      },
	{ "hide_arc",        cmd_hide_arc        },
	{ "remove_arc",      cmd_remove_arc      },
	{ "qr",              cmd_qr              },
	{ "remove_qr",       cmd_remove_qr       },
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
	else
		LOGW("unknown startup command: %s", cmd_name);
	return -1;
}

/*
 * Run a command batch (a JSON array) or a single command object. Used by
 * both the socket path and the startup commands.
 */
int process_json_batch(splash_state_t *st, cJSON *root, int client_idx,
                       int *out_total, int *out_errors) {
	int total = 0, errors = 0;

	if (cJSON_IsArray(root)) {
		int count = cJSON_GetArraySize(root);
		for (int i = 0; i < count; i++) {
			cJSON *cmd = cJSON_GetArrayItem(root, i);
			if (!cJSON_IsObject(cmd))
				continue;
			/* Suppress per-command replies inside a batch: the client
			 * reads only one newline-terminated response per send(), so
			 * only the batch summary below should reach it. */
			total++;
			if (process_single_cmd(st, cmd, -1) < 0)
				errors++;
		}

		if (client_idx >= 0) {
			cJSON *resp = create_response(errors > 0 ? "partial" : "ok",
			                              NULL);
			cJSON_AddNumberToObject(resp, "total",  total);
			cJSON_AddNumberToObject(resp, "errors", errors);
			send_response(st, client_idx, resp);
		}
	} else if (cJSON_IsObject(root)) {
		total = 1;
		if (process_single_cmd(st, root, client_idx) < 0)
			errors = 1;
	} else {
		if (client_idx >= 0)
			send_response(st, client_idx,
			              create_response("error", "invalid command format"));
		errors = 1;
	}

	if (out_total)  *out_total  = total;
	if (out_errors) *out_errors = errors;
	return errors > 0 ? -1 : 0;
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

	int ret = process_json_batch(st, root, client_idx, NULL, NULL);
	cJSON_Delete(root);
	return ret;
}
