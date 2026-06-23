/*
 * splash-ctl.c - JSON control client for the splash-drm daemon.
 *
 * Connects to the daemon's abstract UNIX socket, sends one JSON command
 * (a single object or an array of objects), prints the reply, and exits.
 * The JSON is taken from the command line or, with --file, from a file.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

#include "splash.h"

#ifdef SOCKET_NAME
#undef SOCKET_NAME
#endif

#define SOCKET_NAME "splash-drm"

static int debug_mode = 0;
static int raw_mode   = 0;

/* ========================================================================
 * Send a Command
 * ======================================================================== */

/*
 * Connect, send one JSON line, and read the reply into the caller's buffer
 * (NUL-terminated). Returns the number of bytes read (>= 0) on success, or -1
 * if the daemon could not be reached or the write failed. With quiet != 0 the
 * connect/write failure is silent — used by the liveness probe, which reports
 * "not running" itself rather than as an error.
 */
static ssize_t send_and_recv(const char *json_str, char *reply, size_t replysz,
                             int quiet) {
	if (debug_mode)
		fprintf(stderr, "[debug] Sending: %s\n", json_str);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		if (!quiet)
			perror("socket");
		return -1;
	}

	/* Abstract socket: sun_path[0] == '\0', name follows in the rest. */
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family  = AF_UNIX;
	addr.sun_path[0] = '\0';
	strncpy(addr.sun_path + 1, SOCKET_NAME, sizeof(addr.sun_path) - 2);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		if (!quiet)
			fprintf(stderr, "Cannot connect to splash-drm daemon: %s\n",
			        strerror(errno));
		close(fd);
		return -1;
	}

	size_t len = strlen(json_str);
	if (write(fd, json_str, len) != (ssize_t)len ||
	    write(fd, "\n", 1) != 1) {
		if (!quiet)
			fprintf(stderr, "Failed to write command\n");
		close(fd);
		return -1;
	}

	/*
	 * Read the reply. A single read() can return a partial line, so loop
	 * until a newline arrives, the buffer fills, or the peer closes.
	 */
	size_t total = 0;
	while (total < replysz - 1) {
		ssize_t n = read(fd, reply + total, replysz - 1 - total);
		if (n <= 0)
			break;
		total += (size_t)n;
		if (memchr(reply, '\n', total))
			break;
	}
	reply[total] = '\0';
	close(fd);
	return (ssize_t)total;
}

/* Extract a JSON string field ("key":"value") into out. Returns 1 on success,
 * 0 if the key is absent. Tolerant flat-scan parser, like the message handling
 * below - good enough for the daemon's small, well-formed replies. */
static int extract_json_string(const char *json, const char *key,
                               char *out, size_t outsz) {
	char pat[64];
	snprintf(pat, sizeof(pat), "\"%s\":\"", key);
	const char *p = strstr(json, pat);
	if (!p)
		return 0;
	p += strlen(pat);
	const char *end = strchr(p, '"');
	if (!end)
		return 0;
	size_t n = (size_t)(end - p);
	if (n >= outsz)
		n = outsz - 1;
	memcpy(out, p, n);
	out[n] = '\0';
	return 1;
}

/*
 * Ask the running daemon what version it is, and print it. This is distinct
 * from --version, which reports this client's own build: here we want the
 * version of the live daemon, which may be older if a stale initramfs is still
 * holding the screen. A daemon predating 4.0.1 doesn't know the command and
 * answers "unknown command" - we translate that into a clear diagnostic.
 */
static int print_daemon_version(void) {
	char reply[4096];
	ssize_t total = send_and_recv("{\"system\":\"version\"}", reply, sizeof(reply), 0);
	if (total < 0)
		return 1;			/* connect/write already reported */

	char version[64];
	if (extract_json_string(reply, "version", version, sizeof(version))) {
		printf("splash-drm daemon v%s\n", version);
		return 0;
	}

	/* No version field: most likely an older daemon that rejects the command. */
	if (strstr(reply, "unknown command")) {
		fprintf(stderr,
			"The running daemon does not report a version (older than 4.0.1).\n"
			"A stale initramfs may be running an outdated splash-drm - rebuild it.\n");
	} else {
		fprintf(stderr, "Unexpected reply from daemon: %s\n",
		        total > 0 ? reply : "(empty)");
	}
	return 1;
}

/*
 * Liveness check that never fails with a connection error. Probes the daemon
 * (any reply, or even just a successful connect, means it is up) and reports the
 * result: with json != 0 as {"running":true|false}, otherwise as the words
 * running / not running. Returns 0 if the daemon is up, 1 if not, so the exit
 * code is itself the answer.
 */
static int report_running(int json) {
	char reply[4096];
	int up = send_and_recv("{\"system\":\"running\"}", reply, sizeof(reply), 1) >= 0;
	if (json)
		printf(up ? "{\"running\":true}\n" : "{\"running\":false}\n");
	else
		printf(up ? "running\n" : "not running\n");
	return up ? 0 : 1;
}

/* A bare {"cmd":"running"} is answered locally (see report_running) so it always
 * yields a boolean rather than a connection error. Detect it tolerantly. */
static int is_running_query(const char *s) {
	return strstr(s, "\"system\"") && strstr(s, "\"running\"");
}

static int send_json(const char *json_str) {
	char reply[4096];
	ssize_t total = send_and_recv(json_str, reply, sizeof(reply), 0);
	if (total < 0)
		return -1;
	if (total == 0)
		return 0;

	if (raw_mode || debug_mode) {
		printf("%s\n", reply);
		return 0;
	}

	/* Pretty-print the basic status. */
	if (strstr(reply, "\"status\":\"ok\"")) {
		printf("OK\n");
	} else if (strstr(reply, "\"status\":\"error\"")) {
		printf("ERROR\n");
		char *msg = strstr(reply, "\"message\":\"");
		if (msg) {
			msg += 11;
			char *end = strchr(msg, '\"');
			if (end) {
				printf("  ");
				fwrite(msg, 1, end - msg, stdout);
				printf("\n");
			}
		}
	} else {
		printf("%s\n", reply);
	}

	return 0;
}

/* ========================================================================
 * File Input
 * ======================================================================== */

/* Read a whole file into a newly allocated, NUL-terminated buffer. */
static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f) {
		perror(path);
		return NULL;
	}

	if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
	long size = ftell(f);
	if (size < 0)                   { fclose(f); return NULL; }
	rewind(f);

	char *buf = malloc((size_t)size + 1);
	if (!buf)                       { fclose(f); return NULL; }

	size_t got = fread(buf, 1, (size_t)size, f);
	fclose(f);
	buf[got] = '\0';			/* terminate at however much was read */
	return buf;
}

/* ========================================================================
 * Usage
 * ======================================================================== */

static void print_usage(const char *prog) {
	fprintf(stderr,
		"splash-ctl v%s - JSON control client for splash-drm\n\n"
		"Usage: %s [options] <json-string>\n"
		"       %s [options] --file <json-file>\n\n"
		"Options:\n"
		"  --raw         Output the raw JSON response\n"
		"  --file        Read JSON from a file instead of the argument\n"
		"  --debug       Show debug output\n"
		"  --help        Show this help and exit\n"
		"  --help <cmd>  Show full parameter list for a command\n"
		"  -V, --version Show this client's version and exit\n"
		"  --daemon-version  Ask the running daemon its version and exit\n\n"
		"System command shortcuts (instead of the raw JSON):\n"
		"  --status        Print the daemon status JSON ({\"cmd\":\"status\"})\n"
		"  --running       Print running / not running and exit 0 / 1 (never\n"
		"                  a connection error)\n"
		"  --suspend       Freeze rendering        ({\"cmd\":\"suspend\"})\n"
		"  --resume        Resume rendering         ({\"cmd\":\"resume\"})\n"
		"  --exit          Shut the daemon down     ({\"cmd\":\"exit\"})\n"
		"  --timeout <s>   With --exit: keep rendering for <s> seconds first\n\n"
		"JSON format (single command):\n"
		"  {\"cmd\": \"text\", \"id\": 0, \"x\": -1, \"y\": -1, \"text\": \"Hello\"}\n\n"
		"JSON format (batch):\n"
		"  [{\"cmd\": \"clear\"}, {\"cmd\": \"image\", \"path\": \"splash.png\"}]\n\n"
		"Available commands:\n"
		"  Background:  clear     bg_color         image\n"
		"  Text:        text      remove_text\n"
		"  Rectangle:   rect      remove_rect\n"
		"  Ellipse:     ellipse   circle           remove_ellipse  remove_circle\n"
		"  Line:        line      remove_line\n"
		"  Stepper:     stepper   remove_stepper\n"
		"  Marquee:     marquee   remove_marquee\n"
		"  Sprite:      sprite    remove_sprite\n"
		"  Image:       overlay   remove_overlay\n"
		"  Progress:    progress  update_progress  hide_progress\n"
		"  Arc:         arc       update_arc       hide_arc\n"
		"  Spinner:     spinner\n"
		"  Console:     console   console_write    remove_console\n"
		"  QR code:     qr        remove_qr\n"
		"  Animation:   animate\n"
		"  Query:       query     status           version         running\n"
		"  Control:     suspend   resume           ready           exit\n\n"
		"Run 'splash-ctl --help <cmd>' for full parameter details.\n\n"
		"Examples:\n"
		"  %s '{\"cmd\":\"image\",\"path\":\"boot.png\",\"crossfade\":600}'\n"
		"  %s '{\"cmd\":\"text\",\"id\":0,\"text\":\"Booting...\",\"size\":32}'\n"
		"  %s '{\"cmd\":\"progress\",\"id\":0,\"value\":0.5}'\n"
		"  %s '{\"cmd\":\"arc\",\"id\":1,\"indeterminate\":true}'\n"
		"  %s '{\"cmd\":\"console\",\"id\":2,\"w\":900,\"h\":200}'\n"
		"  %s '{\"cmd\":\"console_write\",\"id\":2,\"text\":\"Starting services...\"}'\n"
		"  %s '{\"cmd\":\"qr\",\"id\":3,\"text\":\"https://example.com\"}'\n"
		"  %s '{\"cmd\":\"animate\",\"type\":\"text\",\"id\":0,\"to\":0,\"duration\":400}'\n"
		"  %s --file /etc/splash-commands.json\n"
		"  %s '{\"cmd\":\"status\"}'\n"
		"  %s --suspend\n"
		"  %s --exit\n"
		"  %s --exit --timeout 3\n",
		SPLASH_VERSION,
		prog, prog,
		prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog, prog,
		prog);
}

/* ========================================================================
 * Entry Point
 * ======================================================================== */

int main(int argc, char **argv) {
	int arg_offset = 1;
	int use_file   = 0;

	/* Pull -D/--define NAME=value out of argv wherever they appear — they may
	 * follow the positional JSON or the --file argument (e.g.
	 * `splash-ctl --file cmds.json -D MSG="hi"`). Register each define and
	 * compact the rest of argv for the normal flag/positional parse below. */
	{
		int w = 1;
		for (int r = 1; r < argc; r++) {
			const char *a = argv[r];
			if ((strcmp(a, "-D") == 0 || strcmp(a, "--define") == 0) &&
			    r + 1 < argc) {
				if (subst_define(argv[++r]) != 0) {
					fprintf(stderr, "Invalid -D '%s' "
					        "(expected NAME=value)\n", argv[r]);
					return 1;
				}
			} else if (strncmp(a, "-D", 2) == 0 && a[2] != '\0') {
				if (subst_define(a + 2) != 0) {
					fprintf(stderr, "Invalid -D '%s' "
					        "(expected NAME=value)\n", a + 2);
					return 1;
				}
			} else {
				argv[w++] = argv[r];
			}
		}
		argc = w;
	}

	/* Convenience flags for the "system" commands, so scripts can write
	 * `splash-ctl --exit` instead of the raw JSON and have those lines stand
	 * out from the drawing commands. action holds the cmd name; timeout is the
	 * exit delay in seconds (only meaningful with --exit). */
	const char *action  = NULL;
	int         timeout = -1;

	while (arg_offset < argc && argv[arg_offset][0] == '-') {
		if (strcmp(argv[arg_offset], "--debug") == 0) {
			debug_mode = 1;
			arg_offset++;
		} else if (strcmp(argv[arg_offset], "--raw") == 0) {
			raw_mode = 1;
			arg_offset++;
		} else if (strcmp(argv[arg_offset], "--file") == 0) {
			use_file = 1;
			arg_offset++;
		} else if (strcmp(argv[arg_offset], "--help") == 0 ||
		           strcmp(argv[arg_offset], "-h") == 0) {
			if (arg_offset + 1 < argc && argv[arg_offset + 1][0] != '-') {
				print_cmd_help(argv[arg_offset + 1]);
				return 0;
			}
			print_usage(argv[0]);
			return 0;
		} else if (strcmp(argv[arg_offset], "--version") == 0 ||
			   strcmp(argv[arg_offset], "-V") == 0 ||
			   strcmp(argv[arg_offset], "-v") == 0) {	/* -v kept as a deprecated alias */
			printf("splash-ctl v%s\n", SPLASH_VERSION);
			return 0;
		} else if (strcmp(argv[arg_offset], "--daemon-version") == 0) {
			return print_daemon_version();
		} else if (strcmp(argv[arg_offset], "--running") == 0) {
			return report_running(0);	/* "running" / "not running" */
		} else if (strcmp(argv[arg_offset], "--status") == 0) {
			raw_mode = 1;			/* show the status JSON, not just OK */
			return send_json("{\"system\":\"status\"}") < 0 ? 1 : 0;
		} else if (strcmp(argv[arg_offset], "--exit") == 0) {
			action = "exit";
			arg_offset++;
		} else if (strcmp(argv[arg_offset], "--suspend") == 0) {
			action = "suspend";
			arg_offset++;
		} else if (strcmp(argv[arg_offset], "--resume") == 0) {
			action = "resume";
			arg_offset++;
		} else if (strcmp(argv[arg_offset], "--timeout") == 0) {
			if (arg_offset + 1 >= argc) {
				fprintf(stderr, "--timeout requires a value in seconds\n");
				return 1;
			}
			char *endp = NULL;
			long t = strtol(argv[arg_offset + 1], &endp, 10);
			if (*endp != '\0' || t < 0) {
				fprintf(stderr,
				        "--timeout value must be a non-negative integer\n");
				return 1;
			}
			timeout = (int)t;
			arg_offset += 2;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[arg_offset]);
			return 1;
		}
	}

	/* --timeout only shapes --exit (a delayed shutdown). */
	if (timeout >= 0 && (action == NULL || strcmp(action, "exit") != 0)) {
		fprintf(stderr, "--timeout only applies to --exit\n");
		return 1;
	}

	/* An action flag is terminal: build its JSON and send it. */
	if (action) {
		char buf[80];
		if (timeout >= 0)
			snprintf(buf, sizeof(buf),
			         "{\"system\":{\"action\":\"exit\",\"timeout\":%d}}",
			         timeout);
		else
			snprintf(buf, sizeof(buf), "{\"system\":\"%s\"}", action);
		return send_json(buf) < 0 ? 1 : 0;
	}

	if (arg_offset >= argc) {
		print_usage(argv[0]);
		return 1;
	}

	char *json_str  = NULL;
	int   needs_free = 0;

	if (use_file) {
		json_str = read_file(argv[arg_offset]);
		if (!json_str)
			return 1;
		needs_free = 1;
	} else {
		/* Concatenate all remaining arguments, space-separated. */
		size_t total = 0;
		for (int i = arg_offset; i < argc; i++)
			total += strlen(argv[i]) + 1;

		json_str = malloc(total + 1);
		if (!json_str) {
			fprintf(stderr, "Out of memory\n");
			return 1;
		}
		needs_free = 1;

		char *p = json_str;
		for (int i = arg_offset; i < argc; i++) {
			if (i > arg_offset)
				*p++ = ' ';
			strcpy(p, argv[i]);
			p += strlen(argv[i]);
		}
		*p = '\0';
	}

	/* With -D defines, expand ${NAME} tokens client-side. The template must be
	 * valid JSON (tree-level substitution); the daemon only ever sees the
	 * already-expanded result. Without defines, the input is sent verbatim. */
	if (subst_count() > 0) {
		cJSON *root = cJSON_Parse(json_str);
		if (!root) {
			fprintf(stderr, "Error: input is not valid JSON "
			        "(required when using -D substitution)\n");
			if (needs_free)
				free(json_str);
			return 1;
		}
		json_substitute(root);
		char *expanded = cJSON_PrintUnformatted(root);
		cJSON_Delete(root);
		if (!expanded) {
			fprintf(stderr, "Out of memory\n");
			if (needs_free)
				free(json_str);
			return 1;
		}
		if (needs_free)
			free(json_str);
		json_str   = expanded;
		needs_free = 1;
	}

	/* A bare {"cmd":"running"} is answered locally so it always yields a
	 * boolean, never a connection error, even when the daemon is down. */
	if (is_running_query(json_str)) {
		if (needs_free)
			free(json_str);
		return report_running(1);	/* {"running":true|false} */
	}

	/* Cheap sanity check before bothering the daemon. */
	if (json_str[0] != '{' && json_str[0] != '[') {
		fprintf(stderr,
		        "Error: argument does not look like JSON "
		        "(must start with { or [)\n");
		if (needs_free)
			free(json_str);
		return 1;
	}

	int ret = send_json(json_str);
	if (needs_free)
		free(json_str);
	return ret;
}
