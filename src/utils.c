/*
 * utils.c - Miscellaneous helpers shared across the daemon:
 *           colour parsing, progress-bar themes, numeric clamping.
 */

#include "splash.h"

/* ========================================================================
 * Colour Parsing
 * ======================================================================== */

/*
 * Parse a colour value into a packed ARGB value.
 *
 * Accepted forms:
 *   #RGB          3-digit hex, each nibble doubled
 *   #RRGGBB       6-digit hex, fully opaque
 *   #RRGGBBAA     8-digit hex with explicit alpha
 *   name          CSS-style name ("white", "transparent", …)
 *
 * Anything unrecognised falls back to opaque white so a bad value in a
 * command shows up on screen rather than rendering invisible.
 */
uint32_t parse_color(const char *str) {
	if (!str)
		return argb(255, 255, 255, 255);

	/* Named colours (case-sensitive, lowercase). */
	static const struct { const char *name; uint32_t val; } names[] = {
		{ "transparent", 0x00000000u },
		{ "black",       0xFF000000u },
		{ "white",       0xFFFFFFFFu },
		{ "red",         0xFFFF0000u },
		{ "green",       0xFF00FF00u },
		{ "blue",        0xFF0000FFu },
		{ "yellow",      0xFFFFFF00u },
		{ "cyan",        0xFF00FFFFu },
		{ "magenta",     0xFFFF00FFu },
		{ "orange",      0xFFFF8000u },
		{ "gray",        0xFF808080u },
		{ "grey",        0xFF808080u },
		{ "darkgray",    0xFF404040u },
		{ "darkgrey",    0xFF404040u },
		{ "lightgray",   0xFFC0C0C0u },
		{ "lightgrey",   0xFFC0C0C0u },
		{ NULL, 0u }
	};
	for (int i = 0; names[i].name; i++) {
		if (strcmp(str, names[i].name) == 0)
			return names[i].val;
	}

	if (str[0] != '#')
		return argb(255, 255, 255, 255);

	unsigned int r = 255, g = 255, b = 255, a = 255;
	switch ((int)strlen(str)) {
	case 4:				/* #RGB – each nibble is doubled */
		sscanf(str, "#%1x%1x%1x", &r, &g, &b);
		r *= 17; g *= 17; b *= 17;
		break;
	case 7:				/* #RRGGBB */
		sscanf(str, "#%02x%02x%02x", &r, &g, &b);
		break;
	case 9:				/* #RRGGBBAA */
		sscanf(str, "#%02x%02x%02x%02x", &r, &g, &b, &a);
		break;
	default:
		break;			/* unrecognised → white fallback */
	}
	return argb((uint8_t)a, (uint8_t)r, (uint8_t)g, (uint8_t)b);
}

/*
 * Like get_int() but accepts a percentage string ("80%") which is resolved
 * relative to `dim` (the relevant screen dimension).  Pure numbers behave
 * identically to get_int().  Missing key returns `default_val`.
 */
int get_coord(cJSON *obj, const char *key, int dim, int default_val) {
	cJSON *item = cJSON_GetObjectItem(obj, key);
	if (!item)
		return default_val;
	if (cJSON_IsNumber(item))
		return item->valueint;
	if (cJSON_IsString(item)) {
		const char *s = item->valuestring;
		size_t len = strlen(s);
		if (len > 1 && s[len - 1] == '%') {
			/* Reject inf/nan and anything that would overflow the int cast
			 * (e.g. "1e40%"): converting such a value is undefined. */
			double v = (double)dim * atof(s) / 100.0 + 0.5;
			if (!isfinite(v) || v <= (double)INT_MIN || v >= (double)INT_MAX)
				return default_val;
			return (int)v;
		}
	}
	return default_val;
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
 * Path Resolution
 *
 * Fonts and images may be referenced by bare filename ("logo.png") or by
 * absolute path ("/boot/logo.png"). For bare names, a small list of
 * well-known directories is searched in order so the caller never needs to
 * know the installation prefix.
 * ======================================================================== */

static int resolve_path(const char *path,
                        const char * const *dirs, int ndirs,
                        char *out, size_t outsz) {
    struct stat st;

    if (!path || !*path)
        return -1;

    /* Absolute path: use as-is, no search. */
    if (path[0] == '/') {
        if (stat(path, &st) == 0) {
            snprintf(out, outsz, "%s", path);
            return 0;
        }
        return -1;
    }

    for (int i = 0; i < ndirs; i++) {
        snprintf(out, outsz, "%s%s", dirs[i], path);
        if (stat(out, &st) == 0)
            return 0;
    }
    return -1;
}

int resolve_font_path(const char *path, char *out, size_t outsz) {
    static const char * const dirs[] = {
        "",
        "/usr/share/splash/fonts/",
        "/usr/share/splash/",
        "/usr/share/fonts/",
    };
    return resolve_path(path, dirs, 4, out, outsz);
}

int resolve_image_path(const char *path, char *out, size_t outsz) {
    static const char * const dirs[] = {
        "",
        "/usr/share/splash/images/",
        "/usr/share/splash/",
    };
    return resolve_path(path, dirs, 3, out, outsz);
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
