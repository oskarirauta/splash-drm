# ubus-progress — OpenWrt procd → splash-drm boot progress

An optional C companion for splash-drm on OpenWrt. It subscribes to procd's
`service` ubus object and, for every service that starts during boot, drives the
splash: it advances an N-of-total progress value and shows the current service
name, then runs a finish sequence when boot completes. Everything it sends is a
UCI-configured JSON template, so the splash layout is never hard-coded here.

It is the event-driven alternative to the shell helpers in `contrib/`. Use the
shell scripts for the full flexibility of plain `splash-ctl` calls; use this when
you want a small always-on daemon driven by real procd service events.

## How it works

- Subscribes to the procd `service` object; each `service.start` notification
  (`{"service":"<name>"}`) is one tick.
- The denominator is the boot-script count (`/etc/rc.d/S*`), or a fixed `total`.
- On each tick it fills and sends `tick_msg` (with `${value}`, the 0..1 fraction)
  and `status_msg` (with `${service}`, the service name). The `${...}` tokens are
  expanded with the same tree-level substitution `splash-ctl` uses, so
  `"value":"${value}"` becomes a JSON number.
- When the `done_service` starts (default `done`, i.e. OpenWrt's `/etc/rc.d/S95done`),
  or the count reaches `total`, it runs the finish: send the `done_msg` list in
  order, hold for `hold` seconds, then run `exit_action`.
- It connects straight to the splash-drm control socket (no `splash-ctl` fork per
  tick) and is silent if the daemon is not reachable.

## Build

```sh
make ubus-progress        # links libubus, libubox, libuci; reuses cJSON + subst
```

Needs the dev packages `libubus-dev`, `libubox-dev`, `libuci-dev` (or the OpenWrt
SDK staging dir). On the device these provide the headers; the `libuci.so` dev
symlink may be missing on some feeds — create it if the link step cannot find
`-luci`.

## Install

```sh
make install-ubus-progress             # binary, sample config, init script
# or by hand:
install -m0755 ubus-progress/ubus-progress      /usr/bin/ubus-progress
install -m0644 ubus-progress/splash.config      /etc/config/splash      # if absent
install -m0755 ubus-progress/splash-progress.init /etc/init.d/splash-progress
/etc/init.d/splash-progress enable
```

## Where it fits in the boot

- The splash **daemon** (`splash-drm`) should start as early as possible — from
  the initramfs, suspended, then resumed in preinit (a `/lib/preinit/*` hook
  running `splash-ctl --resume`). It needs only DRM, not ubus.
- This **bridge** starts at `START=00` in the main boot phase. It must run after
  `ubusd` is up (so it cannot live in preinit), and `S00` aligns its counting
  window with the `/etc/rc.d/S*` denominator. Earlier than that it would also
  count procd's internal early services, skewing the total.

## Configuration (`/etc/config/splash`)

Three sections; see `splash.config` for a ready-to-edit sample.

```
config global 'global'
	option enabled      '1'
	option total        'auto'        # 'auto' = count /etc/rc.d/S*, or a number
	option done_service 'done'        # service whose start means "boot done"

config progress 'progress'
	option tick_msg     '{"progress":{"id":0,"value":"${value}"}}'
	option status_msg   '{"console":{"id":0,"write":"starting ${service}"}}'

config finished 'finished'
	list   done_msg     '{"progress":{"id":0,"remove":true}}'
	list   done_msg     '{"text":{"id":99,"text":"System ready!"}}'
	option hold         '2'           # seconds to show the ready frame
	option exit_action  'fade'        # none | exit | fade
	option exit_timeout '1'           # fade/exit duration in seconds
	option exit_color   '#000000'
```

| Option | Section | Meaning |
|--------|---------|---------|
| `enabled` | global | `0` exits immediately, drawing nothing. |
| `total` | global | `auto` counts `/etc/rc.d/S*`; a number overrides it. |
| `done_service` | global | Service whose start triggers the finish (empty = only the `total` fallback). |
| `tick_msg` | progress | Message sent each tick; `${value}` is the 0..1 fraction. Empty = skip. |
| `status_msg` | progress | Message sent each tick; `${service}` is the service name. Empty = skip. |
| `done_msg` | finished | A `list` of messages sent in order at finish (the bar/console can be removed, a ready message shown, …). |
| `hold` | finished | Seconds to hold the finished frame before the exit action. |
| `exit_action` | finished | `none` (leave the splash up), `exit` (quit), or `fade` (fade out then quit). |
| `exit_timeout` | finished | Duration in seconds of the `exit`/`fade`. |
| `exit_color` | finished | Fade target colour. |

The templates control every element id, type and colour, so the same bridge
drives any scene. For anything the templates cannot express, fall back to the
shell helpers, which can issue arbitrary `splash-ctl` commands.
