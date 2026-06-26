/*
 * ubus-progress.c — OpenWrt procd → splash-drm boot-progress bridge.
 *
 * Optional companion to splash-drm for OpenWrt. It subscribes to procd's
 * "service" ubus object and, for every service that starts during boot, drives
 * the splash: it advances an N-of-total progress value and shows the service
 * name by filling UCI-configured JSON templates and sending them straight to
 * the splash-drm control socket. When boot completes it runs the finished
 * sequence; if boot instead stalls (no service activity for idle_timeout
 * seconds) it runs the fail sequence. Either way the bridge then exits.
 *
 * It is configured entirely through UCI (/etc/config/splash), so nothing about
 * the splash layout is hard-coded — the templates name the element ids, types
 * and colours. An empty template/list means "skip".
 *
 *   config global 'global'
 *       option enabled      '1'
 *       option resume       '1'      # resume the initramfs-suspended splash
 *       option total        'auto'   # 'auto' = count procd-service S## scripts
 *       option done_event   'boot.done'  # optional failsafe: service event type
 *       option done_service 'done'   # service.start name that means "boot done"
 *       option idle_timeout '60'     # stall window (s) while progress is low
 *       option idle_action  'finished'   # finished (settled) | fail (stalled)
 *       option done_at_pct  '90'     # >= this % + quiet = boot done, not a stall
 *       option settle_timeout '5'    # quiet window (s) once near-complete
 *
 *   config start 'start'             # run once when the bridge comes up:
 *       list   start_msg    '{"text":{"id":1,"remove":true}}'   # drop "Preparing…"
 *       list   start_msg    '{"progress":{"id":0,"y":"80%","value":"0"}}'
 *
 *   config progress 'progress'       # per-tick templates, all sent every tick.
 *       # Macros: ${value} (0..1), ${percent} (0..100 int), ${count}/${total}
 *       # (N of M), ${service} (name). Each entry may be one object or a JSON
 *       # array of them (one batch).
 *       list   status_msg   '{"progress":{"id":0,"value":"${value}"}}'
 *       list   status_msg   '{"console":{"id":0,"write":"starting ${service}"}}'
 *
 *   config finished 'finished'       # boot completed normally
 *       list   done_msg     '{"text":{"id":99,"text":"System ready!"}}'
 *       option delay        '0'      # hold the 100% frame before done_msg (debug)
 *       option hold         '2'      # pause (s) after done_msg, before exit
 *       option exit_action  'fade'   # none | exit | fade
 *       option exit_timeout '1'
 *       option exit_color   '#000000'
 *
 *   config fail 'fail'               # boot stalled (idle_timeout). ${idle}=Ns
 *       list   fail_msg     '{"system":{"action":"clear","color":"#2a0000"}}'
 *       list   fail_msg     '{"text":{"id":0,"text":"Boot stalled"}}'
 *       option hold         '15'     # show the fail screen at least this long
 *       option exit_action  'none'   # none keeps the screen up; exit/fade quits
 *       option exit_timeout '0'
 *       option exit_color   '#2a0000'
 *
 * Build: `make ubus-progress` (links libubus, libubox, libuci, and reuses the
 * project's cJSON + subst for ${NAME} expansion). Run it early in boot.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glob.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <libubus.h>
#include <libubox/uloop.h>
#include <libubox/blobmsg.h>
#include <uci.h>

#include "cJSON/cJSON.h"
#include "version.h"		/* SPLASH_VERSION — the bridge tracks the daemon's */

/* subst.c (reused from the splash-drm build): ${NAME} expansion engine. */
extern int  subst_define(const char *spec);   /* "NAME=value" */
extern void json_substitute(cJSON *root);

#define SPLASH_SOCKET "splash-drm"             /* abstract socket name */

/* ---- configuration (loaded once from UCI) ---- */
static struct {
	/* [global] */
	int    enabled;
	int    resume;         /* resume the (initramfs-suspended) splash on start */
	int    total;
	int    idle_timeout;   /* run idle_action if no service.start for this long */
	char  *idle_action;    /* "finished" (boot settled) | "fail" (stalled) */
	int    done_at_pct;    /* progress % that counts as "boot reached the end" */
	int    settle_timeout; /* quiet seconds, once near-complete, to call it done */
	char  *done_service;   /* service.start name that means "boot done" */
	char  *done_event;     /* ubus "service event" type that means "boot done" */
	char  *self_script;    /* our own /etc/init.d name, excluded from the total */
	/* [start] — one-time sequence run when the bridge comes up */
	char **start_msg;
	int    start_msg_n;
	/* [progress] — one `list status_msg` of templates, all sent every tick */
	char **status_msg;
	int    status_msg_n;
	/* [finished] — boot completed normally */
	char **done_msg;
	int    done_msg_n;
	int    fin_delay;      /* seconds to hold the 100% frame before the done seq */
	int    hold;           /* seconds to hold the finished frame before exit */
	char  *fin_action;     /* none | exit | fade */
	int    fin_timeout;
	char  *fin_color;
	/* [fail] — boot stalled */
	char **fail_msg;
	int    fail_msg_n;
	int    fail_hold;      /* seconds to show the fail screen before exit */
	char  *fail_action;
	int    fail_timeout;
	char  *fail_color;
} cfg;

static int                    g_count = 0;
static int                    g_done  = 0;
static char                  *g_exit_json = NULL;   /* sent after the hold */
static struct ubus_subscriber g_sub;
static struct uloop_timeout   g_hold_timer;
static struct uloop_timeout   g_idle_timer;
static struct uloop_timeout   g_finish_timer;   /* delays the done sequence */

/* ---- splash-drm control socket ---- */

/* Send one JSON line to the splash-drm daemon. Fire-and-forget: failure (e.g.
 * the daemon is not running) is ignored, like the shell helpers. */
static void splash_send(const char *json)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0)
		return;

	struct sockaddr_un addr;
	memset(&addr, 0, sizeof(addr));
	addr.sun_family  = AF_UNIX;
	addr.sun_path[0] = '\0';			/* abstract: leading NUL */
	strncpy(addr.sun_path + 1, SPLASH_SOCKET, sizeof(addr.sun_path) - 2);

	if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
		size_t len = strlen(json);
		if (write(fd, json, len) == (ssize_t)len &&
		    write(fd, "\n", 1) == 1) {
			/* Read the reply before closing: closing immediately can race
			 * the daemon's poll, which may see the hang-up before it reads
			 * the line and drop the command. */
			char reply[256];
			(void)!read(fd, reply, sizeof(reply));
		}
	}
	close(fd);
}

/* Fill a UCI template with the current ${...} defines and send it. An empty/NULL
 * template is skipped; invalid JSON is skipped with a warning. */
static void send_template(const char *tmpl)
{
	if (!tmpl || !*tmpl)
		return;

	cJSON *root = cJSON_Parse(tmpl);
	if (!root) {
		fprintf(stderr, "ubus-progress: invalid template JSON: %s\n", tmpl);
		return;
	}
	json_substitute(root);

	char *line = cJSON_PrintUnformatted(root);
	cJSON_Delete(root);
	if (line) {
		splash_send(line);
		free(line);
	}
}

/* Define the progress macros for a tick: ${value} (0..1 fraction), ${percent}
 * (0..100 integer), ${count} (services advanced so far) and ${total} (the
 * denominator). Templates pick whichever they need. Integer percent avoids a
 * libm dependency. */
static void define_progress(double value, int count, int total)
{
	char buf[48];
	snprintf(buf, sizeof(buf), "value=%.4f", value);
	subst_define(buf);
	snprintf(buf, sizeof(buf), "percent=%d", (int)(value * 100.0 + 0.5));
	subst_define(buf);
	snprintf(buf, sizeof(buf), "count=%d", count);
	subst_define(buf);
	snprintf(buf, sizeof(buf), "total=%d", total);
	subst_define(buf);
}

/* ---- finish / fail (shared sequence) ---- */

/* Build the exit message for an action, or NULL for "none". */
static char *build_exit(const char *action, int timeout, const char *color)
{
	static char buf[160];
	if (!action || strcmp(action, "none") == 0)
		return NULL;
	if (strcmp(action, "fade") == 0)
		snprintf(buf, sizeof(buf),
		         "{\"system\":{\"action\":\"exit\",\"timeout\":%d,\"fade\":\"%s\"}}",
		         timeout, color ? color : "#000000");
	else	/* "exit" */
		snprintf(buf, sizeof(buf),
		         "{\"system\":{\"action\":\"exit\",\"timeout\":%d}}", timeout);
	return buf;
}

/* Second phase: after the hold, run the exit action and quit the bridge. */
static void do_exit(struct uloop_timeout *t)
{
	(void)t;
	if (g_exit_json)
		splash_send(g_exit_json);
	uloop_end();
}

/* Send every per-tick template (the `list status_msg`) with the current macros.
 * Used on each tick and to fill the bar to 100% at finish. */
static void send_status_list(void)
{
	for (int i = 0; i < cfg.status_msg_n; i++)
		send_template(cfg.status_msg[i]);
}

/* Send a message list in order, then schedule the exit after `hold` seconds.
 * The g_done re-entry guard lives in the finish()/fail() callers, so this runs
 * exactly once per outcome even when finish() defers it past a delay. */
static void run_sequence(char **msgs, int n, int hold,
                         const char *action, int timeout, const char *color)
{
	for (int i = 0; i < n; i++)
		send_template(msgs[i]);

	g_exit_json = build_exit(action, timeout, color);
	g_hold_timer.cb = do_exit;
	uloop_timeout_set(&g_hold_timer, hold > 0 ? hold * 1000 : 0);
}

/* The finished (success) done sequence, possibly deferred by `fin_delay`. */
static void run_finished(struct uloop_timeout *t)
{
	(void)t;
	run_sequence(cfg.done_msg, cfg.done_msg_n, cfg.hold,
	             cfg.fin_action, cfg.fin_timeout, cfg.fin_color);
}

static void finish(void)
{
	if (g_done)
		return;
	g_done = 1;                          /* claim completion now: blocks fail() */
	uloop_timeout_cancel(&g_idle_timer);

	/* Fill the bar to 100% first, so it always completes even when the count
	 * never reached the (approximate) total. The macros read 100% / count==total
	 * so a ${percent} readout lands on "100". */
	define_progress(1.0, cfg.total, cfg.total);
	send_status_list();

	/* fin_delay holds the filled 100% frame before the done sequence runs —
	 * mainly a debug aid to confirm the bar really reached the end. */
	if (cfg.fin_delay > 0) {
		g_finish_timer.cb = run_finished;
		uloop_timeout_set(&g_finish_timer, cfg.fin_delay * 1000);
	} else {
		run_finished(NULL);
	}
}

static void fail(void)
{
	if (g_done)
		return;
	g_done = 1;
	uloop_timeout_cancel(&g_idle_timer);

	char b[48];
	snprintf(b, sizeof(b), "idle=%d", cfg.idle_timeout);
	subst_define(b);
	run_sequence(cfg.fail_msg, cfg.fail_msg_n, cfg.fail_hold,
	             cfg.fail_action, cfg.fail_timeout, cfg.fail_color);
}

/* Have we advanced far enough that a quiet period means "boot reached the end"
 * rather than "boot stalled"? The auto total tends to over-count by a service or
 * two (init scripts that look like procd services but never fire an observed
 * service.start), so the count often plateaus just short of total and the exact
 * count==total finish never fires. Treating >= done_at_pct as effectively done
 * closes that gap without a done_event/rc.local hook. */
static int near_complete(void)
{
	return cfg.total > 0 &&
	       (long)g_count * 100 >= (long)cfg.total * cfg.done_at_pct;
}

/* (Re)arm the idle/settle watchdog. Near the end we use the short settle_timeout
 * so a normal boot finishes promptly once it goes quiet; earlier on we wait the
 * full idle_timeout, so a genuine early stall still gets the patient treatment. */
static void arm_idle(void)
{
	int secs = near_complete() ? cfg.settle_timeout : cfg.idle_timeout;
	if (secs > 0)
		uloop_timeout_set(&g_idle_timer, secs * 1000);
	else
		uloop_timeout_cancel(&g_idle_timer);
}

/* Idle/settle watchdog fired: no service.start within the window. If we are
 * essentially at the end, the quiet means boot settled — finish, regardless of
 * idle_action, since the (approximate) count never reached total exactly. Only a
 * quiet period while progress is still low is treated as a real stall. */
static void on_idle(struct uloop_timeout *t)
{
	(void)t;
	if (near_complete() ||
	    !(cfg.idle_action && strcmp(cfg.idle_action, "fail") == 0))
		finish();
	else
		fail();
}

/* ---- procd service notifications ---- */

enum { SVC_NAME, __SVC_MAX };
static const struct blobmsg_policy svc_policy[__SVC_MAX] = {
	[SVC_NAME] = { .name = "service", .type = BLOBMSG_TYPE_STRING },
};

enum { EV_TYPE, __EV_MAX };
static const struct blobmsg_policy ev_policy[__EV_MAX] = {
	[EV_TYPE] = { .name = "type", .type = BLOBMSG_TYPE_STRING },
};

/* procd notifies subscribers with method "service.start" / "service.stop"
 * carrying {"service":"<name>"}. We advance on each start. */
static int notify_cb(struct ubus_context *ctx, struct ubus_object *obj,
                     struct ubus_request_data *req, const char *method,
                     struct blob_attr *msg)
{
	(void)ctx; (void)obj; (void)req;

	if (g_done)
		return 0;

	/* An explicit "boot done" event (e.g. fired from /etc/rc.local with
	 * `ubus call service event '{"type":"boot.done"}'`) is the reliable
	 * completion signal, since procd has no boot-complete notification. */
	if (cfg.done_event && *cfg.done_event &&
	    strcmp(method, "event.trigger") == 0) {
		struct blob_attr *et[__EV_MAX];
		blobmsg_parse(ev_policy, __EV_MAX, et, blob_data(msg), blob_len(msg));
		const char *type = et[EV_TYPE] ? blobmsg_get_string(et[EV_TYPE]) : "";
		if (strcmp(type, cfg.done_event) == 0)
			finish();
		return 0;
	}

	if (strcmp(method, "service.start") != 0)
		return 0;

	struct blob_attr *tb[__SVC_MAX];
	blobmsg_parse(svc_policy, __SVC_MAX, tb, blob_data(msg), blob_len(msg));
	const char *name = tb[SVC_NAME] ? blobmsg_get_string(tb[SVC_NAME]) : "?";

	/* Ignore our own service.start: as a procd service the bridge fires one for
	 * itself, and it is excluded from the total, so it must not advance either.
	 * (Normally missed — we subscribe after procd launched us — but be safe.) */
	if (cfg.self_script && *cfg.self_script &&
	    strcmp(name, cfg.self_script) == 0)
		return 0;

	/* The boot-complete service is the explicit "done" signal. */
	if (cfg.done_service && *cfg.done_service &&
	    strcmp(name, cfg.done_service) == 0) {
		finish();
		return 0;
	}

	if (g_count < cfg.total)
		g_count++;

	/* Real activity: re-arm the watchdog. Done after the count bump so the
	 * window shortens to settle_timeout as soon as we cross into near-complete. */
	arm_idle();

	double value = cfg.total > 0 ? (double)g_count / (double)cfg.total : 1.0;
	if (value > 1.0)
		value = 1.0;

	define_progress(value, g_count, cfg.total);

	char sbuf[160];
	snprintf(sbuf, sizeof(sbuf), "service=%s", name);
	subst_define(sbuf);

	send_status_list();

	/* Fallback if the done service never fires. */
	if (g_count >= cfg.total)
		finish();

	return 0;
}

/* ---- UCI configuration ---- */

static char *uci_str(struct uci_context *uci, const char *path)
{
	char buf[256];
	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	struct uci_ptr ptr;
	if (uci_lookup_ptr(uci, &ptr, buf, true) != UCI_OK)
		return NULL;
	if (!(ptr.flags & UCI_LOOKUP_COMPLETE) || !ptr.o ||
	    ptr.o->type != UCI_TYPE_STRING)
		return NULL;
	return strdup(ptr.o->v.string);
}

/* Read a list option into a malloc'd array of strings. Accepts a `list X 'v'`
 * (multiple) or a single `option X 'v'`. Returns the count. */
static int uci_list(struct uci_context *uci, const char *path, char ***out)
{
	*out = NULL;

	char buf[256];
	strncpy(buf, path, sizeof(buf) - 1);
	buf[sizeof(buf) - 1] = '\0';

	struct uci_ptr ptr;
	if (uci_lookup_ptr(uci, &ptr, buf, true) != UCI_OK)
		return 0;
	if (!(ptr.flags & UCI_LOOKUP_COMPLETE) || !ptr.o)
		return 0;

	if (ptr.o->type == UCI_TYPE_STRING) {
		char **arr = malloc(sizeof(char *));
		if (!arr)
			return 0;
		arr[0] = strdup(ptr.o->v.string);
		*out = arr;
		return 1;
	}

	if (ptr.o->type == UCI_TYPE_LIST) {
		int n = 0;
		struct uci_element *e;
		uci_foreach_element(&ptr.o->v.list, e)
			n++;
		if (n == 0)
			return 0;
		char **arr = malloc(sizeof(char *) * (size_t)n);
		if (!arr)
			return 0;
		int i = 0;
		uci_foreach_element(&ptr.o->v.list, e)
			arr[i++] = strdup(e->name);
		*out = arr;
		return n;
	}

	return 0;
}

/* Does this init script register a procd service (and so fire service.start)? */
static int is_procd_service(const char *name)
{
	char path[256];
	snprintf(path, sizeof(path), "/etc/init.d/%s", name);

	FILE *f = fopen(path, "r");
	if (!f)
		return 0;

	char line[512];
	int procd = 0;
	while (fgets(line, sizeof(line), f)) {
		if (strstr(line, "USE_PROCD=1") ||
		    strstr(line, "procd_open_instance")) {
			procd = 1;
			break;
		}
	}
	fclose(f);
	return procd;
}

/* Estimate the number of service.start events the bridge will observe: the
 * enabled boot scripts (/etc/rc.d/S*) that are procd services AND run after the
 * bridge's own script (`self`). The one-shot setup scripts (fstab, sysctl,
 * done, …) never fire a service.start, so counting them over-estimates and the
 * bar tops out early; the bridge's own script and anything before it ran before
 * it subscribed, so their events were missed and must not be counted either.
 * /etc/rc.d/S* is iterated in boot (sorted) order. Recomputed every boot, so
 * adding or removing services is reflected automatically. */
static int count_boot_scripts(const char *self)
{
	int n = 0;
	glob_t g;
	if (glob("/etc/rc.d/S*", 0, NULL, &g) == 0) {
		for (size_t i = 0; i < g.gl_pathc; i++) {
			/* "/etc/rc.d/S20network" -> "network" */
			const char *base = strrchr(g.gl_pathv[i], '/');
			base = base ? base + 1 : g.gl_pathv[i];
			if (*base == 'S') {
				base++;
				while (*base >= '0' && *base <= '9')
					base++;
			}
			/* Our own script: drop everything counted so far (it ran before
			 * we subscribed) and count only what comes after. */
			if (self && *self && strcmp(base, self) == 0) {
				n = 0;
				continue;
			}
			if (is_procd_service(base))
				n++;
		}
		globfree(&g);
	}
	return n > 0 ? n : 1;
}

static void load_config(void)
{
	/* Defaults. */
	cfg.enabled      = 1;
	cfg.resume       = 1;
	cfg.total        = 0;
	cfg.idle_timeout = 60;
	cfg.idle_action  = strdup("finished");
	cfg.done_at_pct  = 90;
	cfg.settle_timeout = 5;
	cfg.done_service = strdup("done");
	cfg.done_event   = strdup("boot.done");
	cfg.self_script  = strdup("splash-progress");
	cfg.fin_delay    = 0;
	cfg.hold         = 2;
	cfg.fin_action   = strdup("none");
	cfg.fin_timeout  = 1;
	cfg.fin_color    = strdup("#000000");
	cfg.fail_hold    = 15;
	cfg.fail_action  = strdup("none");
	cfg.fail_timeout = 1;
	cfg.fail_color   = strdup("#000000");

	struct uci_context *uci = uci_alloc_context();
	if (uci) {
		char *s;

		/* [global] */
		if ((s = uci_str(uci, "splash.global.enabled"))) {
			cfg.enabled = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.global.resume"))) {
			cfg.resume = atoi(s); free(s);
		}
		char *total = uci_str(uci, "splash.global.total");
		if (total && strcmp(total, "auto") != 0 && atoi(total) > 0)
			cfg.total = atoi(total);
		free(total);
		if ((s = uci_str(uci, "splash.global.idle_timeout"))) {
			cfg.idle_timeout = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.global.idle_action"))) {
			free(cfg.idle_action); cfg.idle_action = s;
		}
		if ((s = uci_str(uci, "splash.global.done_at_pct"))) {
			cfg.done_at_pct = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.global.settle_timeout"))) {
			cfg.settle_timeout = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.global.done_service"))) {
			free(cfg.done_service); cfg.done_service = s;
		}
		if ((s = uci_str(uci, "splash.global.done_event"))) {
			free(cfg.done_event); cfg.done_event = s;
		}
		if ((s = uci_str(uci, "splash.global.self_script"))) {
			free(cfg.self_script); cfg.self_script = s;
		}

		/* [start] — one-time setup sequence (accepts a list or single option). */
		cfg.start_msg_n = uci_list(uci, "splash.start.start_msg",
		                           &cfg.start_msg);

		/* [progress] — one list of per-tick templates (accepts a single
		 * `option status_msg` too: uci_list reads either form). */
		cfg.status_msg_n = uci_list(uci, "splash.progress.status_msg",
		                            &cfg.status_msg);

		/* [finished] */
		cfg.done_msg_n = uci_list(uci, "splash.finished.done_msg",
		                          &cfg.done_msg);
		if ((s = uci_str(uci, "splash.finished.delay"))) {
			cfg.fin_delay = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.finished.hold"))) {
			cfg.hold = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.finished.exit_action"))) {
			free(cfg.fin_action); cfg.fin_action = s;
		}
		if ((s = uci_str(uci, "splash.finished.exit_timeout"))) {
			cfg.fin_timeout = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.finished.exit_color"))) {
			free(cfg.fin_color); cfg.fin_color = s;
		}

		/* [fail] */
		cfg.fail_msg_n = uci_list(uci, "splash.fail.fail_msg",
		                          &cfg.fail_msg);
		if ((s = uci_str(uci, "splash.fail.hold"))) {
			cfg.fail_hold = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.fail.exit_action"))) {
			free(cfg.fail_action); cfg.fail_action = s;
		}
		if ((s = uci_str(uci, "splash.fail.exit_timeout"))) {
			cfg.fail_timeout = atoi(s); free(s);
		}
		if ((s = uci_str(uci, "splash.fail.exit_color"))) {
			free(cfg.fail_color); cfg.fail_color = s;
		}

		uci_free_context(uci);
	}

	if (cfg.total <= 0)
		cfg.total = count_boot_scripts(cfg.self_script);
}

static void usage(FILE *out, const char *prog)
{
	fprintf(out,
"Usage: %s [OPTION]\n"
"\n"
"OpenWrt procd -> splash-drm boot-progress bridge. Subscribes to procd's\n"
"'service' ubus object and drives the splash from real service.start events,\n"
"filling UCI-configured JSON templates and sending them to the splash-drm\n"
"control socket. Normally launched as the 'splash-progress' procd service, not\n"
"run by hand; with no option it loads /etc/config/splash and runs the bridge.\n"
"\n"
"Options:\n"
"  -v, --version   print the version (tracks splash-drm) and exit\n"
"  -h, --help      print this help and exit\n"
"\n"
"Configuration lives in UCI at /etc/config/splash (see the bundled sample and\n"
"ubus-progress/README.md). The per-tick templates expand these macros:\n"
"  ${value}    progress as a 0..1 fraction        (e.g. \"value\":\"${value}\")\n"
"  ${percent}  progress as a 0..100 integer        (e.g. a \"${percent}%%\" text)\n"
"  ${count}    services advanced so far (N)\n"
"  ${total}    the denominator (M), so \"${count}/${total}\"\n"
"  ${service}  the name of the service that just started\n"
"  ${idle}     stall time in seconds (fail templates only)\n"
"\n"
"A template may be a single command object or a JSON array of them, e.g.\n"
"  '[{\"progress\":{\"id\":0,\"value\":\"${value}\"}},"
"{\"text\":{\"id\":1,\"text\":\"${percent}%%\"}}]'\n"
"which the daemon runs as one batch.\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *prog = argv[0] ? argv[0] : "ubus-progress";
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
			printf("%s\n", SPLASH_VERSION);
			return 0;
		}
		if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			usage(stdout, prog);
			return 0;
		}
		fprintf(stderr, "%s: unknown option '%s'\n", prog, argv[i]);
		usage(stderr, prog);
		return 2;
	}

	load_config();
	if (!cfg.enabled) {
		fprintf(stderr, "ubus-progress: disabled in UCI, exiting\n");
		return 0;
	}

	uloop_init();

	struct ubus_context *ubus = ubus_connect(NULL);
	if (!ubus) {
		fprintf(stderr, "ubus-progress: cannot connect to ubus\n");
		return 1;
	}
	ubus_add_uloop(ubus);

	g_sub.cb = notify_cb;
	if (ubus_register_subscriber(ubus, &g_sub)) {
		fprintf(stderr, "ubus-progress: cannot register subscriber\n");
		return 1;
	}

	uint32_t id;
	if (ubus_lookup_id(ubus, "service", &id) ||
	    ubus_subscribe(ubus, &g_sub, id)) {
		fprintf(stderr, "ubus-progress: cannot subscribe to 'service'\n");
		return 1;
	}

	/* The splash is typically started suspended from the initramfs and left
	 * frozen until something drives it. Resuming here, as the bridge takes
	 * over, removes the need for a separate preinit resume hook. Idempotent:
	 * harmless if the daemon is already running or not reachable. */
	if (cfg.resume)
		splash_send("{\"system\":\"resume\"}");

	/* One-time start sequence, run once the splash is alive. It can create or
	 * replace any element — e.g. drop an initramfs "Preparing…" text and build
	 * the progress bar — so the whole scene can live in UCI instead of the
	 * initramfs. Macros read 0% / count 0 of total. */
	if (cfg.start_msg_n > 0) {
		define_progress(0.0, 0, cfg.total);
		for (int i = 0; i < cfg.start_msg_n; i++)
			send_template(cfg.start_msg[i]);
	}

	/* Arm the idle watchdog: if no service.start ever arrives, boot stalled. */
	g_idle_timer.cb = on_idle;
	arm_idle();

	uloop_run();

	uloop_done();
	ubus_free(ubus);
	return 0;
}
