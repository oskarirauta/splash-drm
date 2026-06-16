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

static void on_signal(int sig) {
	(void)sig;
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

static void print_cmd_help(const char *cmd) {
	if (strcmp(cmd, "text") == 0) {
		fprintf(stderr,
		"text — add or update a text label\n\n"
		"  id          int      element id (required)\n"
		"  text        string   content; \\n inserts a line break\n"
		"  x, y        coord    anchor position (default: screen centre)\n"
		"  align       0-2      0=left 1=center 2=right  (or string name)\n"
		"  valign      0-2      0=top  1=middle 2=bottom (or string name)\n"
		"  color       color    text color (default: white)\n"
		"  font        int      font slot index (default: 0)\n"
		"  size        float    font size in pixels\n"
		"  wrap        bool     word-wrap long lines\n"
		"  wrap_width  int      max line width in pixels (0 = screen width)\n"
		"  shadow      bool     enable soft drop shadow\n"
		"  shadow_dx/dy int     shadow offset in pixels (default: 2)\n"
		"  shadow_blur int      shadow blur radius (default: 4)\n"
		"  shadow_color color   shadow color (default: #00000080)\n"
		"  opacity     float    master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "rect") == 0) {
		fprintf(stderr,
		"rect — add or update a rectangle\n\n"
		"  id           int     element id (required)\n"
		"  x, y         coord   anchor position (default: screen centre)\n"
		"  w, h         coord   width and height\n"
		"  align/valign 0-2     positioning anchor\n"
		"  color        color   fill or border color\n"
		"  fill         bool    filled rectangle (default: true)\n"
		"  radius       int     corner radius in pixels\n"
		"  border_color color   border color\n"
		"  border_width int     border thickness in pixels\n"
		"  grad_color   color   gradient second stop\n"
		"  grad_dir     int     0=none 1=vertical 2=horizontal 3=diagonal\n"
		"  shadow       bool    soft drop shadow\n"
		"  shadow_dx/dy int     shadow offset\n"
		"  shadow_blur  int     shadow blur radius\n"
		"  shadow_color color   shadow color\n"
		"  opacity      float   master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "overlay") == 0) {
		fprintf(stderr,
		"overlay — add or update an image overlay\n\n"
		"  id           int     element id (required)\n"
		"  path         string  image file path (PNG / JPEG)\n"
		"  x, y         coord   anchor position\n"
		"  w, h         coord   display size (0 = auto from aspect ratio)\n"
		"  align/valign 0-2     positioning anchor\n"
		"  filter       int     0=nearest 1=bilinear 2=bicubic 3=lanczos\n"
		"  opacity      float   master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "progress") == 0) {
		fprintf(stderr,
		"progress — create or reconfigure a horizontal progress bar\n\n"
		"  id              int    element id (required)\n"
		"  x, y            coord  anchor position\n"
		"  w, h            coord  size\n"
		"  align/valign    0-2    positioning anchor\n"
		"  value           float  fill level 0.0-1.0\n"
		"  style           int    built-in theme: 0=blue 1=green 2=amber\n"
		"                         3=red 4=purple 5=cyan  (-1=custom)\n"
		"  bg_color        color  background track\n"
		"  bar_color       color  fill\n"
		"  bar_color2      color  gradient second stop\n"
		"  bar_gradient    int    gradient direction (GRAD_*)\n"
		"  border_color    color  border\n"
		"  text_color      color  percentage text\n"
		"  borderless      bool   suppress border\n"
		"  border_width    int    border thickness\n"
		"  radius          int    corner radius\n"
		"  font_slot       int    font for percentage label\n"
		"  font_size       float  size of percentage label\n"
		"  show_percent    bool   show percentage text\n"
		"  indeterminate   bool   sweeping highlight mode\n"
		"  indet_period_ms int    sweep cycle time in ms\n"
		"  shadow          bool   drop shadow on the whole bar\n"
		"  opacity         float  master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "arc") == 0) {
		fprintf(stderr,
		"arc — create or reconfigure a circular/arc progress bar\n\n"
		"  id              int    element id (required)\n"
		"  x, y            coord  centre (-1 = screen centre on that axis)\n"
		"  radius          int    outer radius in pixels\n"
		"  thickness       int    stroke width (0 = radius/4)\n"
		"  value           float  fill level 0.0-1.0\n"
		"  start_angle     float  start angle in degrees; 0=right, CW\n"
		"                         default: -90 (top of circle)\n"
		"  sweep           float  total arc in degrees (default: 360)\n"
		"  bg_color        color  background (unfilled) arc; alpha 0 = hidden\n"
		"  bar_color       color  filled arc\n"
		"  bar_color2      color  gradient second stop\n"
		"  bar_gradient    int    0=solid, 1=sweep gradient along the arc\n"
		"  cap             int    0=flat ends 1=round end caps\n"
		"  font_slot       int    font for centre label\n"
		"  font_size       float  size of centre label\n"
		"  text_color      color  centre label color\n"
		"  show_percent    bool   show percentage in the centre\n"
		"  indeterminate   bool   spinning highlight mode\n"
		"  indet_period_ms int    rotation cycle time in ms (default: 1200)\n"
		"  opacity         float  master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "spinner") == 0) {
		fprintf(stderr,
		"spinner — create/show/hide an Apple-style rotating spinner\n\n"
		"  id          int     element id (required)\n"
		"  x, y        coord   centre (-1 = screen centre on that axis)\n"
		"  radius      int     outer radius in pixels (default: 36)\n"
		"  spokes      int     number of spokes (default: 12)\n"
		"  color       color   spoke color (default: white)\n"
		"  period      int     full rotation time in ms (default: 900)\n"
		"  action      string  \"show\" or \"hide\" (default: show)\n"
		"  opacity     float   master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "console") == 0) {
		fprintf(stderr,
		"console — create or reconfigure a scrolling log area\n\n"
		"  id          int     element id (required)\n"
		"  x, y        coord   top-left position (-1 = centred)\n"
		"  w, h        coord   width and height\n"
		"  font_slot   int     font slot\n"
		"  size        float   font size in pixels\n"
		"  color       color   text color\n"
		"  bg_color    color   background fill; alpha 0 = transparent\n"
		"  padding     int     inner margin in pixels\n"
		"  max_lines   int     ring buffer capacity (max: 64)\n"
		"  opacity     float   master alpha 0.0-1.0\n\n"
		"console_write — push text into a console element\n\n"
		"  id          int     console id (required)\n"
		"  text        string  text to append; \\n splits into multiple lines\n");
	} else if (strcmp(cmd, "qr") == 0) {
		fprintf(stderr,
		"qr — create or update a QR code element\n\n"
		"  id          int     element id (required)\n"
		"  text        string  payload to encode (required)\n"
		"  x, y        coord   top-left position (-1 = centred on that axis)\n"
		"  align       0-2     horizontal anchor on x\n"
		"  valign      0-2     vertical anchor on y\n"
		"  module_px   int     pixels per QR module (0 = auto)\n"
		"  border      int     quiet zone in modules (default: 4)\n"
		"  ecc         int     error correction: 0=low 1=medium 2=quartile 3=high\n"
		"  color       color   dark module color (default: black)\n"
		"  bg_color    color   light background; alpha 0 = transparent\n"
		"  opacity     float   master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "image") == 0) {
		fprintf(stderr,
		"image — set the background image\n\n"
		"  path        string  image file path (PNG / JPEG)\n"
		"  mode        int     0=cover 1=contain 2=stretch 3=none 4=custom\n"
		"  scale       float   scale factor (mode=4 only)\n"
		"  filter      int     0=nearest 1=bilinear 2=bicubic 3=lanczos\n"
		"  crossfade   int     fade duration in ms (0 = instant)\n");
	} else if (strcmp(cmd, "animate") == 0) {
		fprintf(stderr,
		"animate — animate an element's opacity\n\n"
		"  type         string  element type: text, rect, overlay, progress,\n"
		"                       arc, spinner, console, qr\n"
		"  id           int     element id\n"
		"  from         float   start opacity (default: current)\n"
		"  to           float   end opacity\n"
		"  duration     int     duration in ms\n"
		"  easing       string  linear, ease_in, ease_out, ease_in_out\n"
		"  repeat       bool    loop animation (ping-pong)\n"
		"  remove_on_end bool   deactivate element when animation ends\n");
	} else if (strcmp(cmd, "query") == 0) {
		fprintf(stderr,
		"query — read back current element state\n\n"
		"  type        string  element type: text, rect, overlay, progress,\n"
		"                      arc, spinner, console, qr\n"
		"  id          int     element id\n\n"
		"Returns the element's current position, value/text, opacity,\n"
		"and type-specific fields (e.g. value for arc/progress, text\n"
		"for text/qr, line_count for console).\n");
	} else if (strcmp(cmd, "status") == 0) {
		fprintf(stderr,
		"status — query daemon state\n\n"
		"Returns:\n"
		"  state       string  \"running\" or \"suspended\"\n"
		"  ready       bool    set by the ready command\n"
		"  hidden      bool    true if ESC-toggled to blank\n"
		"  width       int     display width in pixels\n"
		"  height      int     display height in pixels\n");
	} else {
		fprintf(stderr, "No detailed help available for '%s'.\n"
		                "Run with -h for the command list.\n", cmd);
	}
}

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
		"  -q, --quiet            Suppress all output\n"
		"  --debug                Enable debug output\n"
		"  -v, --version          Print version and exit\n"
		"  -h, --help             Print this summary and exit\n"
		"  --help <cmd>           Print full parameter list for a command\n\n"
		"Config JSON format:\n"
		"  {\"fonts\": [\n"
		"    {\"slot\": 0, \"path\": \"DejaVuSans.ttf\", \"size\": 24}\n"
		"  ]}\n\n"
		"Available commands:\n"
		"  clear       bg_color    image\n"
		"  text        remove_text\n"
		"  rect        remove_rect\n"
		"  overlay     remove_overlay\n"
		"  progress    update_progress  hide_progress\n"
		"  arc         update_arc       hide_arc\n"
		"  spinner     console          console_write  remove_console\n"
		"  qr          remove_qr\n"
		"  animate     query\n"
		"  suspend     resume      status  ready  exit\n\n"
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

/* Load the optional config (currently just font slots). */
int load_config(splash_state_t *st, const char *config_str) {
	char *json_data = NULL;
	int needs_free = 0;

	if (is_json_string(config_str)) {
		json_data = (char *)config_str;
	} else {
		json_data = read_file(config_str);
		if (!json_data) {
			if (!st->quiet)
				fprintf(stderr, "Cannot read config file: %s\n",
				        config_str);
			return -1;
		}
		needs_free = 1;
	}

	cJSON *root = cJSON_Parse(json_data);
	if (needs_free)
		free(json_data);

	if (!root) {
		if (!st->quiet)
			fprintf(stderr, "Invalid config JSON\n");
		return -1;
	}

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
				if (font_load(path, size, slot) < 0 && !st->quiet) {
					fprintf(stderr,
					        "Warning: Could not load font %s to slot %d\n",
					        path, slot);
				} else if (st->debug) {
					fprintf(stderr,
					        "[debug] Loaded font %s to slot %d (size %.1f)\n",
					        path, slot, size);
				}
			}
		}
	}

	cJSON_Delete(root);
	return 0;
}

/* Run the optional startup command batch (client_idx -1: no replies). */
int process_startup_cmds(splash_state_t *st, const char *cmds_str) {
	char *json_data = NULL;
	int needs_free = 0;

	if (is_json_string(cmds_str)) {
		json_data = (char *)cmds_str;
	} else {
		json_data = read_file(cmds_str);
		if (!json_data) {
			if (!st->quiet)
				fprintf(stderr, "Cannot read commands file: %s\n",
				        cmds_str);
			return -1;
		}
		needs_free = 1;
	}

	cJSON *root = cJSON_Parse(json_data);
	if (needs_free)
		free(json_data);

	if (!root) {
		if (!st->quiet)
			fprintf(stderr, "Invalid commands JSON\n");
		return -1;
	}

	process_json_batch(st, root, -1);
	cJSON_Delete(root);
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

	splash_state_t st = {0};
	st.bg_color   = 0;
	st.bg_opacity = 1.0f;			/* fully opaque until a crossfade runs */
	st.server_fd  = -1;
	for (int i = 0; i < MAX_SOCKET_CLIENTS; i++)
		st.client_fds[i] = -1;

	if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--version") == 0) {
		printf("splash-drm v%s\n", SPLASH_VERSION);
		return 0;
	} else if (strcmp(argv[1], "--help") == 0 && argc >= 3) {
		print_cmd_help(argv[2]);
		return 0;
	} else if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0) {
		print_usage(argv[0]);
		return 0;
	}

	for (int i = 2; i < argc; i++) {
		if (strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
			config_arg = argv[++i];
		}
		else if (strcmp(argv[i], "--cmds") == 0 && i + 1 < argc) {
			cmds_arg = argv[++i];
		}
		else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
			int secs = atoi(argv[++i]);
			if (secs > 0)
				st.watchdog_ms = (uint32_t)secs * 1000u;
		}
		else if (strcmp(argv[i], "-q") == 0 || strcmp(argv[i], "--quiet") == 0) {
			st.quiet = 1;
		}
		else if (strcmp(argv[i], "--debug") == 0) {
			st.debug = 1;
		}
		else if (strcmp(argv[i], "--fork") == 0) {
			fork_daemon = 1;
		}
		else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
			printf("splash-drm v%s\n", SPLASH_VERSION);
			return 0;
		}
		else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
			print_usage(argv[0]);
			return 0;
		}
	}

	if (drm_init(&st.drm, device) < 0) {
		if (!st.quiet)
			fprintf(stderr, "Failed to initialize DRM on %s\n", device);
		return 1;
	}

	if (!st.quiet)
		printf("splash-drm v%s: DRM %dx%d @ %dHz\n", SPLASH_VERSION,
		       st.drm.mode.hdisplay, st.drm.mode.vdisplay,
		       st.drm.mode.vrefresh);

	if (config_arg) {
		if (load_config(&st, config_arg) < 0 && !st.quiet)
			fprintf(stderr, "Warning: Failed to load config\n");
	}

	if (socket_init(&st) < 0) {
		if (!st.quiet)
			fprintf(stderr, "Failed to create abstract socket\n");
		drm_cleanup(&st.drm);
		font_unload_all();
		return 1;
	}

	install_signal_handlers();

	kbd_init(&st);

	st.running      = 1;
	st.needs_render = 1;
	st.frozen       = 0;

	if (cmds_arg) {
		if (process_startup_cmds(&st, cmds_arg) < 0 && !st.quiet)
			fprintf(stderr, "Warning: Failed to process startup commands\n");
	}

	/* Pick up any animation/spinner started by the startup commands, so
	 * the first poll already uses the short (RENDER_FPS) timeout. */
	st.anim_running     = anim_tick(&st, now_ms());
	st.last_activity_ms = now_ms();

	if (st.needs_render)
		render_frame(&st);

	if (fork_daemon && daemonize() < 0 && !st.quiet)
		fprintf(stderr, "Warning: daemonize failed, staying in foreground\n");
	else if (!fork_daemon)
		setsid();

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

		int ret = poll(fds, nfds, timeout);

		if (ret < 0) {
			if (errno == EINTR)
				continue;		/* signal: re-check the loop guard */
			perror("poll");
			break;
		}

		socket_process(&st, fds, nfds);
		kbd_process(&st);

		/* Watchdog: exit if no command arrived within the timeout, so a
		 * stuck boot script can never leave the splash up forever. */
		if (st.watchdog_ms > 0 && !st.frozen &&
		    now_ms() - st.last_activity_ms >= st.watchdog_ms) {
			if (st.debug)
				fprintf(stderr, "[debug] watchdog: idle timeout, exiting\n");
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

	if (st.debug && g_terminate)
		fprintf(stderr, "[debug] caught termination signal, shutting down\n");

	kbd_cleanup(&st);
	socket_cleanup(&st);
	clear_all_elements(&st);
	free_image(&st.bg_image);
	font_unload_all();
	drm_cleanup(&st.drm);

	if (!st.quiet)
		printf("splash-drm exited cleanly\n");
	return 0;
}
