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
- The denominator is the number of procd-service boot scripts that run after the
  bridge itself (the ones that actually fire `service.start`), or a fixed
  `total`. One-shot setup scripts (fstab, sysctl, done, …) and anything before
  the bridge are excluded, so the bar reaches ~100% at the real end of boot.
- On each tick it sends every `status_msg` template (a UCI `list`, so add as many
  as you like — the bar, a `${percent}%` readout, a console line, …). They share
  one global macro set: `${value}` (0..1 fraction), `${percent}` (0..100
  integer), `${count}` / `${total}` (the N-of-M position) and `${service}` (the
  name that just started). The `${...}` tokens are expanded with the same
  tree-level substitution `splash-ctl` uses, so `"value":"${value}"` becomes a
  JSON number while `"${percent}%"` stays a string. Each list entry may itself be
  a single command object **or a JSON array of them**, which the daemon runs as
  one batch.
- When boot is done — signalled by the count reaching `total`, the `done_event`,
  the `done_service` starting, or progress going quiet near the end (see next
  point) — it runs the **finished** sequence: fill the bar to 100%, optionally
  hold that frame for `delay` seconds, send the `done_msg` list in order, hold
  for `hold` seconds, then run `exit_action`.
- The auto `total` tends to over-count by a service or two (init scripts that
  look like procd services but never fire an observed `service.start`), so the
  count usually plateaus just short of `total` and the exact 100% finish never
  fires on its own. To close that gap **without** a `done_event`/rc.local hook,
  once progress reaches `done_at_pct` (default 90%) a quiet of `settle_timeout`
  (default 5 s) counts as *boot settled* and runs the **finished** sequence. A
  quiet period while progress is still **below** `done_at_pct` is the real
  **stall** case: there the bridge waits the full `idle_timeout` and then runs
  `idle_action` (`fail` for a distinct stall screen, or `finished`). Either way
  the bridge then exits.
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

### Wire up a "boot done" signal (optional)

You normally **do not** need this: the `done_at_pct` / `settle_timeout` heuristic
already finishes a normal boot on its own once progress goes quiet near the end,
without any hook. procd has no native boot-complete event over the `service`
object, and `/etc/rc.local` is a plain script (not a procd service), so there is
nothing for the bridge to subscribe to for "boot done" — which is why this hook
fires the signal explicitly.

Use it only if you want completion the **instant** boot ends rather than after
`settle_timeout`, or your `total` over-counts so badly that progress never
reaches `done_at_pct`. Add to `/etc/rc.local`, before its `exit 0`:

```sh
ubus call service event '{"type":"boot.done","data":{}}'
```

The bridge watches for it (`done_event`, default `boot.done`) and finishes the
instant it arrives, regardless of the count.

## Where it fits in the boot

- The splash **daemon** (`splash-drm`) should start as early as possible — from
  the initramfs, where it is left **suspended** so the boot frame is held on
  screen. It needs only DRM, not ubus.
- This **bridge** starts at `START=00` in the main boot phase. It must run after
  `ubusd` is up (so it cannot live in preinit). Starting early maximises the
  window it observes; the denominator counts only the procd services that run
  *after* the bridge's own script, so wherever it starts, the bar still reaches
  ~100% (just over a smaller set of services if it starts late).
- On start the bridge **resumes** the suspended splash itself (`resume` option),
  so it comes alive exactly when progress begins. The splash content does not
  change before the bridge takes over, so a separate preinit
  `splash-ctl --resume` hook is no longer needed (keep it only if you set
  `resume '0'`).

## Configuration (`/etc/config/splash`)

Up to five sections; see `splash.config` for a ready-to-edit sample.

```
config global 'global'
	option enabled      '1'
	option total        'auto'        # 'auto' = count procd-service S## scripts
	option done_event   'boot.done'   # ubus "service event" type = boot done
	option done_service 'done'        # OR a service.start name = boot done
	option idle_timeout '60'          # stall window (s) while progress is low
	option idle_action  'fail'        # finished (settled) | fail (stalled)
	option done_at_pct  '90'          # >= this % + quiet = boot done, not a stall
	option settle_timeout '5'         # quiet window (s) once near-complete

config start 'start'                 # run once when the bridge comes up
	list   start_msg    '{"text":{"id":1,"remove":true}}'    # drop "Preparing…"
	list   start_msg    '{"progress":{"id":0,"y":"80%","value":"0"}}'

config progress 'progress'           # per-tick templates (a list, all sent)
	list   status_msg   '{"progress":{"id":0,"value":"${value}"}}'
	list   status_msg   '{"console":{"id":0,"write":"starting ${service}"}}'

config finished 'finished'            # boot completed normally
	list   done_msg     '{"text":{"id":99,"text":"System ready!"}}'
	option delay        '0'           # hold the 100% frame before done_msg (debug)
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

Completion is detected by the count reaching `total` (the primary signal), the
`done_event` (a ubus service event — an optional failsafe, see above), or the
`done_service` (a `service.start` name); any of these runs the **finished**
sequence. The idle watchdog (no `service.start` for `idle_timeout` seconds) is
the fallback: with `idle_action 'finished'` it treats the quiet as a settled
boot, with `idle_action 'fail'` as a stall that runs the **fail** sequence on a
distinct screen. The finished sequence always fills the bar to 100% first, so it
completes cleanly even if the count landed a service short. Either way the bridge
then exits; the splash daemon dies too only if that section's `exit_action` is
`exit`/`fade`.

| Option | Section | Meaning |
|--------|---------|---------|
| `start_msg` | start | A `list` of templates sent once when the bridge comes up (after it resumes the splash), before the first tick. Create or replace any element here — drop an initramfs "Preparing…" text, build the progress bar, restyle the scene — so the whole layout can live in UCI instead of the initramfs. Omit the section to leave the initramfs scene unchanged. |
| `enabled` | global | `0` exits immediately, drawing nothing. |
| `resume` | global | `1` (default) resumes the splash on start (it is usually started suspended from the initramfs), so no separate preinit resume hook is needed. `0` to leave that to something else. |
| `total` | global | `auto` counts the procd-service `/etc/rc.d/S*` scripts that run after the bridge (the ones that fire `service.start`); a number overrides it. Recomputed every boot, so adding/removing services is tracked automatically. |
| `self_script` | global | The bridge's own `/etc/init.d` name (default `splash-progress`), used to exclude itself and everything before it from the `auto` total. Set only if you renamed the init script. |
| `done_event` | global | ubus `service event` type that means boot is done — an optional failsafe (fire it from `/etc/rc.local`; see above). Empty disables. |
| `done_service` | global | A `service.start` name that means boot is done (empty = none). |
| `idle_timeout` | global | Seconds of no `service.start`, **while progress is still below `done_at_pct`**, after which `idle_action` runs. `0` disables the watchdog. Reset on every tick. |
| `idle_action` | global | What the watchdog does on a low-progress stall: `finished` (boot settled — the safe default) or `fail` (a distinct stall screen). Does not apply once near-complete (see `done_at_pct`), where a quiet always finishes. |
| `done_at_pct` | global | Progress percentage (default `90`) at/above which a quiet period counts as *boot done* rather than a stall — closing the gap left by an over-counted `total`, with no `done_event`/rc.local hook. Once reached, the watchdog window shortens to `settle_timeout`. Set higher to be stricter, lower if your `total` over-counts by more. |
| `settle_timeout` | global | Seconds of quiet, once progress is at/above `done_at_pct`, after which the **finished** sequence runs (default `5`). Lower = snappier completion, but risks finishing during a long gap between two late services. |
| `status_msg` | progress | A `list` of templates, all sent on every tick (the bar, a `${percent}%` readout, a console line, …). Each entry is one command object or a JSON array. Empty = skip. A single `option status_msg` also works. |
| `done_msg` | finished | A `list` of messages sent in order on success (show a ready message, optionally remove the bar/console, …). |
| `fail_msg` | fail | A `list` of messages sent in order on a stall; `${idle}` is the stall time in seconds. |
| `delay` | finished | Seconds to hold the filled 100% frame before the `done_msg` sequence runs. `0` (default) runs it immediately. Mainly a debug aid to confirm the bar reached the end. |
| `hold` | finished / fail | Seconds to hold that frame before the exit action (fail defaults to 15 so it can be read). |
| `exit_action` | finished / fail | `none` (leave the splash up), `exit` (quit), or `fade` (fade out then quit). Per section. |
| `exit_timeout` | finished / fail | Duration in seconds of the `exit`/`fade`. |
| `exit_color` | finished / fail | Fade target colour. |

### Template macros

Expanded in any template (`${NAME:-fallback}` is also supported):

| Macro | Where | Value |
|-------|-------|-------|
| `${value}` | per-tick | Progress as a `0..1` fraction (becomes a JSON number). |
| `${percent}` | per-tick | Progress as a `0..100` integer. Put `"${percent}%"` in a separate `text` element to show a readout anywhere — handy when the bar itself is only a pixel or two tall. The finished sequence forces it to `100`. |
| `${count}` | per-tick | Services advanced so far (N). |
| `${total}` | per-tick | The denominator (M), so `"${count}/${total}"`. |
| `${service}` | per-tick | Name of the service that just started. |
| `${idle}` | fail | Stall time in seconds. |

Every `status_msg` entry shares one global macro set, so any macro works in any
entry — split the bar, a percent readout and a console line across as many list
entries as you like.

The templates control every element id, type and colour, so the same bridge
drives any scene. For anything the templates cannot express, fall back to the
shell helpers, which can issue arbitrary `splash-ctl` commands.

## Command line

The bridge is normally started as the `splash-progress` procd service and takes
no runtime arguments. Two informational flags are available:

```sh
ubus-progress --version    # prints the version (tracks splash-drm), then exits
ubus-progress --help       # usage, the template macros, and the array form
```
