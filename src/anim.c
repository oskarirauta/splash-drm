/*
 * anim.c - Time-based animation engine.
 *
 * Every animation runs off a monotonic clock, so timing stays correct
 * regardless of frame rate. anim_tick() advances each element's opacity
 * animation and the background crossfade once per frame, and reports
 * whether anything is still moving - the main loop uses that to decide
 * whether to keep rendering at RENDER_FPS or block until the next
 * command arrives.
 */

#include "splash.h"
#include <time.h>

/* ========================================================================
 * Monotonic Clock
 * ======================================================================== */

/* Milliseconds from an arbitrary fixed point; only differences are used. */
uint64_t now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* ========================================================================
 * Easing
 * ======================================================================== */

/* Map linear progress t (0..1) onto an eased curve, also 0..1. */
static float ease(float t, int kind) {
	if (t < 0.0f)
		t = 0.0f;
	if (t > 1.0f)
		t = 1.0f;

	switch (kind) {
	case EASE_IN:
		return t * t;
	case EASE_OUT:
		return t * (2.0f - t);
	case EASE_IN_OUT:
		return t < 0.5f ? 2.0f * t * t
		                : -1.0f + (4.0f - 2.0f * t) * t;
	case EASE_LINEAR:
	default:
		return t;
	}
}

/* ========================================================================
 * Animation Step
 * ======================================================================== */

/*
 * Advance one opacity animation and write the current value into
 * *opacity. On the final tick the animation either:
 *   - ping-pongs (repeat): swap the endpoints and restart, or
 *   - finishes: clear anim->active, and if remove_on_end was set,
 *     deactivate the owning element through *elem_active.
 *
 * Returns 1 if the animation was active during this tick.
 */
static int run_opacity_anim(anim_t *a, uint64_t now,
                            float *opacity, int *elem_active) {
	if (!a->active)
		return 0;

	float t;
	if (a->duration_ms == 0)
		t = 1.0f;
	else if (now <= a->start_ms)
		t = 0.0f;
	else
		t = (float)(now - a->start_ms) / (float)a->duration_ms;

	if (t >= 1.0f) {
		if (a->repeat && a->duration_ms > 0) {
			/* Ping-pong: swap endpoints and step the anchor by exactly
			 * one period so render jitter does not accumulate. */
			float tmp = a->from;
			a->from     = a->to;
			a->to       = tmp;
			a->start_ms += a->duration_ms;
			*opacity    = a->from;
		} else {
			*opacity  = a->to;
			a->active = 0;
			if (a->remove_on_end && elem_active)
				*elem_active = 0;
		}
	} else {
		*opacity = a->from + (a->to - a->from) * ease(t, a->easing);
	}
	return 1;
}

/* ========================================================================
 * Per-frame Tick
 *
 * Returns non-zero if any animation or spinner is still active, i.e. the
 * main loop should render this frame and keep ticking at RENDER_FPS.
 * ======================================================================== */

int anim_tick(splash_state_t *st, uint64_t now) {
	int active = 0;

	for (int i = 0; i < MAX_TEXT_ELEMENTS; i++) {
		text_element_t *te = &st->texts[i];
		if (te->active && te->anim.active)
			active |= run_opacity_anim(&te->anim, now,
			                           &te->opacity, &te->active);
	}

	for (int i = 0; i < MAX_RECTANGLES; i++) {
		rect_element_t *re = &st->rects[i];
		if (re->active && re->anim.active)
			active |= run_opacity_anim(&re->anim, now,
			                           &re->opacity, &re->active);
	}

	for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
		progress_bar_t *pb = &st->bars[i];
		if (pb->active && pb->anim.active)
			active |= run_opacity_anim(&pb->anim, now,
			                           &pb->opacity, &pb->active);
		/* An indeterminate bar's sweep moves every frame. */
		if (pb->active && pb->indeterminate)
			active = 1;
	}

	for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
		image_overlay_t *ov = &st->overlays[i];
		if (ov->active && ov->anim.active) {
			int was = ov->active;
			active |= run_opacity_anim(&ov->anim, now,
			                           &ov->opacity, &ov->active);
			/* Faded out with remove_on_end: free the pixels. */
			if (was && !ov->active)
				free_image(&ov->img);
		}
	}

	for (int i = 0; i < MAX_SPINNERS; i++) {
		spinner_t *sp = &st->spinners[i];
		if (!sp->active)
			continue;
		if (sp->anim.active)
			run_opacity_anim(&sp->anim, now,
			                 &sp->opacity, &sp->active);
		/* An active spinner rotates every frame. */
		if (sp->active)
			active = 1;
	}

	for (int i = 0; i < MAX_ARC_BARS; i++) {
		arc_bar_t *ab = &st->arcs[i];
		if (ab->active && ab->anim.active)
			active |= run_opacity_anim(&ab->anim, now,
			                           &ab->opacity, &ab->active);
		if (ab->active && ab->indeterminate)
			active = 1;
	}

	for (int i = 0; i < MAX_CONSOLES; i++) {
		console_t *con = &st->consoles[i];
		if (con->active && con->anim.active)
			active |= run_opacity_anim(&con->anim, now,
			                           &con->opacity, &con->active);
	}

	for (int i = 0; i < MAX_QR_ELEMENTS; i++) {
		qr_element_t *qr = &st->qrs[i];
		if (qr->active && qr->anim.active)
			active |= run_opacity_anim(&qr->anim, now,
			                           &qr->opacity, &qr->active);
	}

	/* Background crossfade: the incoming image's opacity ramps 0 -> 1. */
	if (st->bg_anim.active) {
		int dummy = 1;
		run_opacity_anim(&st->bg_anim, now, &st->bg_opacity, &dummy);
		active = 1;
		if (!st->bg_anim.active) {		/* crossfade finished */
			free_image(&st->bg_prev);
			st->bg_prev_loaded = 0;
			st->bg_opacity     = 1.0f;
		}
	}

	return active;
}
