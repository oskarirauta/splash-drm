/*
 * log.c - Implementation of the unified logging sinks.
 *
 * See log.h for the level/sink model. Three sinks are supported:
 *
 *   stderr  plain human-readable lines, for foreground/ssh use.
 *   syslog  RFC3164-ish datagrams to the /dev/log AF_UNIX socket; the local
 *           syslog daemon (busybox syslogd, OpenWrt logd, systemd-journald, ...)
 *           timestamps and stores them. We write the socket directly rather
 *           than via libc syslog() so the fallback to kmsg is under our control.
 *   kmsg    one line per message to /dev/kmsg (the kernel log). Survives
 *           switch_root, which is why it is the early-boot fallback.
 *
 * The sink is resolved once (see log.h: no reconnect by design). A forced sink
 * still degrades if its device is missing, so logging never silently vanishes
 * when a channel is simply unavailable on the target.
 */

#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>

/* syslog facility/severity codes, hardcoded so we need not pull in <syslog.h>
 * (whose LOG_* names would also collide with this module's). */
#define SYSLOG_FACILITY_DAEMON  (3 << 3)   /* LOG_DAEMON */

log_level_t log_threshold = LOG_ERROR;

static log_sink_t g_requested = LOG_SINK_AUTO;   /* what --log asked for */
static log_sink_t g_active    = LOG_SINK_STDERR; /* what we resolved to */
static int        g_syslog_fd = -1;
static int        g_kmsg_fd   = -1;

/* Map a level onto a syslog severity (0..7); used for both the syslog PRI and
 * the kmsg level prefix. INFO maps to NOTICE so routine lifecycle lines are not
 * buried at the same priority as application "info" spam. */
static int level_severity(log_level_t level) {
	switch (level) {
	case LOG_ERROR: return 3;   /* err */
	case LOG_WARN:  return 4;   /* warning */
	case LOG_INFO:  return 5;   /* notice */
	case LOG_DEBUG: return 7;   /* debug */
	case LOG_TRACE: return 7;   /* debug */
	default:        return 5;
	}
}

/* Human-readable prefix for the stderr sink. DEBUG keeps the historical
 * "[debug] " tag so existing log-greppers keep working. */
static const char *level_prefix(log_level_t level) {
	switch (level) {
	case LOG_ERROR: return "splash-drm: error: ";
	case LOG_WARN:  return "splash-drm: warning: ";
	case LOG_INFO:  return "splash-drm: ";
	case LOG_DEBUG: return "[debug] ";
	case LOG_TRACE: return "[trace] ";
	default:        return "splash-drm: ";
	}
}

/* Connect a datagram (falling back to stream) socket to /dev/log.
 * Returns 0 and stores the fd in g_syslog_fd on success, -1 otherwise. */
static int open_syslog(void) {
	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family = AF_UNIX;
	strncpy(addr.sun_path, "/dev/log", sizeof(addr.sun_path) - 1);

	int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
	if (fd < 0)
		return -1;

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		/* Some syslog daemons expose /dev/log as a stream socket. */
		if (errno == EPROTOTYPE) {
			close(fd);
			fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
			if (fd < 0)
				return -1;
			if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
				close(fd);
				return -1;
			}
		} else {
			close(fd);
			return -1;
		}
	}

	g_syslog_fd = fd;
	return 0;
}

static int open_kmsg(void) {
	int fd = open("/dev/kmsg", O_WRONLY | O_CLOEXEC);
	if (fd < 0)
		return -1;
	g_kmsg_fd = fd;
	return 0;
}

void log_close(void) {
	if (g_syslog_fd >= 0) {
		close(g_syslog_fd);
		g_syslog_fd = -1;
	}
	if (g_kmsg_fd >= 0) {
		close(g_kmsg_fd);
		g_kmsg_fd = -1;
	}
}

/* Resolve g_active from the requested sink and the process state. */
static void resolve_sink(int forked) {
	log_close();

	switch (g_requested) {
	case LOG_SINK_STDERR:
		g_active = LOG_SINK_STDERR;
		return;

	case LOG_SINK_SYSLOG:
		/* Forced syslog: still degrade rather than drop everything. */
		if (open_syslog() == 0)        g_active = LOG_SINK_SYSLOG;
		else if (open_kmsg() == 0)     g_active = LOG_SINK_KMSG;
		else                           g_active = LOG_SINK_STDERR;
		return;

	case LOG_SINK_KMSG:
		if (open_kmsg() == 0)          g_active = LOG_SINK_KMSG;
		else                           g_active = LOG_SINK_STDERR;
		return;

	case LOG_SINK_AUTO:
	default:
		/* Foreground keeps stderr (visible on ssh/console). A forked daemon
		 * has lost stdio to /dev/null, so prefer syslog, then kmsg. */
		if (!forked) {
			g_active = LOG_SINK_STDERR;
		} else if (open_syslog() == 0) {
			g_active = LOG_SINK_SYSLOG;
		} else if (open_kmsg() == 0) {
			g_active = LOG_SINK_KMSG;
		} else {
			g_active = LOG_SINK_STDERR;   /* == /dev/null after daemonize */
		}
		return;
	}
}

log_sink_t log_sink_from_string(const char *name, int *ok) {
	if (ok)
		*ok = 1;
	if (name) {
		if (strcmp(name, "auto")   == 0) return LOG_SINK_AUTO;
		if (strcmp(name, "stderr") == 0) return LOG_SINK_STDERR;
		if (strcmp(name, "syslog") == 0) return LOG_SINK_SYSLOG;
		if (strcmp(name, "kmsg")   == 0) return LOG_SINK_KMSG;
	}
	if (ok)
		*ok = 0;
	return LOG_SINK_AUTO;
}

void log_init(log_level_t level, log_sink_t sink) {
	log_threshold = level;
	g_requested   = sink;
	resolve_sink(0);
}

void log_reopen(int forked) {
	resolve_sink(forked);
}

void log_set_level(log_level_t level) {
	log_threshold = level;
}

static void emit_syslog(log_level_t level, const char *msg) {
	int pri = SYSLOG_FACILITY_DAEMON + level_severity(level);
	char line[1100];
	int n = snprintf(line, sizeof(line), "<%d>splash-drm[%d]: %s",
	                 pri, (int)getpid(), msg);
	if (n < 0)
		return;
	if (n > (int)sizeof(line))
		n = (int)sizeof(line);
	/* Best-effort: the sink is locked, so a dropped datagram is just lost. */
	(void)send(g_syslog_fd, line, (size_t)n, MSG_NOSIGNAL);
}

static void emit_kmsg(log_level_t level, const char *msg) {
	int pri = SYSLOG_FACILITY_DAEMON + level_severity(level);
	dprintf(g_kmsg_fd, "<%d>splash-drm: %s\n", pri, msg);
}

static void emit_stderr(log_level_t level, const char *msg) {
	fprintf(stderr, "%s%s\n", level_prefix(level), msg);
}

void log_write(log_level_t level, const char *fmt, ...) {
	if (level > log_threshold)
		return;

	char msg[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);

	switch (g_active) {
	case LOG_SINK_SYSLOG:
		if (g_syslog_fd >= 0) { emit_syslog(level, msg); return; }
		break;
	case LOG_SINK_KMSG:
		if (g_kmsg_fd >= 0) { emit_kmsg(level, msg); return; }
		break;
	default:
		break;
	}
	emit_stderr(level, msg);
}
