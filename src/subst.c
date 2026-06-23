/*
 * subst.c - ${NAME} variable substitution for config documents and commands.
 *
 * This is an orthogonal preprocessing pass that runs *below* the schema: it
 * fills ${...} placeholders, and only the result is interpreted as a scene
 * document or message. The schema itself is unaware of variables.
 *
 * Values are supplied with -D NAME=value (see main.c and splash-ctl.c) into a
 * small global registry, then json_substitute() walks a parsed cJSON tree and
 * expands every string *value* (object keys are never touched, so the
 * single-key element discriminator is never rewritten).
 *
 * Resolution is tree-level: the template must be valid JSON, so ${...} may
 * appear only inside string values. A token that is the *whole* value, e.g.
 * "value": "${V}", is replaced by the value's native JSON type (a
 * numeric-looking value becomes a number, true/false a boolean); a token
 * *embedded* in surrounding text stays a string. Because values are written
 * into already-parsed string nodes there is no JSON-escaping hazard.
 *
 * Syntax:  ${NAME}             value of NAME, or empty + warning if undefined
 *          ${NAME:-fallback}   value of NAME, or fallback if undefined
 *          $${                 a literal "${"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "splash.h"	/* pulls in cJSON/cJSON.h */

typedef struct {
	char *name;
	char *value;
} subst_var_t;

static subst_var_t *g_vars = NULL;
static int          g_nvars = 0;
static int          g_cap   = 0;

/* Register a "NAME=value" define. A later define of the same name overrides an
 * earlier one. Returns 0 on success, -1 if the spec has no '=' or empty name. */
int subst_define(const char *spec) {
	if (!spec)
		return -1;
	const char *eq = strchr(spec, '=');
	if (!eq || eq == spec)
		return -1;

	size_t namelen = (size_t)(eq - spec);

	/* Override an existing define with the same name. */
	for (int i = 0; i < g_nvars; i++) {
		if (strlen(g_vars[i].name) == namelen &&
		    strncmp(g_vars[i].name, spec, namelen) == 0) {
			char *nv = strdup(eq + 1);
			if (!nv)
				return -1;
			free(g_vars[i].value);
			g_vars[i].value = nv;
			return 0;
		}
	}

	if (g_nvars == g_cap) {
		int newcap = g_cap ? g_cap * 2 : 8;
		subst_var_t *nv = realloc(g_vars, (size_t)newcap * sizeof(*nv));
		if (!nv)
			return -1;
		g_vars = nv;
		g_cap  = newcap;
	}

	char *name  = malloc(namelen + 1);
	char *value = strdup(eq + 1);
	if (!name || !value) {
		free(name);
		free(value);
		return -1;
	}
	memcpy(name, spec, namelen);
	name[namelen] = '\0';

	g_vars[g_nvars].name  = name;
	g_vars[g_nvars].value = value;
	g_nvars++;
	return 0;
}

int subst_count(void) {
	return g_nvars;
}

void subst_free(void) {
	for (int i = 0; i < g_nvars; i++) {
		free(g_vars[i].name);
		free(g_vars[i].value);
	}
	free(g_vars);
	g_vars  = NULL;
	g_nvars = 0;
	g_cap   = 0;
}

/* Look up NAME (given by pointer + length). Returns its value or NULL. */
static const char *lookup(const char *name, size_t len) {
	for (int i = 0; i < g_nvars; i++) {
		if (strlen(g_vars[i].name) == len &&
		    strncmp(g_vars[i].name, name, len) == 0)
			return g_vars[i].value;
	}
	return NULL;
}

/* Append n bytes of src to a growing heap buffer (*buf, *len, *cap). */
static int append(char **buf, size_t *len, size_t *cap,
                  const char *src, size_t n) {
	if (*len + n + 1 > *cap) {
		size_t newcap = *cap ? *cap : 32;
		while (*len + n + 1 > newcap)
			newcap *= 2;
		char *nb = realloc(*buf, newcap);
		if (!nb)
			return -1;
		*buf = nb;
		*cap = newcap;
	}
	memcpy(*buf + *len, src, n);
	*len += n;
	(*buf)[*len] = '\0';
	return 0;
}

/*
 * Expand all tokens in `in` into a freshly allocated string. *was_whole is set
 * to 1 when the entire input was a single token (so the caller may coerce the
 * resulting value to a native JSON type). Returns NULL only on allocation
 * failure.
 */
static char *expand(const char *in, int *was_whole) {
	*was_whole = 0;

	char  *out = NULL;
	size_t len = 0, cap = 0;
	if (append(&out, &len, &cap, "", 0) < 0)
		return NULL;

	int token_count = 0;
	int token_at_start = 0;

	const char *p = in;
	while (*p) {
		/* Escape: "$${" yields a literal "${". */
		if (p[0] == '$' && p[1] == '$' && p[2] == '{') {
			if (append(&out, &len, &cap, "${", 2) < 0) goto oom;
			p += 3;
			continue;
		}

		if (p[0] == '$' && p[1] == '{') {
			const char *start = p;
			const char *close = strchr(p + 2, '}');
			if (!close) {
				/* Unterminated: emit verbatim and stop scanning. */
				if (append(&out, &len, &cap, p, strlen(p)) < 0) goto oom;
				break;
			}

			const char *name = p + 2;
			size_t      namelen = (size_t)(close - name);

			/* Optional ":-default" inside the braces. */
			const char *deflt    = NULL;
			size_t      deflen   = 0;
			const char *sep = NULL;
			for (const char *q = name; q + 1 <= close; q++) {
				if (q[0] == ':' && q[1] == '-') { sep = q; break; }
			}
			if (sep) {
				namelen = (size_t)(sep - name);
				deflt   = sep + 2;
				deflen  = (size_t)(close - deflt);
			}

			const char *val = lookup(name, namelen);
			if (val) {
				if (append(&out, &len, &cap, val, strlen(val)) < 0) goto oom;
			} else if (deflt) {
				if (append(&out, &len, &cap, deflt, deflen) < 0) goto oom;
			} else {
				fprintf(stderr,
				        "splash: warning: undefined variable ${%.*s}\n",
				        (int)namelen, name);
				/* resolves to empty */
			}

			if (start == in)
				token_at_start = 1;
			token_count++;
			p = close + 1;
			continue;
		}

		if (append(&out, &len, &cap, p, 1) < 0) goto oom;
		p++;
	}

	/* Whole-value token: exactly one token, at the very start, and the input
	 * ended right after it (nothing trailed). */
	if (token_count == 1 && token_at_start) {
		const char *close = strchr(in + 2, '}');
		if (close && *(close + 1) == '\0' && strncmp(in, "${", 2) == 0)
			*was_whole = 1;
	}

	return out;

oom:
	free(out);
	return NULL;
}

/* Looks like a JSON number? (optional sign, digits, fraction, exponent.) */
static int parse_number(const char *s, double *out) {
	if (!s || !*s)
		return 0;
	char *end = NULL;
	double d = strtod(s, &end);
	if (end == s || *end != '\0')
		return 0;
	*out = d;
	return 1;
}

/* Replace a parsed cJSON string node in place with a coerced native type.
 * Assumes `item` comes from cJSON_Parse (owned valuestring, no reference/const
 * flags), which holds for every call site here. */
static void coerce_in_place(cJSON *item, char *value) {
	double d;
	if (parse_number(value, &d)) {
		free(item->valuestring);
		item->valuestring = NULL;
		item->type        = cJSON_Number;
		item->valuedouble = d;
		item->valueint    = (int)d;
		free(value);
	} else if (strcmp(value, "true") == 0 || strcmp(value, "false") == 0) {
		int t = (value[0] == 't');
		free(item->valuestring);
		item->valuestring = NULL;
		item->type        = t ? cJSON_True : cJSON_False;
		item->valueint    = t;
		free(value);
	} else if (strcmp(value, "null") == 0) {
		free(item->valuestring);
		item->valuestring = NULL;
		item->type        = cJSON_NULL;
		free(value);
	} else {
		free(item->valuestring);
		item->valuestring = value;	/* keep as string */
	}
}

/* Recursively expand every string value in the tree, in place. */
void json_substitute(cJSON *root) {
	if (!root || g_nvars == 0)
		return;

	for (cJSON *item = (cJSON_IsObject(root) || cJSON_IsArray(root))
	                   ? root->child : NULL;
	     item; item = item->next) {

		if (cJSON_IsString(item) && item->valuestring &&
		    strstr(item->valuestring, "${")) {
			int whole = 0;
			char *expanded = expand(item->valuestring, &whole);
			if (!expanded)
				continue;		/* OOM: leave node untouched */
			if (whole)
				coerce_in_place(item, expanded);
			else {
				free(item->valuestring);
				item->valuestring = expanded;
			}
		} else if (cJSON_IsObject(item) || cJSON_IsArray(item)) {
			json_substitute(item);
		}
	}
}
