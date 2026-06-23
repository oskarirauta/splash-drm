/*
 * main.c - splash-drm daemon entry point.
 *
 * Sets up DRM, the control socket and signal handling, then runs the
 * event loop: it blocks in poll() when idle, ticks at RENDER_FPS while
 * an animation or spinner is active, and exits cleanly on a termination
 * signal or - if --timeout is given - after an idle watchdog period.
 */

#include "splash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <sys/types.h>

/* ========================================================================
 * Signal Handling
 * ======================================================================== */

/*
 * Set by SIGTERM/SIGINT/SIGHUP so the main loop can exit and run its
 * cleanup. drm_cleanup() restores the saved CRTC; skipping it would leave
 * the screen showing a stale splash buffer.
 */
static volatile sig_atomic_t g_terminate = 0;
static volatile sig_atomic_t g_term_sig  = 0;

const char *exit_reason = "exit command (running=0)";	/* default: cmd_exit */

/* See log.h note in splash.h: --check sets this so asset loads are skipped. */
int g_validate_only = 0;

static void on_signal(int sig) {
	g_term_sig  = sig;
	g_terminate = 1;
}

/* Install termination handlers and ignore SIGPIPE, so a client vanishing
 * mid-write can never kill the daemon. */
static void install_signal_handlers(void) {
	struct sigaction sa;
	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = on_signal;
	/* No SA_RESTART: a signal should interrupt poll() so the loop wakes. */
	sigaction(SIGTERM, &sa, NULL);
	sigaction(SIGINT,  &sa, NULL);
	/* Ignore SIGHUP: the shell sends it to background jobs when exiting
	 * (e.g. during switch_root). The daemon is controlled via the socket,
	 * so SIGHUP has no meaningful role here. */
	signal(SIGHUP,  SIG_IGN);
	signal(SIGPIPE, SIG_IGN);
}

/* ========================================================================
 * Usage
 * ======================================================================== */

static void print_usage(const char *prog) {
	fprintf(stderr,
		"splash-drm v%s - DRM/KMS bootsplash daemon\n\n"
		"Usage: %s <drm_device> [options]\n\n"
		"Arguments:\n"
		"  drm_device             DRM device path (e.g. /dev/dri/card0)\n\n"
		"Options:\n"
		"  --config <file|json>   Load configuration (fonts, defaults)\n"
		"  --cmds <file|json>     Execute initial commands on startup\n"
		"  --timeout <seconds>    Exit if idle for this long (watchdog)\n"
		"  --fork                 Fork to background; parent exits immediately\n"
		"                         (recommended for initramfs use — guarantees\n"
		"                         a new session so switch_root cannot kill us)\n"
		"  --headless             Initialise DRM but never program the CRTC, so\n"
		"                         the console stays visible; renders off-screen\n"
		"                         only (for debugging logs live during boot)\n"
		"  --check                Validate --config/--cmds without opening DRM\n"
		"                         and exit (0 = ok); for testing boot configs\n"
		"  --dump <file.png>      Render one frame (from --config/--cmds) to a\n"
		"                         PNG and exit; pair with --headless to avoid\n"
		"                         touching the live console\n"
		"  -q, --quiet            Silence all output (even errors)\n"
		"  -v / -vv / -vvv        Log threshold: info / debug / trace. The\n"
		"                         default (no flag) is error only; each level\n"
		"                         also includes the lower ones, so warnings\n"
		"                         already show at -v. (Repeats add up: -v -vv\n"
		"                         is the same as -vvv.)\n"
		"  --debug                Alias for -vv (debug verbosity)\n"
		"  --log <target>         Log sink: auto (default), stderr, syslog, kmsg\n"
		"  -V, --version          Print version and exit\n"
		"  -h, --help             Print this summary and exit\n"
		"  --help <cmd>           Print full parameter list for a command\n\n"
		"Config JSON format:\n"
		"  {\"fonts\": [\n"
		"    {\"slot\": 0, \"path\": \"DejaVuSans.ttf\", \"size\": 24}\n"
		"  ]}\n\n"
		"Available commands:\n"
		"  clear       bg_color         image\n"
		"  text        remove_text\n"
		"  rect        remove_rect\n"
		"  ellipse     circle           remove_ellipse  remove_circle\n"
		"  line        remove_line\n"
		"  stepper     remove_stepper\n"
		"  marquee     remove_marquee\n"
		"  sprite      remove_sprite\n"
		"  overlay     remove_overlay\n"
		"  progress    update_progress  hide_progress\n"
		"  arc         update_arc       hide_arc\n"
		"  spinner     console          console_write  remove_console\n"
		"  qr          remove_qr\n"
		"  animate     query\n"
		"  suspend     resume           status         ready           exit\n\n"
		"Run '%s --help <cmd>' for full parameter details.\n\n"
		"Examples:\n"
		"  %s /dev/dri/card0\n"
		"  %s /dev/dri/card0 --config /etc/splash/config.json --cmds /etc/splash/boot.json\n"
		"  %s --help arc\n",
		SPLASH_VERSION, prog, prog, prog, prog, prog);
}

/* ========================================================================
 * Config / Command File Helpers
 * ======================================================================== */

/* True if the string looks like inline JSON rather than a file path. */
static int is_json_string(const char *str) {
	if (!str || !*str)
		return 0;
	while (*str == ' ' || *str == '\t' || *str == '\n')
		str++;
	return (*str == '{' || *str == '[');
}

/*
 * Read a whole file into a newly allocated, NUL-terminated buffer.
 * fseek/ftell/fread are all checked, so a stat error or a short read can
 * never leave the buffer unterminated. Caller frees the result.
 */
static char *read_file(const char *path) {
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;

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

/* Load the scene document: font slots (loaded once at startup), the backdrop
 * colour, and any declarative elements. Older fonts-only configs still work —
 * the background/elements sections are simply absent. */
int load_config(splash_state_t *st, const char *config_str) {
	char *json_data = NULL;
	int needs_free = 0;

	if (is_json_string(config_str)) {
		json_data = (char *)config_str;
	} else {
		json_data = read_file(config_str);
		if (!json_data) {
			LOGE("cannot read config file: %s", config_str);
			return -1;
		}
		needs_free = 1;
	}

	cJSON *root = cJSON_Parse(json_data);
	if (needs_free)
		free(json_data);

	if (!root) {
		LOGE("invalid config JSON");
		return -1;
	}

	json_substitute(root);

	cJSON *fonts = cJSON_GetObjectItem(root, "fonts");
	if (fonts && cJSON_IsArray(fonts)) {
		int count = cJSON_GetArraySize(fonts);
		for (int i = 0; i < count; i++) {
			cJSON *font = cJSON_GetArrayItem(fonts, i);
			if (!cJSON_IsObject(font))
				continue;

			int slot         = get_int(font, "slot", -1);
			const char *path = get_string(font, "path", NULL);
			float size       = get_float(font, "size", 24.0f);

			if (slot >= 0 && slot < MAX_FONTS && path) {
				if (font_load(path, size, slot) < 0) {
					LOGE("could not load font %s to slot %d",
					     path, slot);
				} else {
					LOGD("loaded font %s to slot %d (size %.1f)",
					     path, slot, size);
				}
			}
		}
	}

	/* Backdrop colour: a string ("#101418") or {"color": ...}. */
	cJSON *bg = cJSON_GetObjectItem(root, "background");
	if (bg) {
		if (cJSON_IsString(bg))
			st->bg_color = parse_color(bg->valuestring);
		else if (cJSON_IsObject(bg)) {
			const char *c = get_string(bg, "color", NULL);
			if (c)
				st->bg_color = parse_color(c);
		}
	}

	/* Declarative scene: run the elements array as a startup batch. */
	cJSON *elements = cJSON_GetObjectItem(root, "elements");
	if (elements && cJSON_IsArray(elements))
		process_json_batch(st, elements, -1, NULL, NULL);

	cJSON_Delete(root);
	return 0;
}

/* Run the optional startup command batch (client_idx -1: no replies).
 * out_total/out_errors (optional) report how many commands ran and failed. */
int process_startup_cmds(splash_state_t *st, const char *cmds_str,
                         int *out_total, int *out_errors) {
	char *json_data = NULL;
	int needs_free = 0;

	if (is_json_string(cmds_str)) {
		json_data = (char *)cmds_str;
	} else {
		json_data = read_file(cmds_str);
		if (!json_data) {
			LOGE("cannot read commands file: %s", cmds_str);
			return -1;
		}
		needs_free = 1;
	}

	cJSON *root = cJSON_Parse(json_data);
	if (needs_free)
		free(json_data);

	if (!root) {
		LOGE("invalid commands JSON");
		return -1;
	}

	json_substitute(root);

	int total = 0, errors = 0;
	process_json_batch(st, root, -1, &total, &errors);
	cJSON_Delete(root);

	if (out_total)  *out_total  = total;
	if (out_errors) *out_errors = errors;

	if (errors > 0) {
		LOGE("%d of %d startup command(s) failed", errors, total);
		return -1;
	}
	return 0;
}

/* ========================================================================
 * Daemonize
 * ======================================================================== */

/*
 * Detach so the daemon can outlive the initramfs and the switch to the
 * real rootfs. Called only after DRM, the socket and the first frame are
 * up, so the splash is already on screen and any init error has already
 * been reported in the foreground. The parent then exits, letting the
 * init script reach switch_root without waiting (a trailing '&' becomes
 * unnecessary). fork() hands every open fd to the child, and the shared
 * open file descriptions keep DRM master and the listening socket alive;
 * only the inherited stdio - still wired to /dev/console on the initramfs
 * - is dropped.
 */
static int daemonize(void) {
	pid_t pid = fork();
	if (pid < 0)
		return -1;			/* fork failed: caller stays in the foreground */
	if (pid > 0)
		_exit(0);			/* parent: return control to the init script */

	setsid();				/* new session, no controlling terminal */

	/* Don't pin the initramfs root as cwd; '/' is the real rootfs after
	 * the switch and is harmless before it. */
	if (chdir("/") != 0) {
		/* non-fatal: no relative paths are used after startup */
	}

	/* Drop the boot console: stop holding /dev/console open and stop
	 * writing onto a framebuffer the real init will want back. */
	int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
	if (nul >= 0) {
		dup2(nul, STDIN_FILENO);
		dup2(nul, STDOUT_FILENO);
		dup2(nul, STDERR_FILENO);
		if (nul > STDERR_FILENO)
			close(nul);
	}
	return 0;
}

/* ========================================================================
 * Entry Point
 * ======================================================================== */

int main(int argc, char **argv) {
	if (argc < 2) {
		print_usage(argv[0]);
		return 1;
	}

	const char *device     = argv[1];
	const char *config_arg = NULL;
	const char *cmds_arg   = NULL;
	int         fork_daemon = 0;
	int         headless    = 0;
	int         check_mode  = 0;
	const char *dump_path   = NULL;

	int        verbosity = 0;   /* 0=error 1=info 2=debug 3=trace (raised by -v) */
	int        quiet     = 0;   /* -q: silence everything, even errors */
	log_sink_t log_sink  = LOG_SINK_AUTO;

	splash_state_t st = {0};
	st.bg_color   = 0;
	st.bg_opacity = 1.0f;			/* fully opaque until a crossfade runs */
	st.server_fd  = -1;
	for (int i = 0; i < MAX_SOCKET_CLIENTS; i++)
		st.client_fds[i] = -1;

	if (strcmp(argv[1], "-V") == 0 || strcmp(argv[1], "--version") == 0) {
		printf("splash-drm v%s\n", SPLASH_VERSION);
		return 0;
	} else if (strcmp(argv[1], "--help") == 0 && argc >= 3) {
		print_cmd_help(argv[2]);
		return 0;
	} else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		print_usage(argv[0]);
		return 0;
	}

	if (argv[1][0] == '-') {
		fprintf(stderr, "splash-drm: '%s' is not a DRM device path\n"
		                "The first argument must be a device (e.g. /dev/dri/card0).\n"
		                "Run '%s -h' for usage.\n", argv[1], argv[0]);
		return 1;
	}

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
			config_arg = argv[++i];
		}
		else if (strcmp(argv[i], "--cmds") == 0 && i + 1 < argc) {
			cmds_arg = argv[++i];
		}
		else if ((strcmp(argv[i], "-D") == 0 ||
		          strcmp(argv[i], "--define") == 0) && i + 1 < argc) {
			if (subst_define(argv[++i]) != 0)
				fprintf(stderr, "splash-drm: invalid -D '%s' "
				        "(expected NAME=value)\n", argv[i]);
		}
		else if (strncmp(argv[i], "-D", 2) == 0 && argv[i][2] != '\0') {
			if (subst_define(argv[i] + 2) != 0)
				fprintf(stderr, "splash-drm: invalid -D '%s' "
				        "(expected NAME=value)\n", argv[i] + 2);
		}
		else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
			const char *arg = argv[++i];
			char *end;
			errno = 0;
			long secs = strtol(arg, &end, 10);
			if (errno != 0 || end == arg || *end != '\0' || secs <= 0) {
				fprintf(stderr, "splash-drm: invalid --timeout '%s' "
				        "(expected a positive number of seconds)\n", arg);
				return 1;
			}
			if (secs > 86400)		/* clamp to a day; avoids the *1000 overflow */
				secs = 86400;
			st.watchdog_ms = (uint32_t)secs * 1000u;
		}
		else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
			quiet = 1;
		}
		else if (strcmp(argv[i], "-v") == 0) {
			verbosity += 1;
		}
		else if (strcmp(argv[i], "-vv") == 0) {
			verbosity += 2;
		}
		else if (strcmp(argv[i], "-vvv") == 0) {
			verbosity += 3;
		}
		else if (strcmp(argv[i], "--debug") == 0) {
			if (verbosity < 2)
				verbosity = 2;
		}
		else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
			int ok;
			log_sink_t s = log_sink_from_string(argv[++i], &ok);
			if (ok)
				log_sink = s;
			else
				fprintf(stderr, "splash-drm: unknown --log target '%s' "
				        "(use auto/stderr/syslog/kmsg)\n", argv[i]);
		}
		else if (strcmp(argv[i], "--fork") == 0) {
			fork_daemon = 1;
		}
		else if (strcmp(argv[i], "--headless") == 0) {
			headless = 1;
		}
		else if (strcmp(argv[i], "--check") == 0) {
			check_mode = 1;
		}
		else if (strcmp(argv[i], "--dump") == 0 && i + 1 < argc) {
			dump_path = argv[++i];
		}
		else if (strcmp(argv[i], "-V") == 0 || strcmp(argv[i], "--version") == 0) {
			printf("splash-drm v%s\n", SPLASH_VERSION);
			return 0;
		}
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}

	/* Resolve verbosity into a threshold and bring the logger up before any
	 * DRM/socket work, so early failures are reported through the same sink. */
	log_level_t log_level = quiet      ? LOG_SILENT :
	                        verbosity <= 0 ? LOG_ERROR :
	                        verbosity == 1 ? LOG_INFO  :
	                        verbosity == 2 ? LOG_DEBUG : LOG_TRACE;
	log_init(log_level, log_sink);

	if (check_mode) {
		/* Validate --config/--cmds without opening DRM or the socket, so a
		 * boot.json can be checked on a build host. Surface warnings even at
		 * the default (ERROR) threshold. */
		if (log_threshold < LOG_INFO)
			log_set_level(LOG_INFO);
		g_validate_only = 1;	/* skip asset loads; validate structure only */

		/* Stub a typical resolution so percentage coordinates resolve. */
		st.drm.mode.hdisplay = 1920;
		st.drm.mode.vdisplay = 1080;

		int problems = 0, total = 0, errors = 0;
		if (config_arg && load_config(&st, config_arg) < 0) {
			LOGE("config: failed to load");
			problems++;
		}
		if (cmds_arg) {
			if (process_startup_cmds(&st, cmds_arg, &total, &errors) < 0 &&
			    total == 0)
				problems++;		/* read/parse failure: nothing ran */
			problems += errors;
		}

		font_unload_all();
		fprintf(stderr, "check: %d command(s) parsed, %d problem(s) - %s\n",
		        total, problems, problems ? "FAILED" : "OK");
		return problems ? 1 : 0;
	}

	if (drm_init(&st.drm, device, headless) < 0) {
		LOGE("failed to initialize DRM on %s", device);
		return 1;
	}

	LOGI("v%s, DRM %dx%d @ %dHz", SPLASH_VERSION,
	     st.drm.mode.hdisplay, st.drm.mode.vdisplay,
	     st.drm.mode.vrefresh);
	if (headless)
		LOGI("headless: rendering off-screen, console left visible");

	if (config_arg) {
		if (load_config(&st, config_arg) < 0)
			LOGW("failed to load config");
	}

	/* --dump is a one-shot screenshot: skip the control socket entirely so it
	 * never collides with a running daemon's abstract name. */
	if (!dump_path && socket_init(&st) < 0) {
		LOGE("failed to create abstract socket");
		drm_cleanup(&st.drm);
		font_unload_all();
		return 1;
	}

	install_signal_handlers();

	kbd_init(&st);

	st.running      = 1;
	st.needs_render = 1;
	st.frozen       = 0;

	if (cmds_arg)
		process_startup_cmds(&st, cmds_arg, NULL, NULL);

	/* Pick up any animation/spinner started by the startup commands, so
	 * the first poll already uses the short (RENDER_FPS) timeout. */
	st.anim_running     = anim_tick(&st, now_ms());
	st.last_activity_ms = now_ms();

	if (st.needs_render)
		render_frame(&st);

	/* --dump: one frame is now composed; write it out and exit. */
	if (dump_path) {
		int rc = write_buffer_png(&st.drm.buf[st.drm.front_buf], dump_path);
		if (rc == 0)
			LOGI("dumped frame to %s", dump_path);
		else
			LOGE("failed to write %s", dump_path);
		kbd_cleanup(&st);
		socket_cleanup(&st);
		clear_all_elements(&st);
		free_image(&st.bg_image);
		font_unload_all();
		drm_cleanup(&st.drm);
		log_close();
		return rc == 0 ? 0 : 1;
	}

	if (fork_daemon) {
		if (daemonize() < 0) {
			LOGW("daemonize failed, staying in foreground");
		} else {
			/* Child: stdio is now /dev/null, so an AUTO sink must move off
			 * stderr onto syslog/kmsg. A sink forced via --log is kept. */
			log_reopen(1);
			LOGI("forked to background (pid %d)", (int)getpid());
		}
	} else {
		/* setsid() fails with EPERM when we are already a process-group leader
		 * (the common `exec splash-drm` init case); surface it so an operator
		 * can see the daemon stayed in the original session. */
		if (setsid() < 0)
			LOGW("setsid failed (%s); staying in current session - "
			     "use --fork for a clean initramfs detach", strerror(errno));
	}

	LOGT("entering main loop");

	while (st.running && !g_terminate) {
		struct pollfd fds[1 + MAX_SOCKET_CLIENTS + 1];
		int nfds = 0;

		socket_poll(&st, fds, &nfds);

		if (st.kbd_fd >= 0) {
			fds[nfds].fd     = st.kbd_fd;
			fds[nfds].events = POLLIN;
			nfds++;
		}

		/* Block indefinitely when idle; tick at RENDER_FPS while an
		 * animation or spinner is running. The watchdog, when armed,
		 * caps the wait so the inactivity deadline is always honoured. */
		int timeout = (st.frozen || !st.anim_running)
		              ? -1 : (1000 / RENDER_FPS);

		if (st.watchdog_ms > 0 && !st.frozen) {
			uint64_t since = now_ms() - st.last_activity_ms;
			int remain = (since >= st.watchdog_ms)
			             ? 0 : (int)(st.watchdog_ms - since);
			if (timeout < 0 || remain < timeout)
				timeout = remain;
		}

		if (st.exit_at_ms > 0) {
			uint64_t now = now_ms();
			int remain = (now >= st.exit_at_ms)
			             ? 0 : (int)(st.exit_at_ms - now);
			if (timeout < 0 || remain < timeout)
				timeout = remain;
		}

		int ret = poll(fds, nfds, timeout);

		if (ret < 0) {
			if (errno == EINTR)
				continue;		/* signal: re-check the loop guard */
			exit_reason = "poll error";
			LOGE("poll: %s", strerror(errno));
			break;
		}

		socket_process(&st, fds, nfds);
		kbd_process(&st);

		/* Watchdog: exit if no command arrived within the timeout, so a
		 * stuck boot script can never leave the splash up forever. */
		if (st.watchdog_ms > 0 && !st.frozen &&
		    now_ms() - st.last_activity_ms >= st.watchdog_ms) {
			exit_reason = "watchdog idle timeout";
			break;
		}

		/* Delayed exit: triggered by {"cmd":"exit","delay":<seconds>}. */
		if (st.exit_at_ms > 0 && now_ms() >= st.exit_at_ms) {
			exit_reason = "delayed exit elapsed";
			break;
		}

		/* Advance animations on the monotonic clock. */
		if (!st.frozen) {
			st.anim_running = anim_tick(&st, now_ms());
			if (st.anim_running)
				st.needs_render = 1;
		}

		if (!st.frozen && st.needs_render) {
			if (st.hidden) {
				drm_buffer_t *buf =
				    &st.drm.buf[st.drm.front_buf ^ 1];
				draw_filled_rect(buf, 0, 0,
				                 (int)buf->width, (int)buf->height,
				                 0xFF000000);
				drm_flip(&st.drm);
				st.needs_render = 0;
			} else {
				render_frame(&st);
			}
		}
	}

	/* Report why the loop ended exactly once. */
	if (g_terminate) {
		const char *sig =
			(g_term_sig == SIGHUP)  ? "SIGHUP"  :
			(g_term_sig == SIGTERM) ? "SIGTERM" :
			(g_term_sig == SIGINT)  ? "SIGINT"  : "signal";
		LOGI("exiting on %s", sig);
	} else {
		LOGI("exiting: %s", exit_reason);
	}

	kbd_cleanup(&st);
	socket_cleanup(&st);
	clear_all_elements(&st);
	free_image(&st.bg_image);
	font_unload_all();
	drm_cleanup(&st.drm);

	LOGT("cleanup complete");
	log_close();
	return 0;
}
