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

static int send_json(const char *json_str) {
	if (debug_mode)
		fprintf(stderr, "[debug] Sending: %s\n", json_str);

	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
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
		fprintf(stderr, "Cannot connect to splash-drm daemon: %s\n",
		        strerror(errno));
		close(fd);
		return -1;
	}

	size_t len = strlen(json_str);
	if (write(fd, json_str, len) != (ssize_t)len ||
	    write(fd, "\n", 1) != 1) {
		fprintf(stderr, "Failed to write command\n");
		close(fd);
		return -1;
	}

	/*
	 * Read the reply. A single read() can return a partial line, so loop
	 * until a newline arrives, the buffer fills, or the peer closes.
	 */
	char reply[4096];
	size_t total = 0;
	while (total < sizeof(reply) - 1) {
		ssize_t n = read(fd, reply + total, sizeof(reply) - 1 - total);
		if (n <= 0)
			break;
		total += (size_t)n;
		if (memchr(reply, '\n', total))
			break;
	}
	reply[total] = '\0';
	close(fd);

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
		"splash-ctl v3.0 - JSON control client for splash-drm\n\n"
		"Usage: %s [options] <json-string>\n"
		"       %s [options] --file <json-file>\n\n"
		"Options:\n"
		"  --raw      Output the raw JSON response\n"
		"  --file     Read JSON from a file instead of the argument\n\n"
		"  --help     Show this help and exit\n"
		"  --version  Show version and exit\n"
		"  --debug    Show debug output\n\n"
		"JSON format (single command):\n"
		"  {\"cmd\": \"text\", \"id\": 0, \"x\": 100, \"y\": 200, \"text\": \"Hello\"}\n\n"
		"JSON format (batch):\n"
		"  [{\"cmd\": \"clear\"}, {\"cmd\": \"image\", \"path\": \"splash.png\"}]\n\n"
		"Available commands:\n"
		"  Display:   clear, bg_color, image, text, remove_text, rect,\n"
		"             remove_rect, overlay, remove_overlay, progress,\n"
		"             update_progress, hide_progress\n"
		"  Animation: animate, spinner\n"
		"  Control:   suspend, resume, status, ready, exit\n\n"
		"Examples:\n"
		"  %s '{\"cmd\":\"image\",\"path\":\"boot.png\",\"crossfade\":600}'\n"
		"  %s '{\"cmd\":\"spinner\",\"id\":0,\"radius\":40}'\n"
		"  %s '{\"cmd\":\"progress\",\"id\":0,\"value\":0.5}'\n"
		"  %s '{\"cmd\":\"animate\",\"type\":\"text\",\"id\":0,\"to\":0,\"duration\":400}'\n"
		"  %s --file /etc/splash-commands.json\n"
		"  %s '{\"cmd\":\"status\"}'\n"
		"  %s '{\"cmd\":\"exit\"}'\n",
		prog, prog, prog, prog, prog, prog, prog, prog, prog);
}

/* ========================================================================
 * Entry Point
 * ======================================================================== */

int main(int argc, char **argv) {
	int arg_offset = 1;
	int use_file   = 0;

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
			print_usage(argv[0]);
			return 0;
		} else if (strcmp(argv[arg_offset], "--version") == 0 ||
			   strcmp(argv[arg_offset], "-v") == 0) {
			printf("splash-ctl v%s\n", SPLASH_VERSION);
			return 0;
		} else {
			fprintf(stderr, "Unknown option: %s\n", argv[arg_offset]);
			return 1;
		}
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
