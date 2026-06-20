/*
 * kbd.c - Keyboard input via Linux evdev.
 *
 * Scans /dev/input/event0..15 at startup for the first device that reports
 * EV_KEY capability and supports KEY_ESC. The fd is opened O_NONBLOCK so
 * it can be added to the main poll() loop without ever blocking.
 *
 * Pressing ESC toggles the splash visibility: the back buffer is blanked
 * and flipped so the kernel console becomes readable, and pressing ESC again
 * resumes normal rendering. No VT switching is attempted - the approach is
 * deliberately minimal so it works in initramfs before udev is fully up.
 */

#include "splash.h"
#include <linux/input.h>

/* ========================================================================
 * Keyboard Initialisation
 * ======================================================================== */

void kbd_init(splash_state_t *st) {
	st->kbd_fd = -1;

	for (int i = 0; i < 16; i++) {
		char path[32];
		snprintf(path, sizeof(path), "/dev/input/event%d", i);

		int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
		if (fd < 0)
			continue;

		/* Require EV_KEY support. */
		uint8_t evbits[(EV_MAX + 7) / 8];
		memset(evbits, 0, sizeof(evbits));
		if (ioctl(fd, EVIOCGBIT(0, sizeof(evbits)), evbits) < 0 ||
		    !(evbits[EV_KEY / 8] & (1u << (EV_KEY % 8)))) {
			close(fd);
			continue;
		}

		/* Require KEY_ESC capability. */
		uint8_t keybits[(KEY_MAX + 7) / 8];
		memset(keybits, 0, sizeof(keybits));
		if (ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybits)), keybits) < 0 ||
		    !(keybits[KEY_ESC / 8] & (1u << (KEY_ESC % 8)))) {
			close(fd);
			continue;
		}

		st->kbd_fd = fd;

		if (LOG_DEBUG <= log_threshold) {
			char name[64] = "(unknown)";
			ioctl(fd, EVIOCGNAME(sizeof(name) - 1), name);
			LOGD("keyboard: %s (%s)", path, name);
		}
		return;
	}

	LOGD("keyboard: no suitable input device found");
}

/* ========================================================================
 * Cleanup
 * ======================================================================== */

void kbd_cleanup(splash_state_t *st) {
	if (st->kbd_fd >= 0) {
		close(st->kbd_fd);
		st->kbd_fd = -1;
	}
}

/* ========================================================================
 * Event Processing
 * ======================================================================== */

/*
 * Drain all pending evdev events. Called after poll() signals POLLIN on
 * kbd_fd (or unconditionally - the non-blocking read returns immediately
 * when the queue is empty).
 *
 * ESC key-down toggles st->hidden and sets needs_render so the main loop
 * blanks or restores the framebuffer on the next iteration.
 */
void kbd_process(splash_state_t *st) {
	if (st->kbd_fd < 0)
		return;

	struct input_event ev;
	while (read(st->kbd_fd, &ev, sizeof(ev)) == (ssize_t)sizeof(ev)) {
		if (ev.type != EV_KEY || ev.code != KEY_ESC || ev.value != 1)
			continue;

		st->hidden       = !st->hidden;
		st->needs_render = 1;

		LOGD("ESC: splash %s", st->hidden ? "hidden" : "visible");
	}
}
