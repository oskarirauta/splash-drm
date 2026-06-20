/*
 * log.h - Unified logging for the splash-drm daemon.
 *
 * A single global severity threshold and output sink, configured once at
 * startup, so any source file can log without threading splash_state_t
 * around. The control client (splash-ctl) pulls in this header via splash.h
 * but never calls these functions, so it carries no link dependency on log.c.
 *
 * Levels form an ordered ladder; a message is emitted when its level is at
 * or below the active threshold:
 *
 *     ERROR  serious / fatal failures        - the floor of anything visible
 *     WARN   recoverable problems            - e.g. a font that failed to load
 *     INFO   normal lifecycle notes          - resolution, clean exit
 *     DEBUG  per-command / per-client detail - the old [debug] lines
 *     TRACE  dense boot bring-up tracing      - loop enter/exit, switch_root
 *
 * The default threshold is ERROR: silent but for genuine failures. -v/-vv/-vvv
 * raise it to INFO/DEBUG/TRACE; -q drops below ERROR for total silence.
 *
 * Sinks (log_sink_t): a foreground run defaults to stderr, where ssh/console
 * users see it live. A forked daemon loses its stdio to /dev/null, so AUTO
 * falls back to the syslog socket (/dev/log) and then the kernel log
 * (/dev/kmsg). Any sink can also be forced with --log, independent of forking.
 * Nothing is Linux- or OpenWrt-specific beyond /dev/kmsg, which is only ever
 * reached on Linux where it exists.
 */
#ifndef SPLASH_LOG_H
#define SPLASH_LOG_H

typedef enum {
	LOG_SILENT = -1,   /* threshold only: never matches a real message */
	LOG_ERROR  = 0,
	LOG_WARN,
	LOG_INFO,
	LOG_DEBUG,
	LOG_TRACE,
} log_level_t;

typedef enum {
	LOG_SINK_AUTO = 0, /* foreground: stderr; forked: syslog -> kmsg -> none */
	LOG_SINK_STDERR,
	LOG_SINK_SYSLOG,
	LOG_SINK_KMSG,
} log_sink_t;

/*
 * Current threshold. Exposed only so the LOG_* macros can skip argument
 * evaluation and formatting for a message that would be discarded; assign it
 * through log_init()/log_set_level(), never directly.
 */
extern log_level_t log_threshold;

/* Map a --log argument ("auto"/"stderr"/"syslog"/"kmsg") to a sink. On an
 * unknown name returns LOG_SINK_AUTO and sets *ok to 0 (otherwise 1). */
log_sink_t log_sink_from_string(const char *name, int *ok);

/* Set the threshold and the requested sink, then resolve the sink as if still
 * in the foreground. Call once after parsing arguments. */
void log_init(log_level_t level, log_sink_t sink);

/* Re-resolve an AUTO sink for the current process state: pass forked != 0 from
 * the daemon child after daemonize() has redirected stdio to /dev/null, so the
 * sink can move off the now-dead stderr onto syslog/kmsg. A forced sink is left
 * untouched. */
void log_reopen(int forked);

void log_set_level(log_level_t level);

/* Close any sink fds (syslog/kmsg). Idempotent. */
void log_close(void);

void log_write(log_level_t level, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

#define LOG_AT(lvl, ...) \
	do { if ((lvl) <= log_threshold) log_write((lvl), __VA_ARGS__); } while (0)

#define LOGE(...) LOG_AT(LOG_ERROR, __VA_ARGS__)
#define LOGW(...) LOG_AT(LOG_WARN,  __VA_ARGS__)
#define LOGI(...) LOG_AT(LOG_INFO,  __VA_ARGS__)
#define LOGD(...) LOG_AT(LOG_DEBUG, __VA_ARGS__)
#define LOGT(...) LOG_AT(LOG_TRACE, __VA_ARGS__)

#endif /* SPLASH_LOG_H */
