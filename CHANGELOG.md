# Changelog

All notable changes to splash-drm are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [5.0.2] - 2026-06-26

### Added

- **`remove_at_full` on the progress element (cmd.c, anim.c, splash.h)** — a new
  per-bar boolean (default `false`): when set, the daemon deletes the bar once it
  reaches 100%. The full bar is shown for one frame before removal, and with
  `smooth` it waits for the fill tween to finish first. Default keeps the bar on
  screen at 100% — the disappearing is now opt-in rather than something a config
  has to undo. Documented in `REFERENCE.md` and `--help progress`.
- **ubus-progress gains `--version` / `--help` (ubus-progress.c)** — the bridge
  now answers `--version` (it tracks the daemon's version) and `--help`, which
  documents the template macros and the array form. With no argument it behaves
  exactly as before (load UCI, run the bridge).
- **More template macros for ubus-progress (ubus-progress.c)** — alongside
  `${value}` (0..1) and `${service}`, every per-tick template can now use
  `${percent}` (0..100 integer), `${count}` and `${total}`. `${percent}` lets a
  thin progress bar carry a separate `"${percent}%"` text readout placed
  anywhere in the scene, and makes "did it reach 100%?" visible at a glance.
- **ubus-progress `finished` gains a `delay` option (ubus-progress.c)** — seconds
  to hold the filled 100% frame before the `done_msg` sequence runs (default `0`
  = immediate). Mainly a debug aid to confirm the bar really reached the end.
- **ubus-progress `[start]` section (ubus-progress.c)** — a `list start_msg` run
  once when the bridge comes up (after it resumes the splash), before the first
  tick. It can create or replace any element — drop an initramfs "Preparing…"
  text, build the progress bar, restyle the whole scene — so the layout can live
  in UCI instead of the initramfs (no initramfs rebuild for layout tweaks).
- **`fade` on the `clear` system action (cmd.c, render.c, main.c, splash.h)** —
  `{"system":{"action":"clear","fade":true,"timeout":N}}` cross-fades into the
  cleared scene instead of cutting to it: the old frame is snapshotted and the
  elements dropped immediately (so anything sent next is built on the fresh
  scene), then the snapshot fades out over `timeout` seconds (`fade":true` to the
  clear colour, `"#rrggbb"` to that colour) and lifts to reveal what is
  underneath. Reuses the exit-fade renderer (the fade window is now a generic
  `fade_end_ms` shared by the exit and clear fades). Documented in `REFERENCE.md`
  and `--help clear`.

### Changed

- **ubus-progress per-tick config consolidated to a single `list status_msg`
  (ubus-progress.c)** — the former separate `tick_msg` and `status_msg` options
  are replaced by one `list status_msg`, every entry of which is sent on each
  tick. This matches the `list done_msg` / `list fail_msg` shape of the other
  sections and is strictly more capable (the macros were already global and a
  template may be an array), so the split bought nothing. **Config change:**
  rename `option tick_msg`/`option status_msg` to `list status_msg` entries; a
  single `option status_msg` still works.
- **Version string moved to `include/version.h`** — a tiny dependency-free
  header is now the single source of truth for `SPLASH_VERSION`. `splash.h`
  includes it for the daemon and control client, and `ubus-progress` includes it
  directly so it reports the same version without pulling in the DRM headers.
- **ubus-progress docs refreshed (README.md, splash.config)** — document the
  `list status_msg` consolidation, the `delay` option, the new macros, the array
  form, and that the bar now stays at 100% by default (the sample no longer
  removes it in the finished sequence).

### Fixed

- **ubus-progress no longer fails a normal boot whose `total` over-counts
  (ubus-progress.c)** — the auto `total` tends to be a service or two high (init
  scripts that look like procd services but never fire an observed
  `service.start`), so the count plateaued just short of `total`, the exact
  `count == total` finish never fired, and the idle watchdog ran `idle_action`
  ("fail") on an otherwise-complete boot. Now, once progress reaches
  `done_at_pct` (default 90%), a quiet of `settle_timeout` (default 5 s) is
  treated as *boot settled* and runs the **finished** sequence — closing the gap
  with no `done_event`/rc.local hook. The full `idle_timeout` + `idle_action`
  path now applies only to a genuine low-progress stall. Two new `[global]`
  options, `done_at_pct` and `settle_timeout`, tune the threshold and window.
- **`--help <cmd>` text caught up with schema v1 (usage.c)** — the per-command
  parameter lists had drifted behind the code. The `progress` and `arc` help now
  document `smooth` (animated value changes); `progress` also lists the `border`
  shorthand and the individual `shadow_dx`/`shadow_dy`/`shadow_blur`/
  `shadow_color` fields. The `system` namespace is no longer undocumented:
  `--help system` gives an overview, and `suspend`, `resume`, `clear` (with its
  `color`), `ready`, and `background` gained their own entries instead of falling
  through to "No detailed help available".

## [5.0.0] - 2026-06-24

A breaking redesign of the JSON message and configuration format ("schema v1").
The program is still in its alpha phase, so this is a hard switch with no
compatibility layer for the old `{"cmd":...}` grammar. The design rationale is
in `docs/SCHEMA-v1.md`; the full reference is `REFERENCE.md`.

### Changed

- **Single-key message format (cmd.c)** — A message is now a single-key JSON
  object whose key is the discriminator: `{"<type>": {...}}` for an element,
  `{"system": ...}` for daemon control, `{"background": ...}` for the backdrop.
  The `{"cmd": "<verb>"}` form is gone — e.g. `{"text":{"id":0,"text":"hi"}}`
  replaces `{"cmd":"text","id":0,"text":"hi"}`. Element handlers are reused
  unchanged; only the dispatch and the wire shape changed.
- **`--config` is a scene document (main.c)** — `--config` now loads one object
  holding `version`, `fonts`, `background`, and an `elements` array, so a single
  file describes the whole splash. This unifies the former fonts-only `--config`
  and the `--cmds` command array. `--check` validates the document's elements,
  not just its JSON syntax.
- **`exit` parameter renamed `delay` → `timeout` (cmd.c)** — sent as
  `{"system":{"action":"exit","timeout":N}}`, or the shorthand `{"system":"exit"}`.

### Added

- **`system` namespace (cmd.c)** — daemon lifecycle and queries are sent under
  `system`, as a string shorthand (`{"system":"status"}`) or an action object
  (`{"system":{"action":"query","type":"arc","id":0}}`): exit, suspend, resume,
  clear, status, version, running, ready, query.
- **`${NAME}` variable substitution (subst.c)** — both the daemon (`--config`)
  and `splash-ctl` expand `${NAME}` / `${NAME:-default}` tokens in JSON string
  values, supplied with `-D NAME=value`. A whole-value token takes its native
  JSON type; an undefined variable resolves to empty (never `0`). Lets a themed
  layout carry a dynamic value (e.g. a failure message) safely — the substituted
  value is JSON-escaped automatically.
- **stdin input for splash-ctl (splash-ctl.c)** — with no inline argument and a
  piped stdin, `splash-ctl` reads the message from stdin; `--file -` reads stdin
  explicitly. The inline argument and `--file <path>` forms still work.
- **Uniform `"hidden"` field (render.c, cmd.c, splash.h)** — every drawable
  element accepts `"hidden":true` to stop drawing it while keeping its slot and
  state; reveal it again with `"hidden":false`. Replaces the per-type
  `hide_progress` / `hide_arc` commands. (The spinner keeps its richer
  `action:"hide"/"show_animated"` mechanism.)
- **`animate` as an element field (cmd.c)** — an `"animate"` object on an
  element op tweens a property after the element's fields are applied, e.g.
  `{"text":{"id":0,"animate":{"to":0,"duration":400,"remove_on_end":true}}}`.
- **`background` operation (cmd.c)** — `{"background":"#rrggbb"}` sets the solid
  backdrop at runtime, mirroring the scene document's `background` field.
- **Fade-out on exit (cmd.c, render.c, main.c, splash-ctl.c)** —
  `{"system":{"action":"exit","timeout":N,"fade":true}}` fades the whole screen
  to black over the `timeout` window before exiting; `"fade":"#rrggbb"` fades to
  another colour. `splash-ctl --exit --timeout <s> --fade [#color]` builds it.
  The fade renders even while the daemon is suspended, so it always plays, and
  stays smooth at full resolution by fading a cached snapshot of the scene
  rather than re-blending the write-combining framebuffer each frame.
- **CSS-like `border` shorthand on progress (cmd.c)** — `border` accepts a
  number, or `false` (0) / `true` (1), as an alias for `border_width`.
- **`smooth` value animation on progress and arc (cmd.c, anim.c, splash.h)** — a
  `"smooth"` field makes `value` changes animate instead of jumping: `true`
  tweens over 300 ms, a number sets the duration in ms (`0`/`false` = instant).
  The fill eases (ease-in-out) from its current position to each new target, and
  a new `value` arriving mid-tween retargets the running animation rather than
  restarting it — so a stream of coarse updates (e.g. a boot bar stepping ~5% at
  a time) reads as one continuous glide. Reuses the existing tween engine; the
  first `value` and `indeterminate` mode are unaffected.
- **ubus-progress: OpenWrt procd boot-progress bridge (ubus-progress/)** — an
  optional C companion that subscribes to procd's `service` ubus object and
  drives the splash from real service.start events: it advances an N-of-total
  bar, shows the current service name, and on completion runs a finished
  sequence. The denominator counts only the procd-service `/etc/rc.d/S*` scripts
  that run after the bridge itself — the ones that actually fire `service.start`
  — so the bar reaches ~100% at the real end of boot instead of topping out
  early (one-shot setup scripts and anything before the bridge are excluded;
  recomputed every boot, so service changes are tracked). Completion is signalled
  by the count reaching `total` (primary), a ubus "service event" (`done_event`,
  an optional failsafe fired from /etc/rc.local), or a service.start name; an
  idle watchdog is the fallback, configurable to treat the quiet as a settled
  boot or a stall (a distinct fail screen). It also resumes
  the initramfs-suspended splash on start. Configured entirely through UCI
  (`/etc/config/splash`: global / progress / finished / fail sections) with JSON
  templates expanded via the shared `${NAME}` substitution; it connects straight
  to the control socket (no per-tick `splash-ctl` fork). Built with
  `make ubus-progress` (links libubus/libubox/libuci, reuses cJSON + subst); not
  part of `all`.

### Removed

- **`--cmds` flag (main.c)** — superseded by the scene document's `elements`.
- **Standalone verb commands (cmd.c)** — `update_progress`, `update_arc`,
  `hide_progress`, `hide_arc`, every `remove_*`, `console_write`, `bg_color`,
  and the standalone `animate` command. Their behaviour is now expressed through
  the merge model: re-send to update, `"remove":true` deletes, `"hidden":true`
  hides, the `"animate"` field tweens, the console `"write"` field appends, and
  `{"background":...}` sets the backdrop.
- **`borderless` progress field (cmd.c)** — redundant with `border_width:0` (and
  the new `border` shorthand).

### Fixed

- **Thin progress bars drew nothing (render.c)** — a very thin bar (e.g. a 1px
  line) rendered as just its track because the inset border consumed the whole
  height, collapsing the fill. The border is now capped so the fill always keeps
  at least 1px; a 1px bar shows as a clean line.
- **Zero-length lines left a faint stub (render.c)** — a zero-length butt-cap
  line has no area but still painted a faint blob; it now draws nothing (a round
  cap still degenerates to a dot).

## [4.0.2] - 2026-06-21

### Added

- **splash-ctl shorthand flags for the system commands (splash-ctl.c)** —
  `--status`, `--suspend`, `--resume` and `--exit` send the matching
  `{"cmd":...}`, and `--exit --timeout <s>` adds the `delay` (a delayed
  shutdown). Init scripts mix drawing commands (JSON) with control commands; the
  flags let the "system" actions stand out as plain options instead of being
  buried in JSON. `--status` prints the status JSON; `--timeout` is rejected
  unless paired with `--exit`.

- **`running` liveness probe (cmd.c, splash-ctl.c)** — A new `running` command
  and the `splash-ctl --running` flag report whether the daemon is up. Unlike
  every other command, this never surfaces a connection error: `--running`
  prints `running` / `not running` and exits `0` / `1`, and a raw
  `{"cmd":"running"}` always prints `{"running":true}` or `{"running":false}` —
  the client answers it locally so a script gets a boolean even when the daemon
  is down. A live daemon's own reply is `{"status":"ok","running":true}`.

- **Console `autofit` option (cmd.c, font.c)** — `{"cmd":"console", ...,
  "autofit":true}` snaps the console's drawn height down to a whole number of
  text rows. The box height rarely divides evenly by the line advance, and the
  leftover sub-line remainder shows up as a gap above (or, before the
  bottom-anchor fix, below) the text. With `autofit`, the box shrinks from the
  bottom to the nearest row boundary so a full log fills it edge to edge with
  symmetric padding and no leftover gap. A console therefore needs no empty space
  of its own — any decorative margin can be drawn with a `rect` behind it.

### Fixed

- **A bad `--config` / `--cmds` file failed silently (main.c)** — An unreadable
  file, unparseable JSON, or a startup command that errored were all logged at
  `LOGW` (warning), but the default log threshold is `ERROR`, so nothing reached
  the log: a malformed startup file simply produced no splash with no clue why.
  These are now `LOGE` (error), so they are visible at the default level (and in
  syslog/kmsg when forked). `--check` already reported them; normal startup now
  does too.

- **Console ignored its padding on the right edge (font.c)** — Text started at
  `con->x + padding` (so the left, top and bottom margins were honoured) but the
  compositing clip ran to the full right edge `con->x + con->w`, so a long
  enough line filled to the box edge with no right margin. The clip is now inset
  by `padding` on the right too, giving an equal margin on both sides; an
  over-long line is cropped at `con->w - padding`.

- **Out-of-bounds write rendering text in an over-sized box (font.c)** —
  `composite_coverage` clipped glyph pixels only to the caller's clip rectangle,
  not to the framebuffer. A console (or any text) whose box extended past the
  screen edge — easy to hit now that quoted pixel sizes like `"h": "900"` are
  honoured, e.g. a tall box on a small panel — composited glyphs at coordinates
  outside the buffer and segfaulted the daemon on the next render after a write.
  The clip rectangle is now clamped to the framebuffer bounds, so an over-sized
  or off-screen box is simply cropped instead of crashing.

- **Quoted pixel coordinates were silently ignored (utils.c)** — `get_coord`
  accepted a JSON number (`480`) or a percentage string (`"40%"`), but a plain
  numeric *string* (`"480"`) fell through to the default. Because the layout
  examples write every dimension as a string (`"h": "40%"`), quoting the pixel
  form alongside them (`"h": "480"`) looked natural yet was dropped — so an
  element's `x`/`y`/`w`/`h` appeared to honour only percentages. Strings are now
  parsed with `strtod`: a non-percentage numeric string is taken as pixels, so
  `480` and `"480"` behave identically. Percentages, negative-centre `-1`, and
  rejection of non-numeric junk are unchanged.

- **Console element wasted the bottom of the box and left a stray empty line
  (font.c)** — The console anchored its lines off `pad + max_vis * line_adv`,
  where `max_vis = inner_h / line_adv` truncates: the `inner_h % line_adv`
  remainder was left as blank space *below* the newest line, so the most recent
  line never sat flush at the bottom as documented ("an empty line at the end").
  Lines are now anchored directly off the box bottom (`con->y + con->h - pad`)
  and grow upward, so the newest line is flush against the bottom padding and
  any slack opens at the top instead. A full buffer now fills the box top to
  bottom with no gap.

### Changed

- **Console `max_lines` now defaults to the full capacity (cmd.c)** — The default
  was 32, but the backing `lines[]` ring is always `CONSOLE_MAX_LINES` (64)
  wide, so a tall console capped its visible history at half the box for no
  saving. The default is now `CONSOLE_MAX_LINES`; combined with the anchoring
  fix, a console fills completely once enough lines are written. The
  `examples/openwrt-boot.json` layout dropped its explicit `max_lines: 14`,
  which had been smaller than the box's visible capacity and so left the upper
  half permanently empty.

## [4.0.1] - 2026-06-20

### Added

- **`version` command — ask the running daemon its version (cmd.c, splash-ctl.c,
  usage.c)** — A new socket command, `{"cmd":"version"}`, returns the live
  daemon's compiled version (`{"status":"ok","version":"4.0.1"}`); the `status`
  reply now carries the same `version` field. The `splash-ctl --daemon-version`
  shortcut sends it and prints `splash-drm daemon v<x>` directly. This is a
  diagnostic for a real footgun: after upgrading the on-disk binaries, a stale
  initramfs can keep an *old* daemon alive, so newer commands "should work" but
  silently do nothing. Querying the live daemon over the socket — e.g. via SSH
  when the screen is blocked — reveals the mismatch. A daemon predating 4.0.1
  doesn't know the command and replies `"unknown command"`; `--daemon-version`
  translates that into a clear "older than 4.0.1, rebuild your initramfs" hint,
  so even the failure is diagnostic. Distinct from `splash-ctl --version` / `-V`,
  which reports the *client's* own build.

## [4.0.0] - 2026-06-20

### New files

- **src/kbd.c** — evdev keyboard input handler (ESC toggle).
- **src/qr.c** — QR code element rendering (nayuki/QR-Code-generator vendored as `qrcodegen/` submodule).
- **src/log.c / include/log.h** — unified logging module: an ordered severity
  threshold and a selectable output sink, both global, so any source file can
  log without threading `splash_state_t` around.

### Added

- **Automatic OpenWrt boot-progress companion (contrib/, Makefile)** — Added
  `contrib/openwrt-boot-progress-auto.sh`: a zero-touch hook for OpenWrt. Every
  init script flows through `/etc/rc.common` (re-sourced once per script), so
  sourcing this file there — right after its `shift 2` — makes every
  `/etc/rc.d/S*` boot script advance a deterministic *N-of-total*
  `progress`/`arc` bar (`id=0`) and log a `starting <svc>` line to a `console`
  (`id=0`), with no per-service edits. It self-limits to the boot window (each
  update is a `splash-ctl` call that fails silently once the daemon exits) and
  is fully configurable via environment variables. The previous manual helper
  script was renamed to `contrib/openwrt-boot-progress-simple.sh` (its
  `splash_progress_done` / `splash_status_text` / `splash_boot_done` helpers are
  unchanged, for hand-placed reporting). `make install` now also installs both
  companions to `$(PREFIX)/share/splash/`. A ready-made layout,
  `examples/openwrt-boot.json`, creates the matching `progress` and `console`
  (`id=0`) elements — a title, a scrolling `starting <svc>` log and a percentage
  bar — so the automatic hook is plug-and-play.

- **Overlay effects — rounded corners, rotation, tint (cmd.c / render.c)** — The
  `overlay` element gained three optional effect parameters: `radius` (rounded-
  corner clip radius in px, `0` = none), `angle` (rotation around the image's
  centre in degrees, `0` = none), and `tint` (a multiply tint whose alpha is the
  strength — omit or alpha `0` for no tint, a white tint is a no-op, a coloured
  tint multiplies the image toward that colour). All three sample bilinearly. On
  a merge update of an existing overlay the `path` is now optional — omit it to
  keep the current image and change only other fields (e.g. just the `angle`); a
  fresh overlay still requires `path`.

- **Text outline / stroke (cmd.c / font.c)** — The `text` element accepts
  optional `"outline": <pixels>` and `"outline_color": <color>` parameters. The
  outline is drawn under the glyphs as a stroke of the given width (`0` = none,
  the default; default colour opaque black), keeping text legible over busy or
  low-contrast backgrounds.

- **Console per-line colour (cmd.c)** — `console_write` accepts an optional
  `"color"` parameter that sets the colour of the line(s) being written. Each
  written line retains its own colour; lines written without `color` use the
  console's default colour, so a single console can mix colours — e.g.
  severity-coloured boot logs (green ok / yellow warn / red fail).

- **Generalised `animate` — position / size / colour (anim.c / cmd.c)** — The
  `animate` command, previously opacity-only, gained a `property` field selecting
  what to animate: `opacity` (float 0..1, as before), `x`/`y` (int pixels, move
  the element), `w`/`h` (int pixels, resize / grow), or `color` (per-channel
  colour lerp). `from`/`to` are interpreted per property (pixel integers for
  x/y/w/h, colours for `color`, 0..1 floats for opacity) and `from` defaults to
  the element's current value; `duration`, `easing`, `repeat`, and
  `remove_on_end` work the same for every property. Each element animates only
  the properties it has — most support `opacity`/`x`/`y`, `w`/`h` also work on
  rect/marquee/console/sprite/progress/overlay/ellipse, and `color` also on
  text/rect/ellipse/line/marquee/console/spinner/qr — and an unsupported
  property/element returns an error.

- **Path resolution (utils.c)** — `resolve_font_path()` and `resolve_image_path()`
  try the bare path first, then standard installation prefixes
  (`/usr/share/splash/fonts/`, `/usr/share/splash/`, `/usr/share/fonts/` for
  fonts; `/usr/share/splash/images/`, `/usr/share/splash/` for images). Absolute
  paths bypass the search entirely. `font.c` and `image.c` now call these before
  opening files, so configuration files can reference font and image names without
  hardcoded paths.

- **ESC keyboard toggle (kbd.c)** — The daemon now scans `/dev/input/event0..15`
  on startup for the first keyboard device that supports `KEY_ESC`. While the
  splash is running, pressing ESC blanks the screen (renders a single black
  frame); pressing ESC again restores it. The fd is opened `O_NONBLOCK` and
  polled alongside the control socket; if no keyboard is found the feature is
  silently skipped.

- **Gamma-correct text anti-aliasing (font.c)** — `composite_coverage` now
  blends glyph coverage in linear light (gamma ≈ 2 approximation:
  `linear = sRGB² / 255`, then `sRGB = sqrt(linear) · √255` via a 256-entry
  LUT built once). Previously, blending in sRGB space caused 50 % coverage to
  appear as ~21 % brightness; edges on small text looked thin and washed-out.

- **Text word wrap (cmd.c / font.c)** — The `text` command accepts optional
  `"wrap": true` and `"wrap_width": <pixels>` parameters. When enabled,
  `wrap_text()` pre-processes the string by inserting `\n` at word boundaries
  so that no rendered line exceeds `wrap_width` pixels (defaults to the buffer
  width). Hard newlines in the original text are preserved.

- **Console element — scrolling log area (cmd.c / font.c / elements.c)** — A
  new `console` element type provides a fixed-position rectangle that displays
  the most recent N lines of text pushed via `console_write`. Lines are stored
  in a circular buffer (capacity `max_lines`, capped at `CONSOLE_MAX_LINES=64`).
  When the buffer is not full, lines are anchored to the bottom of the area
  ("grow from bottom"). Background fill is optional (alpha 0 = transparent).
  Text is rendered with the same gamma-correct AA path as regular text elements,
  clipped to the console's own bounding rectangle. Commands: `console`,
  `console_write`, `remove_console`.

- **QR code element (src/qr.c, cmd.c)** — A new `qr` element type encodes any
  text payload as a QR Code (via the vendored nayuki/qrcodegen submodule) and
  renders it as a grid of filled rectangles. Configurable: pixel size per module
  (`module_px`; auto-sizes to ¼ of the shorter screen dimension when 0), quiet
  zone width (`border`, default 4), error correction level (`ecc`: 0=LOW,
  1=MEDIUM, 2=QUARTILE, 3=HIGH), dark/light module colours, alignment anchors,
  and opacity. Commands: `qr`, `remove_qr`.

- **Arc/circular progress bar (render.c, cmd.c)** — A new `arc` element type
  draws a circular progress indicator. Centre is at `(x, y)` (negative = screen
  centre). Configurable: `radius`, `thickness` (0 = radius/4), `start_angle`
  (default −90°/top), `sweep` (degrees, default 360), `value` (0.0–1.0),
  `bg_color` / `bar_color` / `bar_color2` + `bar_gradient`, `cap` (0=flat,
  1=round end caps), `show_percent` with font options, `indeterminate` spinning
  mode with `indet_period_ms`. Commands: `arc`, `update_arc`, `hide_arc`.

- **Ellipse / circle, line, and stepper elements (render.c, cmd.c, elements.c)**
  — Three new element types. `ellipse` (with `circle` as an alias) draws a filled
  disc or, with `thickness > 0`, an outline ring, centred at `(x, y)`; `radius`
  is shorthand for `rx`, and `ry` 0 mirrors `rx`. `line` draws a divider from
  `(x1, y1)` to `(x2, y2)` with configurable `thickness` and `cap` (0 = flat,
  1 = round). `stepper` renders a centred row of dots (`style` 0) or pills
  (`style` 1) as a boot-stage indicator: the first `current` of `count` steps are
  drawn "done" (`color_done`) and the rest "todo" (`color_todo`, filled or
  outlined via `thickness`), advanced as boot progresses with a merge update such
  as `{"cmd":"stepper","id":0,"current":3}`. All three honour the merge /
  `replace` / `remove` model, opacity `animate`, and `query`. Commands:
  `ellipse`, `circle`, `remove_ellipse`, `remove_circle`, `line`, `remove_line`,
  `stepper`, `remove_stepper`.

- **Marquee and sprite elements (render.c, cmd.c, elements.c)** — Two new
  element types. `marquee` scrolls a single line of text horizontally inside a
  clip box (default 400×40), repeating it with `gap` spacing for a seamless loop
  and clipping to the box; `speed` sets px/sec (`>0` left, `<0` right, `0`
  static) and, like `text`/`console`, it needs a loaded font slot. `sprite`
  cycles through a list of loaded images (`frames`, PNG/JPEG, up to 32) at `fps`
  frames per second — e.g. an animated logo or spinner — with `loop` false
  playing once and holding the last frame, and a merge update reloading `frames`
  only when supplied so omitting it keeps the running animation. Both honour the
  merge / `replace` / `remove` model, opacity `animate`, and `query`. Commands:
  `marquee`, `remove_marquee`, `sprite`, `remove_sprite`.

- **Percentage coordinates** — All element commands now accept percentage strings
  for positional and size fields (`"x": "50%"`, `"w": "80%"`, etc.). The value
  is resolved against the display width (for x/w) or height (for y/h) at command
  time. Integer values continue to work as before.

- **Named colours** — `parse_color()` now accepts CSS-style colour names in
  addition to hex strings: `transparent`, `black`, `white`, `red`, `green`,
  `blue`, `yellow`, `cyan`, `magenta`, `orange`, `gray`/`grey`,
  `darkgray`/`darkgrey`, `lightgray`/`lightgrey`.

- **`status` command extended** — Response now includes `"hidden"` (bool,
  reflects ESC-toggle state), `"width"` and `"height"` (display resolution).

- **`query` command** — New command that returns the current state of any named
  element: `{"cmd":"query","type":"arc","id":0}`. Returns position, value/text,
  opacity and type-specific fields. Useful for scripts that need to read current
  state before deciding whether to send an update.

- **Console and QR centering** — `console` and `qr` elements now support the
  same negative-coordinate centre shorthand as other elements (`"x": -1` places
  the element horizontally centred on screen).

- **Levelled logging (log.c / log.h)** — Five ordered levels —
  `ERROR < WARN < INFO < DEBUG < TRACE` — replace the old ad-hoc
  `if (st->debug) fprintf(...)` / `if (!st->quiet) ...` checks scattered across
  the daemon. A message is emitted only when its level is at or below the active
  threshold. New verbosity flags raise the threshold: `-v` → INFO, `-vv` → DEBUG,
  `-vvv` → TRACE; `--debug` is an alias for `-vv`. The default threshold is ERROR
  (silent but for genuine failures) and `-q`/`--quiet` drops below it for total
  silence. DRM bring-up (`drm.c`) — previously silent — now reports the chosen
  connector/CRTC/mode at DEBUG and traces each cold-boot connector-probe retry at
  TRACE; the control socket, keyboard handler and command dispatch likewise emit
  diagnostics at appropriate levels.

- **Selectable log sink (`--log <auto|stderr|syslog|kmsg>`)** — `auto` (default)
  keeps a foreground run on stderr, while a `--fork`ed daemon — whose stdio is
  redirected to `/dev/null` — falls back to the syslog socket (`/dev/log`) and
  then the kernel log (`/dev/kmsg`). The syslog sink is a direct `AF_UNIX`
  datagram writer (not libc `syslog()`), tagged `splash-drm[pid]` at the
  `daemon` facility, so the kmsg fallback stays under the daemon's control. A
  sink can also be forced explicitly, independent of forking; a forced sink that
  is unavailable degrades down the same chain rather than dropping messages.

- **Headless mode (`--headless`)** — Initialises DRM and allocates/renders the
  double buffers as usual, but never programs the CRTC: the initial scan-out,
  every `drm_flip()`, and the cleanup restore are all suppressed, so the existing
  console scan-out stays on screen. Rendering still happens off-screen (the full
  render path is exercised and command replies are unaffected), making this a
  safe way to watch logs and console output live during boot — the splash never
  covers them. Composes with `-vvv`/`--log` for live diagnostics. Because the
  CRTC is never touched it requires DRM master neither to start nor to run.

- **Config-check mode (`--check`)** — Validates a boot configuration
  (`--config`/`--cmds`: JSON structure, known commands, parameters) without
  opening DRM, the socket, or the referenced asset files, then exits non-zero on
  any structural problem. Lets a `boot.json` be checked on a build host before
  deploying to initramfs, where a typo is worst to discover. Referenced
  images/fonts are skipped (a global `g_validate_only` short-circuits
  `load_image()`/`font_load()`), so a config that is structurally fine passes
  even where the assets are not present.

- **Screenshot/dump mode (`--dump <file.png>`)** — Renders a single frame from
  `--config`/`--cmds` to a PNG file at the connected display's resolution, then
  exits. It runs neither the daemon loop nor the control socket (so it never
  collides with a running daemon's abstract name) and reuses the normal render
  path via `write_buffer_png()`. Composes with `--headless` to render entirely
  off-screen, leaving the live console untouched — a one-shot way to preview a
  boot configuration on a build host or capture golden frames for testing.

- **Build & packaging (Makefile)** — `make strip` (separate target, so
  packagers that strip themselves are not second-guessed), `make install` /
  `make uninstall` honouring `DESTDIR`/`PREFIX`, `-MMD -MP` header dependency
  tracking, and a CI workflow that now runs on push/PR and builds both the
  dynamic and the static (incl. musl/Alpine) targets.

- **Merge-update attributes and `remove_spinner`** — Element commands now accept
  `"replace": true` (reset the element to defaults before applying the supplied
  fields) and `"remove": true` (delete the element in place, like the matching
  `remove_*` command). A new `remove_spinner` command fills the one gap in the
  `remove_*` family (the spinner was previously only hideable via
  `action:"hide"`). A `get_bool` JSON helper now accepts `true`/`false` or `1`/`0`
  for every boolean field, so merge updates can preserve them.

### Changed

- **CLI: `-v` is now verbosity, not version** — `-v`/`-vv`/`-vvv` raise the log
  level (see above). The short flag for printing the version moved to **`-V`**;
  the long form `--version` is unchanged.

- **Boot-tracing folded into the TRACE level** — The always-on `/dev/kmsg`
  progress markers in `main.c` (added to pinpoint where the daemon was being
  torn down during the initramfs→rootfs `switch_root`) are gone. The handful
  that remain useful are now ordinary `LOGT(...)` calls, silent unless `-vvv`
  is given, instead of writing to the kernel log unconditionally.

- **`splash_state_t` slimmed** — The `quiet` and `debug` fields were removed;
  verbosity now lives in the logging module's global threshold. `load_config()`
  no longer takes a `splash_state_t *` (it only needed it for those flags).

- **`splash-ctl` version flag** — `-V`/`--version` is now the primary version
  flag (matching the daemon), with `-v` kept as a deprecated alias so existing
  scripts keep working.

- **README / REFERENCE synced** — The daemon options tables documented the old
  `-v, --version`; they now describe `-v/-vv/-vvv`, `-V`, `--log`, `--headless`
  and `--check`, plus prose on the logging levels/sinks. The undocumented
  spinner `show_animated`/`hide_animated` actions were added, the console `size`
  default corrected (slot's loaded size, not 14), and `src/log.c`/`include/log.h`
  added to the project tree.

- **Element commands merge on update** — Re-sending `text`, `rect`, `overlay`,
  `progress`, `arc`, `spinner`, `console` or `qr` with an existing `id` now
  MERGES: only the fields supplied change, and every omitted field keeps its
  current value. Previously each omitted field reset to its default — sending
  `{id:0, text:"world"}` recentred the text and dropped it to the default font.
  `replace: true` restores the old full-reset behavior, and a merge update no
  longer cancels a running opacity animation unless `opacity` is given. The
  `update_*` / `remove_*` / `hide_*` commands remain as explicit aliases.

### Performance

All renderer changes below were verified output-preserving: before/after
`--dump` of the same scene produces a byte-identical PNG (`md5sum`).

- **Arc bar drawn in a single pass (render.c)** — `draw_arc_bar` walked its
  bounding box twice, recomputing `sqrtf`/`atan2f` per pixel each time (once for
  the background ring, once for the fill). The passes are merged into one loop
  that computes the distance and angle once and applies the background then the
  fill, halving the transcendental math per covered pixel.

- **Background scaling cached across frames (render.c)** — A non-nearest
  background image (Lanczos/bicubic/bilinear) was re-resampled every frame, so a
  crossfade or any concurrent animation re-ran the full kernel each tick. The
  steady-state result is now cached (`resample_image_to` into `bg_cache`) and
  blitted 1:1 while valid; it is rebuilt only when the image, output size or
  filter changes (`bg_cache_dirty`). The live path still runs during a crossfade,
  where the source genuinely changes each frame.

- **Marquee text rasterised once, not per frame (font.c, cmd.c, elements.c)** — A
  scrolling `marquee` re-rasterised its entire text every frame just to re-tile
  the same bitmap at a new scroll offset. The rasterised coverage is now cached
  on the element and reused as it scrolls; it is rebuilt only when the text, font
  slot or size changes (`cov_dirty`, set by `cmd_marquee`) and freed on
  remove/replace/clear (`marquee_free_cache`). As the marquee animates
  continuously, this removes a full glyph rasterisation from every rendered frame.

- **Shared anchor / percent-label helpers (render.c, font.c, splash.h)** — The
  `x|y < 0 → centre` anchor maths (duplicated across text, progress, rect and
  ellipse) is now one `resolve_anchor()` inline, and the progress and arc
  percentage labels share one `draw_centered_percent()`. Pure de-duplication —
  byte-identical output, and the two percent labels can no longer drift apart in
  their rounding.

### Fixed (pre-release cleanup)

- **elements.c / cmd.c** — `progress_find()` was a file-local `static`
  function hidden inside `cmd.c` instead of living in `elements.c` alongside
  all other `*_find()` helpers. Moved to `elements.c` and declared in
  `splash.h` for consistency.

- **anim.c** — `anim_tick()` did not iterate `arc`, `console`, or `qr`
  element arrays. Opacity animations and the arc indeterminate flag were
  silently ignored for these three element types. All three are now ticked
  each frame.

- **cmd.c** — `cmd_animate` did not handle `"arc"`, `"console"`, or `"qr"`
  as the `type` field, returning `"unknown type"` for all three. Added the
  missing branches.

- **cmd.c** — `progress` and `arc` elements had `hide_*` commands (which
  deactivate but preserve the slot) but no `remove_*` commands (which fully
  clear the slot back to the pool). Added `remove_progress` and `remove_arc`
  for API consistency with `text`, `rect`, `overlay`, `console`, and `qr`.

### Fixed

- **Makefile** — `make static` straight after `make` was silently a no-op:
  the objects and binary already existed and only `-static` (a flag make cannot
  see) had changed, so a *dynamic* binary was shipped as "static". The static
  build now compiles into its own object directory (via a recursive sub-make) so
  it always relinks correctly, and `splash-ctl` now honours `LDFLAGS` so it, too,
  is actually static.

- **image.c / font.c** — A control-socket `image`/`overlay`/font path pointing
  at a FIFO or blocking device wedged the single-threaded daemon forever in a
  blocking `open()`/`fopen()`. Loads now use `O_NONBLOCK` and require a regular
  file (`fstat`/`S_ISREG`), and log the path and failure class.

- **cmd.c — documented parameters that silently did nothing** — `rect`
  `grad_dir`, `progress` `bar_gradient`, and the `progress` percent-label
  `font_slot`/`font_size` were read under different keys than the docs specified,
  and `arc` `show_percent`/`indeterminate` were parsed with `get_int` so a JSON
  boolean was dropped; `qr` `align`/`valign` ignored their string forms. All now
  accept the documented keys (with the old names kept as legacy aliases) and
  JSON `true`/`false` via a new `get_bool` helper.

- **cmd.c — `query` was case-sensitive while `animate` was not** — `query
  type=Text` returned "unknown type" though `animate type=Text` worked. Both use
  `strcasecmp` now.

- **drm.c** — Signed-integer shift UB in `find_crtc_for_encoder` (`1 << i` for
  `i` up to 63); guarded to `i < 32 && (1u << i)`.

- **render.c / utils.c / qr.c** — Out-of-range client values could reach
  undefined float→int conversions or integer overflow: the progress percent text
  cast, `get_coord`'s percentage parse (`"1e40%"`), and the QR `module_px *
  total` size. Each is now clamped/validated.

- **socket.c** — A short/failed reply `write()` on the non-blocking client fd
  was ignored, leaving the client blocked forever on a missing newline; the
  client is now dropped. The dead abstract-socket "stale → rebind" probe (an
  abstract name can never be stale) was removed.

- **main.c** — `--timeout` parsed with `atoi`, so a typo silently disabled the
  watchdog; it is validated with `strtol` and rejected. A failed non-fork
  `setsid()` is now logged instead of being invisible.

- **anim.c** — Ping-pong animations drifted over time because the period anchor
  was reset to the current timestamp (`start_ms = now`) instead of being
  stepped forward by exactly one period (`start_ms += duration_ms`). Render
  jitter of ~1–3 ms per tick accumulated across cycles, causing animations to
  gradually slow down.

- **render.c** — `draw_filled_rect` wrote straight-alpha ARGB colours directly
  to the framebuffer, but `blend_pixel` expects premultiplied values in the
  destination buffer. Semi-transparent rectangle fills (0 < alpha < 255)
  produced incorrect colours when composited with subsequent elements. Fixed by
  premultiplying the colour before writing it to the buffer.

- **render.c** — Spinner spoke bounding box used `max(half_len, half_w)` as the
  half-extent, which clips the tips of spokes rotated near 45°. It now uses the
  exact per-angle rotated half-extents of the capsule (`half_len·|cos θ| +
  half_w·|sin θ|` and the transpose, `+1 px` for the AA edge), which both ends
  the clipping and keeps the SDF-tested pixel count tight — a generous
  `half_len + half_w` square would test several times more pixels per spoke.

- **drm.c** — `calloc()` results in `_drmModeGetResources` and
  `_drmModeGetConnector` were passed directly to `memcpy()` without NULL checks.
  An out-of-memory condition would cause a NULL-pointer write (crash). Added
  `goto oom` paths that call the corresponding `_drmModeFree*` helpers and
  return NULL cleanly.

- **drm.c** — `_drmModeFreeResources` and `_drmModeFreeConnector` were defined
  after their first use in the same translation unit, causing a compile error
  with `-std=c99`. Added forward declarations before their first call site.

- **socket.c** — When `bind()` returned `EADDRINUSE`, the probe `socket()` call
  could fail (e.g. due to file-descriptor exhaustion). The original code wrapped
  the entire probe in `if (test_fd >= 0)` and silently fell through when the fd
  was invalid, reaching `listen()` on an unbound socket and producing a
  misleading "Invalid argument" error. The error path now returns -1 explicitly.
  Removed the no-op `SO_REUSEADDR` (has no effect on abstract UNIX sockets).

- **cmd.c** — Batch JSON commands (array of command objects) called
  `process_single_cmd` with the real `client_idx`, causing each command in the
  batch to send its own reply, followed by a batch-level summary — N+1 responses
  in total. `splash-ctl` reads only the first newline-terminated response and
  discards the rest. Per-command replies in a batch are now suppressed by
  passing `client_idx = -1`.

- **cmd.c** — The `overlay` command called `overlay_alloc()` (which sets
  `active = 1` immediately) and then returned early on a missing-path or
  failed-load error without clearing `active`. The allocated slot was permanently
  consumed, leaking it for the lifetime of the daemon. Error paths now reset
  `active = 0` for freshly allocated slots.

- **cmd.c** — Progress bar colour fields (`bg_color`, `bar_color`,
  `border_color`, `text_color`) were retrieved with `cJSON_GetObjectItem()` and
  their `->valuestring` was passed directly to `parse_color()`. If the JSON
  value was not a string (e.g. a number or boolean), `valuestring` is NULL and
  `parse_color(NULL)` silently returns white. Replaced with `get_color()` which
  uses a `cJSON_IsString()` guard and falls back to the current colour value.

## [3.2.0] - 2025-03-16

- Release v3.2.0

## [3.1.0] - 2025-03-09

- Release v3.1.0
