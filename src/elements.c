/*
 * elements.c - Allocation, lookup and cleanup for the visual elements
 *              held in splash_state_t (text, overlays, rectangles,
 *              progress bars and spinners).
 *
 * Every element type uses the same fixed-slot pattern: a find() that
 * locates an active element by id, and an alloc() that claims the first
 * free slot. The daemon never heap-allocates elements, which keeps
 * memory use bounded and predictable on small systems.
 */

#include "splash.h"

/* ========================================================================
 * Text Elements
 * ======================================================================== */

text_element_t *text_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
		if (st->texts[i].active && st->texts[i].id == id)
			return &st->texts[i];
	}
	return NULL;
}

text_element_t *text_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
		if (!st->texts[i].active) {
			memset(&st->texts[i], 0, sizeof(text_element_t));
			st->texts[i].active  = 1;
			st->texts[i].opacity = 1.0f;
			return &st->texts[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Image Overlays
 * ======================================================================== */

image_overlay_t *overlay_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
		if (st->overlays[i].active && st->overlays[i].id == id)
			return &st->overlays[i];
	}
	return NULL;
}

image_overlay_t *overlay_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
		if (!st->overlays[i].active) {
			memset(&st->overlays[i], 0, sizeof(image_overlay_t));
			st->overlays[i].active  = 1;
			st->overlays[i].opacity = 1.0f;
			return &st->overlays[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Rectangle Elements
 * ======================================================================== */

rect_element_t *rect_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_RECTANGLES; i++) {
		if (st->rects[i].active && st->rects[i].id == id)
			return &st->rects[i];
	}
	return NULL;
}

rect_element_t *rect_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_RECTANGLES; i++) {
		if (!st->rects[i].active) {
			memset(&st->rects[i], 0, sizeof(rect_element_t));
			st->rects[i].active  = 1;
			st->rects[i].opacity = 1.0f;
			return &st->rects[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Ellipses / Circles
 * ======================================================================== */

ellipse_t *ellipse_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_ELLIPSES; i++) {
		if (st->ellipses[i].active && st->ellipses[i].id == id)
			return &st->ellipses[i];
	}
	return NULL;
}

ellipse_t *ellipse_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_ELLIPSES; i++) {
		if (!st->ellipses[i].active) {
			memset(&st->ellipses[i], 0, sizeof(ellipse_t));
			st->ellipses[i].active  = 1;
			st->ellipses[i].opacity = 1.0f;
			return &st->ellipses[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Lines
 * ======================================================================== */

line_t *line_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_LINES; i++) {
		if (st->lines[i].active && st->lines[i].id == id)
			return &st->lines[i];
	}
	return NULL;
}

line_t *line_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_LINES; i++) {
		if (!st->lines[i].active) {
			memset(&st->lines[i], 0, sizeof(line_t));
			st->lines[i].active  = 1;
			st->lines[i].opacity = 1.0f;
			return &st->lines[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Steppers
 * ======================================================================== */

stepper_t *stepper_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_STEPPERS; i++) {
		if (st->steppers[i].active && st->steppers[i].id == id)
			return &st->steppers[i];
	}
	return NULL;
}

stepper_t *stepper_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_STEPPERS; i++) {
		if (!st->steppers[i].active) {
			memset(&st->steppers[i], 0, sizeof(stepper_t));
			st->steppers[i].active  = 1;
			st->steppers[i].opacity = 1.0f;
			return &st->steppers[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Marquees
 * ======================================================================== */

marquee_t *marquee_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_MARQUEES; i++) {
		if (st->marquees[i].active && st->marquees[i].id == id)
			return &st->marquees[i];
	}
	return NULL;
}

marquee_t *marquee_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_MARQUEES; i++) {
		if (!st->marquees[i].active) {
			marquee_free_cache(&st->marquees[i]);	/* free before memset drops the ptr */
			memset(&st->marquees[i], 0, sizeof(marquee_t));
			st->marquees[i].active  = 1;
			st->marquees[i].opacity = 1.0f;
			return &st->marquees[i];
		}
	}
	return NULL;
}

void marquee_free_cache(marquee_t *m) {
	if (!m)
		return;
	free(m->cov_cache);
	m->cov_cache   = NULL;
	m->cov_cache_w = 0;
	m->cov_cache_h = 0;
	m->cov_dirty   = 1;
}

/* ========================================================================
 * Sprites (frame animations)
 * ======================================================================== */

void sprite_clear_frames(sprite_t *sp) {
	if (!sp)
		return;
	for (int i = 0; i < sp->frame_count; i++)
		free_image(&sp->frames[i]);
	sp->frame_count = 0;
	sp->current     = 0;
}

sprite_t *sprite_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_SPRITES; i++) {
		if (st->sprites[i].active && st->sprites[i].id == id)
			return &st->sprites[i];
	}
	return NULL;
}

sprite_t *sprite_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_SPRITES; i++) {
		if (!st->sprites[i].active) {
			memset(&st->sprites[i], 0, sizeof(sprite_t));
			st->sprites[i].active  = 1;
			st->sprites[i].opacity = 1.0f;
			return &st->sprites[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Spinners
 *
 * Unlike the other elements a spinner has two flags: `used` marks the
 * slot as configured and `active` marks it as currently visible. This
 * lets "hide" stop rendering without losing the configuration, so a
 * later "show" can reuse the same slot. spinner_find() therefore matches
 * on `used`, not `active`.
 * ======================================================================== */

spinner_t *spinner_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_SPINNERS; i++) {
		if (st->spinners[i].used && st->spinners[i].id == id)
			return &st->spinners[i];
	}
	return NULL;
}

spinner_t *spinner_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_SPINNERS; i++) {
		if (!st->spinners[i].used) {
			memset(&st->spinners[i], 0, sizeof(spinner_t));
			st->spinners[i].used    = 1;
			st->spinners[i].active  = 1;
			st->spinners[i].opacity = 1.0f;
			return &st->spinners[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Progress Bars
 * ======================================================================== */

progress_bar_t *progress_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
		if (st->bars[i].active && st->bars[i].id == id)
			return &st->bars[i];
	}
	return NULL;
}

progress_bar_t *progress_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
		if (!st->bars[i].active) {
			memset(&st->bars[i], 0, sizeof(progress_bar_t));
			st->bars[i].active  = 1;
			st->bars[i].opacity = 1.0f;
			return &st->bars[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Arc Progress Bars
 * ======================================================================== */

arc_bar_t *arc_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_ARC_BARS; i++) {
		if (st->arcs[i].active && st->arcs[i].id == id)
			return &st->arcs[i];
	}
	return NULL;
}

arc_bar_t *arc_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_ARC_BARS; i++) {
		if (!st->arcs[i].active) {
			memset(&st->arcs[i], 0, sizeof(arc_bar_t));
			st->arcs[i].active  = 1;
			st->arcs[i].opacity = 1.0f;
			return &st->arcs[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Console Elements
 * ======================================================================== */

console_t *console_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_CONSOLES; i++) {
		if (st->consoles[i].active && st->consoles[i].id == id)
			return &st->consoles[i];
	}
	return NULL;
}

console_t *console_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_CONSOLES; i++) {
		if (!st->consoles[i].active) {
			memset(&st->consoles[i], 0, sizeof(console_t));
			st->consoles[i].active  = 1;
			st->consoles[i].opacity = 1.0f;
			return &st->consoles[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * QR Code Elements
 * ======================================================================== */

qr_element_t *qr_find(splash_state_t *st, int id) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_QR_ELEMENTS; i++) {
		if (st->qrs[i].active && st->qrs[i].id == id)
			return &st->qrs[i];
	}
	return NULL;
}

qr_element_t *qr_alloc(splash_state_t *st) {
	if (!st)
		return NULL;

	for (int i = 0; i < MAX_QR_ELEMENTS; i++) {
		if (!st->qrs[i].active) {
			memset(&st->qrs[i], 0, sizeof(qr_element_t));
			st->qrs[i].active  = 1;
			st->qrs[i].opacity = 1.0f;
			return &st->qrs[i];
		}
	}
	return NULL;
}

/* ========================================================================
 * Global Cleanup
 * ======================================================================== */

/*
 * Deactivate every element and release everything they own. Used by the
 * "clear" command and at shutdown. Loaded fonts are deliberately left
 * alone - they live in font.c, not in splash_state_t, and cannot be
 * reloaded over the control socket.
 */
void clear_all_elements(splash_state_t *st) {
	if (!st)
		return;

	for (int i = 0; i < MAX_TEXT_ELEMENTS; i++)
		st->texts[i].active = 0;

	/* Overlays own a decoded image, so free the pixels too. */
	for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
		free_image(&st->overlays[i].img);
		st->overlays[i].active = 0;
	}

	for (int i = 0; i < MAX_RECTANGLES; i++)
		st->rects[i].active = 0;

	for (int i = 0; i < MAX_ELLIPSES; i++)
		st->ellipses[i].active = 0;

	for (int i = 0; i < MAX_LINES; i++)
		st->lines[i].active = 0;

	for (int i = 0; i < MAX_STEPPERS; i++)
		st->steppers[i].active = 0;

	for (int i = 0; i < MAX_MARQUEES; i++) {
		marquee_free_cache(&st->marquees[i]);
		st->marquees[i].active = 0;
	}

	for (int i = 0; i < MAX_SPRITES; i++) {
		sprite_clear_frames(&st->sprites[i]);
		st->sprites[i].active = 0;
	}

	for (int i = 0; i < MAX_PROGRESS_BARS; i++)
		st->bars[i].active = 0;

	for (int i = 0; i < MAX_SPINNERS; i++) {
		st->spinners[i].active = 0;
		st->spinners[i].used   = 0;
	}

	for (int i = 0; i < MAX_CONSOLES; i++)
		st->consoles[i].active = 0;

	for (int i = 0; i < MAX_ARC_BARS; i++)
		st->arcs[i].active = 0;

	for (int i = 0; i < MAX_QR_ELEMENTS; i++)
		st->qrs[i].active = 0;

	/* Background, plus any crossfade still in flight. */
	st->bg_loaded = 0;
	free_image(&st->bg_image);
	free_image(&st->bg_prev);
	free_image(&st->bg_cache);
	st->bg_cache_dirty = 1;
	st->bg_anim.active = 0;
	st->bg_opacity     = 1.0f;
}
