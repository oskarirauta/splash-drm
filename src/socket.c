/*
 * socket.c - Abstract UNIX socket server.
 *
 * The daemon listens on an abstract-namespace UNIX socket (no filesystem
 * entry, so nothing to clean up and it works before rootfs is mounted).
 * Clients send newline-terminated JSON commands and receive a one-line
 * JSON reply. Up to MAX_SOCKET_CLIENTS connections are served at once.
 */

#include "splash.h"

/* ========================================================================
 * Socket Initialisation
 * ======================================================================== */

int socket_init(splash_state_t *st) {
	struct sockaddr_un addr;

	st->server_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (st->server_fd < 0) {
		LOGE("socket: %s", strerror(errno));
		return -1;
	}

	/* Abstract socket: sun_path[0] == '\0', name follows in the rest. */
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	addr.sun_path[0] = '\0';
	strncpy(addr.sun_path + 1, SOCKET_NAME + 1, sizeof(addr.sun_path) - 2);

	if (bind(st->server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		/* An abstract-namespace name has no filesystem entry and is reaped by
		 * the kernel the instant the last holder's fd closes, so it can never
		 * be left "stale" by a crashed daemon. EADDRINUSE therefore means a
		 * live instance already holds it - bail rather than fight over it. */
		if (errno == EADDRINUSE)
			LOGE("another splash-drm instance is already running");
		else
			LOGE("bind: %s", strerror(errno));
		close(st->server_fd);
		st->server_fd = -1;
		return -1;
	}

	if (listen(st->server_fd, MAX_SOCKET_CLIENTS) < 0) {
		LOGE("listen: %s", strerror(errno));
		close(st->server_fd);
		st->server_fd = -1;
		return -1;
	}

	for (int i = 0; i < MAX_SOCKET_CLIENTS; i++) {
		st->client_fds[i]     = -1;
		st->client_cmd_len[i] = 0;
	}

	LOGD("control socket listening (abstract namespace)");
	return 0;
}

void socket_cleanup(splash_state_t *st) {
	for (int i = 0; i < MAX_SOCKET_CLIENTS; i++) {
		if (st->client_fds[i] >= 0) {
			close(st->client_fds[i]);
			st->client_fds[i] = -1;
		}
	}
	if (st->server_fd >= 0) {
		close(st->server_fd);
		st->server_fd = -1;
	}
}

/* ========================================================================
 * Poll Setup
 *
 * Builds the pollfd array: the listening socket first, then one entry
 * per connected client. `fds` must hold 1 + MAX_SOCKET_CLIENTS entries.
 * ======================================================================== */

int socket_poll(splash_state_t *st, struct pollfd *fds, int *nfds) {
	int count = 0;

	fds[count].fd     = st->server_fd;
	fds[count].events = POLLIN;
	count++;

	for (int i = 0; i < MAX_SOCKET_CLIENTS; i++) {
		if (st->client_fds[i] >= 0) {
			fds[count].fd     = st->client_fds[i];
			fds[count].events = POLLIN;
			count++;
		}
	}

	*nfds = count;
	return count;
}

/* ========================================================================
 * Client Management
 * ======================================================================== */

/* Accept a pending connection into the first free client slot. */
static int accept_client(splash_state_t *st) {
	int fd = accept4(st->server_fd, NULL, NULL, SOCK_NONBLOCK);
	if (fd < 0)
		return -1;

	for (int i = 0; i < MAX_SOCKET_CLIENTS; i++) {
		if (st->client_fds[i] < 0) {
			st->client_fds[i]     = fd;
			st->client_cmd_len[i] = 0;
			LOGD("client connected, slot %d (fd=%d)", i, fd);
			return i;
		}
	}

	/* No free slot: drop the connection. */
	LOGD("client rejected: no slots");
	close(fd);
	return -1;
}

static void close_client(splash_state_t *st, int idx) {
	if (idx < 0 || idx >= MAX_SOCKET_CLIENTS)
		return;

	if (st->client_fds[idx] >= 0) {
		LOGD("client disconnected, slot %d (fd=%d)", idx, st->client_fds[idx]);
		close(st->client_fds[idx]);
		st->client_fds[idx]     = -1;
		st->client_cmd_len[idx] = 0;
	}
}

static int find_client_idx(splash_state_t *st, int fd) {
	for (int i = 0; i < MAX_SOCKET_CLIENTS; i++) {
		if (st->client_fds[i] == fd)
			return i;
	}
	return -1;
}

/* ========================================================================
 * JSON Reply to Client
 * ======================================================================== */

/* Serialise `response` and write it to the client as one '\n'-terminated
 * line. Best-effort: a failed write is ignored (the client may be gone). */
void socket_reply_json(splash_state_t *st, int client_idx, cJSON *response) {
	if (client_idx < 0 || client_idx >= MAX_SOCKET_CLIENTS)
		return;
	if (st->client_fds[client_idx] < 0)
		return;

	char *str = cJSON_PrintUnformatted(response);
	if (!str)
		return;

	size_t len = strlen(str);
	char *buf = malloc(len + 2);
	if (!buf) {
		free(str);
		return;
	}

	memcpy(buf, str, len);
	buf[len]     = '\n';
	buf[len + 1] = '\0';

	ssize_t written = write(st->client_fds[client_idx], buf, len + 1);

	free(buf);
	free(str);

	if (written != (ssize_t)(len + 1)) {
		/* Partial or failed reply on the non-blocking fd: the client would
		 * block forever waiting for the missing newline, so drop it to fail
		 * fast rather than leave a half-line dangling. */
		LOGD("reply to client %d incomplete (%zd/%zu), closing",
		     client_idx, written, len + 1);
		close_client(st, client_idx);
		return;
	}

	LOGD("reply to client %d sent", client_idx);
}

/* ========================================================================
 * Process Socket Events
 * ======================================================================== */

/*
 * Handle every fd flagged by poll(): accept new connections on the
 * listening socket, and on client sockets read bytes into the client's
 * line buffer, dispatching a command on each newline.
 */
void socket_process(splash_state_t *st, struct pollfd *fds, int nfds) {
	for (int i = 0; i < nfds; i++) {
		if (fds[i].fd == st->server_fd && (fds[i].revents & POLLIN)) {
			accept_client(st);
			continue;
		}

		int idx = find_client_idx(st, fds[i].fd);
		if (idx < 0)
			continue;

		if (fds[i].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			close_client(st, idx);
			continue;
		}

		if (!(fds[i].revents & POLLIN))
			continue;

		char buf[256];
		ssize_t n = read(st->client_fds[idx], buf, sizeof(buf) - 1);
		if (n <= 0) {
			close_client(st, idx);
			continue;
		}

		for (ssize_t j = 0; j < n; j++) {
			/* Over-long command with no newline: drop and resync. */
			if (st->client_cmd_len[idx] >= CMD_MAX_LEN - 1)
				st->client_cmd_len[idx] = 0;

			st->client_cmd_buf[idx][st->client_cmd_len[idx]++] = buf[j];

			if (buf[j] == '\n') {
				st->client_cmd_buf[idx][st->client_cmd_len[idx]] = '\0';

				/* len includes the trailing '\n'; drop it from the log. */
				LOGD("JSON from client %d: %.*s", idx,
				     (int)st->client_cmd_len[idx] - 1,
				     st->client_cmd_buf[idx]);

				handle_json_command(st, st->client_cmd_buf[idx], idx);
				st->client_cmd_len[idx] = 0;
			}
		}
	}
}
