/*
 * utils.c - Miscellaneous helpers shared across the daemon:
 *           colour parsing, progress-bar themes, numeric clamping.
 */

#include "splash.h"

/* ========================================================================
 * Colour Parsing
 * ======================================================================== */

/*
 * Parse a hex colour string into a packed ARGB value.
 *
 * Accepted forms:  #RGB   #RRGGBB   #RRGGBBAA
 *
 * Anything malformed or of an unrecognised length falls back to opaque
 * white, so a bad value in a command shows up on screen rather than
 * silently rendering invisible.
 */
uint32_t parse_color(const char *str) {
	unsigned int r = 255, g = 255, b = 255, a = 255;

	if (!str || str[0] != '#')
		return argb(255, 255, 255, 255);

	switch ((int)strlen(str)) {
	case 4:					/* #RGB - each nibble is doubled */
		sscanf(str, "#%1x%1x%1x", &r, &g, &b);
		r *= 17;
		g *= 17;
		b *= 17;
		break;
	case 7:					/* #RRGGBB */
		sscanf(str, "#%02x%02x%02x", &r, &g, &b);
		break;
	case 9:					/* #RRGGBBAA */
		sscanf(str, "#%02x%02x%02x%02x", &r, &g, &b, &a);
		break;
	default:
		break;				/* unrecognised length -> white fallback */
	}

	return argb((uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/* ========================================================================
 * Progress Bar Colour Themes
 *
 * A built-in style only sets the four progress-bar colours. Every theme
 * shares the same dark track, border and text; just the fill colour
 * differs - so the common values are set once and the switch overrides
 * only what changes. style < 0 means "custom", and the caller supplies
 * the colours instead.
 * ======================================================================== */

void set_default_progress_colors(progress_bar_t *pb, int style) {
	if (style < 0)
		return;				/* custom colours: caller fills them in */

	pb->bg_color     = argb(255,  30,  30,  30);
	pb->border_color = argb(255,  60,  60,  60);
	pb->text_color   = argb(255, 255, 255, 255);

	switch (style) {
	case 0:  pb->bar_color = argb(255,   0, 150, 255); break;  /* blue   */
	case 1:  pb->bar_color = argb(255,   0, 200, 100); break;  /* green  */
	case 2:  pb->bar_color = argb(255, 255, 180,   0); break;  /* amber  */
	case 3:  pb->bar_color = argb(255, 255,  60,  60); break;  /* red    */
	case 4:  pb->bar_color = argb(255, 150,  50, 255); break;  /* purple */
	case 5:  pb->bar_color = argb(255,   0, 200, 200); break;  /* cyan   */
	default:
		/* Unknown style: a slightly lighter neutral theme. */
		pb->bg_color     = argb(255, 40, 40, 40);
		pb->bar_color    = argb(255,  0, 150, 255);
		pb->border_color = argb(255, 80, 80, 80);
		break;
	}
}

/* ========================================================================
 * Numeric Clamping
 * ======================================================================== */

/* Clamp an integer to the inclusive range [min, max]. */
int clamp(int val, int min, int max) {
	if (val < min)
		return min;
	if (val > max)
		return max;
	return val;
}

/* Clamp a float to the inclusive range [min, max]. */
float fclamp(float val, float min, float max) {
	if (val < min)
		return min;
	if (val > max)
		return max;
	return val;
}
