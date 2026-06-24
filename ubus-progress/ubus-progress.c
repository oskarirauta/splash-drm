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
 *       option total        'auto'   # 'auto' = count /etc/rc.d/S*, or a number
 *       option done_event   'boot.done'  # ubus "service event" type = boot done
 *       option done_service 'done'   # service.start name that means "boot done"
 *       option idle_timeout '60'     # run idle_action if idle for Ns (0 = off)
 *       option idle_action  'finished'   # finished (settled) | fail (stalled)
 *
 *   config progress 'progress'       # per-tick (${value}=0..1, ${service}=name)
 *       option tick_msg     '{"progress":{"id":0,"value":"${value}"}}'
 *       option status_msg   '{"console":{"id":0,"write":"starting ${service}"}}'
 *
 *   config finished 'finished'       # boot completed normally
 *       list   done_msg     '{"progress":{"id":0,"remove":true}}'
 *       list   done_msg     '{"text":{"id":99,"text":"System ready!"}}'
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
	char  *done_service;   /* service.start name that means "boot done" */
	char  *done_event;     /* ubus "service event" type that means "boot done" */
	/* [progress] */
	char  *tick_msg;
	char  *status_msg;
	/* [finished] — boot completed normally */
	char **done_msg;
	int    done_msg_n;
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

/* Send a message list in order, then schedule the exit after `hold` seconds.
 * Shared by the finished (success) and fail (stall) outcomes. Fires once. */
static void run_sequence(char **msgs, int n, int hold,
                         const char *action, int timeout, const char *color)
{
	if (g_done)
		return;
	g_done = 1;
	uloop_timeout_cancel(&g_idle_timer);

	for (int i = 0; i < n; i++)
		send_template(msgs[i]);

	g_exit_json = build_exit(action, timeout, color);
	g_hold_timer.cb = do_exit;
	uloop_timeout_set(&g_hold_timer, hold > 0 ? hold * 1000 : 0);
}

static void finish(void)
{
	if (g_done)
		return;
	/* Fill the bar to 100% first, so it always completes even when the count
	 * never reached the (approximate) total, then run the done sequence. */
	subst_define("value=1.0");
	send_template(cfg.tick_msg);
	run_sequence(cfg.done_msg, cfg.done_msg_n, cfg.hold,
	             cfg.fin_action, cfg.fin_timeout, cfg.fin_color);
}

static void fail(void)
{
	char b[48];
	snprintf(b, sizeof(b), "idle=%d", cfg.idle_timeout);
	subst_define(b);
	run_sequence(cfg.fail_msg, cfg.fail_msg_n, cfg.fail_hold,
	             cfg.fail_action, cfg.fail_timeout, cfg.fail_color);
}

/* Idle watchdog: no service.start within idle_timeout. With a done event wired
 * up, this only fires on a genuine stall (idle_action "fail"); without one, the
 * quiet simply means boot settled (idle_action "finished", the safe default). */
static void on_idle(struct uloop_timeout *t)
{
	(void)t;
	if (cfg.idle_action && strcmp(cfg.idle_action, "fail") == 0)
		fail();
	else
		finish();
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

	/* Real activity: keep the idle watchdog from firing. */
	if (cfg.idle_timeout > 0)
		uloop_timeout_set(&g_idle_timer, cfg.idle_timeout * 1000);

	/* The boot-complete service is the explicit "done" signal. */
	if (cfg.done_service && *cfg.done_service &&
	    strcmp(name, cfg.done_service) == 0) {
		finish();
		return 0;
	}

	if (g_count < cfg.total)
		g_count++;

	double value = cfg.total > 0 ? (double)g_count / (double)cfg.total : 1.0;
	if (value > 1.0)
		value = 1.0;

	char vbuf[48];
	snprintf(vbuf, sizeof(vbuf), "value=%.4f", value);
	subst_define(vbuf);

	char sbuf[160];
	snprintf(sbuf, sizeof(sbuf), "service=%s", name);
	subst_define(sbuf);

	send_template(cfg.tick_msg);
	send_template(cfg.status_msg);

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

static int count_boot_scripts(void)
{
	int n = 0;
	glob_t g;
	if (glob("/etc/rc.d/S*", 0, NULL, &g) == 0) {
		n = (int)g.gl_pathc;
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
	cfg.done_service = strdup("done");
	cfg.done_event   = strdup("boot.done");
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
		if ((s = uci_str(uci, "splash.global.done_service"))) {
			free(cfg.done_service); cfg.done_service = s;
		}
		if ((s = uci_str(uci, "splash.global.done_event"))) {
			free(cfg.done_event); cfg.done_event = s;
		}

		/* [progress] */
		cfg.tick_msg   = uci_str(uci, "splash.progress.tick_msg");
		cfg.status_msg = uci_str(uci, "splash.progress.status_msg");

		/* [finished] */
		cfg.done_msg_n = uci_list(uci, "splash.finished.done_msg",
		                          &cfg.done_msg);
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
		cfg.total = count_boot_scripts();
}

int main(void)
{
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

	/* Arm the idle watchdog: if no service.start ever arrives, boot stalled. */
	g_idle_timer.cb = on_idle;
	if (cfg.idle_timeout > 0)
		uloop_timeout_set(&g_idle_timer, cfg.idle_timeout * 1000);

	uloop_run();

	uloop_done();
	ubus_free(ubus);
	return 0;
}
