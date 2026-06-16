# Changelog

All notable changes to splash-drm are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

## [Unreleased]

### New files

- **src/kbd.c** — evdev keyboard input handler (ESC toggle).
- **src/qr.c** — QR code element rendering (nayuki/QR-Code-generator vendored as `qrcodegen/` submodule).

### Added

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
  half-extent, which clips the tips of spokes rotated near 45°. The correct
  worst-case half-extent for a rotated capsule is `half_len + half_w`.

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
