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
  or the count reaches `total`, it runs the **finished** sequence: send the
  `done_msg` list in order, hold for `hold` seconds, then run `exit_action`.
- If instead no `service.start` arrives for `idle_timeout` seconds, boot is
  treated as stalled and it runs the **fail** sequence (a distinct screen, since
  the done point was never reached). Either way the bridge then exits.
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
  the initramfs, where it is left **suspended** so the boot frame is held on
  screen. It needs only DRM, not ubus.
- This **bridge** starts at `START=00` in the main boot phase. It must run after
  `ubusd` is up (so it cannot live in preinit), and `S00` aligns its counting
  window with the `/etc/rc.d/S*` denominator. Earlier than that it would also
  count procd's internal early services, skewing the total.
- On start the bridge **resumes** the suspended splash itself (`resume` option),
  so it comes alive exactly when progress begins. The splash content does not
  change before the bridge takes over, so a separate preinit
  `splash-ctl --resume` hook is no longer needed (keep it only if you set
  `resume '0'`).

## Configuration (`/etc/config/splash`)

Four sections; see `splash.config` for a ready-to-edit sample.

```
config global 'global'
	option enabled      '1'
	option total        'auto'        # 'auto' = count /etc/rc.d/S*, or a number
	option done_service 'done'        # service whose start means "boot done"
	option idle_timeout '60'          # show the fail screen if idle for Ns (0 = off)

config progress 'progress'
	option tick_msg     '{"progress":{"id":0,"value":"${value}"}}'
	option status_msg   '{"console":{"id":0,"write":"starting ${service}"}}'

config finished 'finished'            # boot completed normally
	list   done_msg     '{"progress":{"id":0,"remove":true}}'
	list   done_msg     '{"text":{"id":99,"text":"System ready!"}}'
	option hold         '2'           # seconds to show the ready frame
	option exit_action  'fade'        # none | exit | fade
	option exit_timeout '1'           # fade/exit duration in seconds
	option exit_color   '#000000'

config fail 'fail'                    # boot stalled — a distinct screen
	list   fail_msg     '{"system":{"action":"clear","color":"#2a0000"}}'
	list   fail_msg     '{"text":{"id":0,"text":"Boot stalled"}}'
	option hold         '15'          # show the fail screen at least this long
	option exit_action  'none'        # none keeps it up; exit/fade quits
	option exit_timeout '0'
	option exit_color   '#2a0000'
```

Two outcomes: a normal completion runs the **finished** sequence, while a stall
(the idle watchdog firing with no `service.start` for `idle_timeout` seconds —
the done point was never reached) runs the **fail** sequence on a distinct
screen. Either way the bridge then exits; the splash daemon dies too only if the
matching `exit_action` is `exit`/`fade`.

| Option | Section | Meaning |
|--------|---------|---------|
| `enabled` | global | `0` exits immediately, drawing nothing. |
| `resume` | global | `1` (default) resumes the splash on start (it is usually started suspended from the initramfs), so no separate preinit resume hook is needed. `0` to leave that to something else. |
| `total` | global | `auto` counts `/etc/rc.d/S*`; a number overrides it. |
| `done_service` | global | Service whose start triggers the finish (empty = only the `total` fallback). |
| `idle_timeout` | global | Seconds of no `service.start` after which boot is treated as stalled and the fail sequence runs. `0` disables the watchdog. Reset on every tick. |
| `tick_msg` | progress | Message sent each tick; `${value}` is the 0..1 fraction. Empty = skip. |
| `status_msg` | progress | Message sent each tick; `${service}` is the service name. Empty = skip. |
| `done_msg` | finished | A `list` of messages sent in order on success (remove the bar/console, show a ready message, …). |
| `fail_msg` | fail | A `list` of messages sent in order on a stall; `${idle}` is the stall time in seconds. |
| `hold` | finished / fail | Seconds to hold that frame before the exit action (fail defaults to 15 so it can be read). |
| `exit_action` | finished / fail | `none` (leave the splash up), `exit` (quit), or `fade` (fade out then quit). Per section. |
| `exit_timeout` | finished / fail | Duration in seconds of the `exit`/`fade`. |
| `exit_color` | finished / fail | Fade target colour. |

The templates control every element id, type and colour, so the same bridge
drives any scene. For anything the templates cannot express, fall back to the
shell helpers, which can issue arbitrary `splash-ctl` commands.
