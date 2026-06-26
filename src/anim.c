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

/* Per-channel linear interpolation of two ARGB colours. */
static uint32_t lerp_color(uint32_t c0, uint32_t c1, float e) {
	int a0 = c0 >> 24, r0 = (c0 >> 16) & 0xFF, g0 = (c0 >> 8) & 0xFF, b0 = c0 & 0xFF;
	int a1 = c1 >> 24, r1 = (c1 >> 16) & 0xFF, g1 = (c1 >> 8) & 0xFF, b1 = c1 & 0xFF;
	int a = a0 + (int)((a1 - a0) * e + 0.5f);
	int r = r0 + (int)((r1 - r0) * e + 0.5f);
	int g = g0 + (int)((g1 - g0) * e + 0.5f);
	int b = b0 + (int)((b1 - b0) * e + 0.5f);
	return ((uint32_t)a << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

/* Write the value at eased position `e` into the anim's target. A NULL target
 * is the legacy opacity path: write the eased float into *opacity instead, so
 * existing opacity animations need no change. */
static void anim_apply(anim_t *a, float *opacity, float e) {
	if (a->target) {
		switch (a->kind) {
		case ANIM_INT:
			*(int *)a->target = (int)(a->from + (a->to - a->from) * e + 0.5f);
			break;
		case ANIM_COLOR:
			*(uint32_t *)a->target = lerp_color(a->from_color, a->to_color, e);
			break;
		default:
			*(float *)a->target = a->from + (a->to - a->from) * e;
			break;
		}
	} else if (opacity) {
		*opacity = a->from + (a->to - a->from) * e;
	}
}

/*
 * Advance one animation and write the current value into its target (or, for a
 * legacy opacity anim with no target, into *opacity). On the final tick it
 * either ping-pongs (repeat: swap endpoints and restart) or finishes (clear
 * anim->active, and if remove_on_end was set, deactivate the owning element
 * through *elem_active). Returns 1 if the animation was active this tick.
 */
static int run_anim(anim_t *a, uint64_t now,
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
			float tmp   = a->from; a->from = a->to; a->to = tmp;
			uint32_t tc = a->from_color;
			a->from_color = a->to_color; a->to_color = tc;
			a->start_ms += a->duration_ms;
			anim_apply(a, opacity, 0.0f);
		} else {
			anim_apply(a, opacity, 1.0f);
			a->active = 0;
			if (a->remove_on_end && elem_active)
				*elem_active = 0;
		}
	} else {
		anim_apply(a, opacity, ease(t, a->easing));
	}
	return 1;
}

/* Start or retarget a float tween onto *target. Reuses run_anim(): with a
 * non-NULL target and no element/opacity, each tick writes the eased value into
 * *target only. `from` is normally the current value, so retargeting mid-tween
 * continues smoothly from where it is instead of jumping. */
void anim_value_start(anim_t *a, float *target,
                      float from, float to, uint32_t ms) {
	a->active        = 1;
	a->easing        = EASE_IN_OUT;
	a->from          = from;
	a->to            = to;
	a->start_ms      = now_ms();
	a->duration_ms   = ms;
	a->remove_on_end = 0;
	a->repeat        = 0;
	a->kind          = ANIM_FLOAT;
	a->target        = target;
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
			active |= run_anim(&te->anim, now,
			                           &te->opacity, &te->active);
	}

	for (int i = 0; i < MAX_RECTANGLES; i++) {
		rect_element_t *re = &st->rects[i];
		if (re->active && re->anim.active)
			active |= run_anim(&re->anim, now,
			                           &re->opacity, &re->active);
	}

	for (int i = 0; i < MAX_ELLIPSES; i++) {
		ellipse_t *e = &st->ellipses[i];
		if (e->active && e->anim.active)
			active |= run_anim(&e->anim, now,
			                           &e->opacity, &e->active);
	}

	for (int i = 0; i < MAX_LINES; i++) {
		line_t *l = &st->lines[i];
		if (l->active && l->anim.active)
			active |= run_anim(&l->anim, now,
			                           &l->opacity, &l->active);
	}

	for (int i = 0; i < MAX_STEPPERS; i++) {
		stepper_t *s = &st->steppers[i];
		if (s->active && s->anim.active)
			active |= run_anim(&s->anim, now,
			                           &s->opacity, &s->active);
	}

	for (int i = 0; i < MAX_MARQUEES; i++) {
		marquee_t *m = &st->marquees[i];
		if (!m->active)
			continue;
		/* Advance the scroll on the wall clock; non-zero speed keeps the
		 * render loop ticking at RENDER_FPS so the text keeps moving. */
		if (m->speed != 0) {
			if (m->last_ms == 0)
				m->last_ms = now;
			uint64_t dt = now - m->last_ms;
			m->last_ms = now;
			m->offset += (float)m->speed * (float)dt / 1000.0f;
			active = 1;
		}
		if (m->anim.active)
			active |= run_anim(&m->anim, now,
			                           &m->opacity, &m->active);
	}

	for (int i = 0; i < MAX_SPRITES; i++) {
		sprite_t *sp = &st->sprites[i];
		if (!sp->active)
			continue;
		/* Advance the current frame on the wall clock. A looping animation,
		 * or one that has not yet reached its last frame, keeps the loop
		 * ticking so it keeps playing. */
		if (sp->frame_count > 1 && sp->fps > 0) {
			uint64_t dur = 1000u / (uint32_t)sp->fps;
			if (dur < 1) dur = 1;
			if (sp->last_ms == 0)
				sp->last_ms = now;
			if (now - sp->last_ms >= dur) {
				sp->last_ms = now;
				int next = sp->current + 1;
				if (next >= sp->frame_count)
					next = sp->loop ? 0 : sp->frame_count - 1;
				sp->current = next;
			}
			if (sp->loop || sp->current < sp->frame_count - 1)
				active = 1;
		}
		if (sp->anim.active)
			active |= run_anim(&sp->anim, now,
			                           &sp->opacity, &sp->active);
	}

	for (int i = 0; i < MAX_PROGRESS_BARS; i++) {
		progress_bar_t *pb = &st->bars[i];
		if (pb->active && pb->anim.active)
			active |= run_anim(&pb->anim, now,
			                           &pb->opacity, &pb->active);
		/* Smooth value tween: drives `value` toward its new target. */
		if (pb->active && pb->value_anim.active)
			active |= run_anim(&pb->value_anim, now, NULL, NULL);
		/* An indeterminate bar's sweep moves every frame. */
		if (pb->active && pb->indeterminate)
			active = 1;

		/* remove_at_full: once the bar settles at 100% (the smooth tween, if
		 * any, has finished), keep it on screen for exactly one rendered frame
		 * and remove it on the next tick. The `active = 1` on each branch forces
		 * that following frame even when nothing else is animating, so the loop
		 * does not block on poll() before the removal is drawn. */
		if (pb->active && pb->remove_at_full && !pb->indeterminate &&
		    !pb->value_anim.active && pb->value >= 1.0f) {
			if (pb->full_pending) {
				memset(pb, 0, sizeof(*pb));   /* full frame shown: drop it */
			} else {
				pb->full_pending = 1;         /* draw 100% now, remove next */
			}
			active = 1;
		} else if (pb->active) {
			pb->full_pending = 0;             /* not at full: reset the latch */
		}
	}

	for (int i = 0; i < MAX_IMAGE_OVERLAYS; i++) {
		image_overlay_t *ov = &st->overlays[i];
		if (ov->active && ov->anim.active) {
			int was = ov->active;
			active |= run_anim(&ov->anim, now,
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
			run_anim(&sp->anim, now,
			                 &sp->opacity, &sp->active);
		/* An active spinner rotates every frame. */
		if (sp->active)
			active = 1;
	}

	for (int i = 0; i < MAX_ARC_BARS; i++) {
		arc_bar_t *ab = &st->arcs[i];
		if (ab->active && ab->anim.active)
			active |= run_anim(&ab->anim, now,
			                           &ab->opacity, &ab->active);
		/* Smooth value tween: drives `value` toward its new target. */
		if (ab->active && ab->value_anim.active)
			active |= run_anim(&ab->value_anim, now, NULL, NULL);
		if (ab->active && ab->indeterminate)
			active = 1;
	}

	for (int i = 0; i < MAX_CONSOLES; i++) {
		console_t *con = &st->consoles[i];
		if (con->active && con->anim.active)
			active |= run_anim(&con->anim, now,
			                           &con->opacity, &con->active);
	}

	for (int i = 0; i < MAX_QR_ELEMENTS; i++) {
		qr_element_t *qr = &st->qrs[i];
		if (qr->active && qr->anim.active)
			active |= run_anim(&qr->anim, now,
			                           &qr->opacity, &qr->active);
	}

	/* Background crossfade: the incoming image's opacity ramps 0 -> 1. */
	if (st->bg_anim.active) {
		int dummy = 1;
		run_anim(&st->bg_anim, now, &st->bg_opacity, &dummy);
		active = 1;
		if (!st->bg_anim.active) {		/* crossfade finished */
			free_image(&st->bg_prev);
			st->bg_opacity     = 1.0f;
		}
	}

	return active;
}
