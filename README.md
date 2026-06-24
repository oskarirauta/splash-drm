# splash-drm

A self-contained DRM/KMS bootsplash daemon for Linux. It renders directly
through DRM/KMS kernel ioctls — no legacy framebuffer, and no libdrm at
runtime (libdrm headers are used only at build time). It is designed to
start from an initramfs and survive the switch to the real rootfs: the
control channel is an abstract UNIX socket, so there is no filesystem
entry to relocate.

Two programs are built:

- **`splash-drm`** — the daemon that owns the display.
- **`splash-ctl`** — a small client that sends JSON commands to the daemon.

## Features

- **Direct DRM/KMS** via kernel ioctls; statically linkable, links only `libm`.
- **Anti-aliased rounded geometry** — rectangles and progress bars rendered
  with a signed-distance field; crisp at any size.
- **Gamma-correct text anti-aliasing** — glyph coverage blended in linear
  light for sharp edges at all font sizes.
- **Gradient fills and soft drop shadows** for rectangles and progress bars.
- **High-quality image scaling** — nearest, bilinear, Mitchell bicubic, or
  Lanczos-3 resampling.
- **TrueType text** — sub-pixel positioning, kerning, UTF-8, multi-line (`\n`),
  optional word wrap, a soft text shadow, and an optional glyph outline/stroke
  for legibility over busy backgrounds.
- **Animation engine** — time-based property animations (opacity, position,
  size, and per-channel colour), background crossfades, and an Apple-style
  rotating boot spinner.
- **Progress bars** — horizontal (determinate or indeterminate), with six
  built-in colour themes or fully custom colours.
- **Arc/circular progress bar** — circular ring indicator with optional round
  end caps, sweep gradient, and indeterminate spinning mode.
- **Scrolling log console** — a fixed-position text area that displays the
  most recent N lines pushed via the console `write` field. Grows from the bottom.
- **QR code element** — encodes any text payload and renders it as a pixel
  grid (nayuki/qrcodegen, vendored as a submodule).
- **ESC keyboard toggle** — pressing ESC blanks the splash; pressing it again
  restores it. Useful for peeking at boot messages.
- **Path resolution** — font and image names are searched in standard
  installation prefixes so configuration files can omit the full path.
- **Percentage coordinates** — any `x`, `y`, `w`, or `h` field accepts a
  percentage string (`"50%"`) resolved against the display dimension.
- **Named colours** — `"white"`, `"transparent"`, `"red"`, etc. in addition
  to `#RRGGBBAA` hex strings.
- **JSON control protocol** over an abstract UNIX socket (`\0splash-drm`).
- **`query` command** — scripts can read back current element state (value,
  position, text, …) before deciding whether to send an update.
- **Clean shutdown** on `SIGTERM`/`SIGINT`, plus an optional inactivity
  watchdog. `SIGHUP` is intentionally ignored so the daemon survives shell
  exit during `switch_root`.
- **Vendored dependencies** — cJSON, stb, and qrcodegen are git submodules;
  nothing external is needed at runtime.

## Building

### Requirements

- A C compiler (GCC or Clang).
- Linux kernel UAPI headers (`linux-headers` on Alpine, `linux-libc-dev` on
  Debian/Ubuntu) — needed only to compile. The libdrm headers are vendored
  under `drm/`, so no `libdrm-dev` is required.

### Get the submodules

```bash
git clone --recurse-submodules <repo-url>
# or, in an existing clone:
make submodules            # == git submodule update --init --recursive
```

### Build

```bash
make            # dynamic build (development)
make static     # statically linked build (for initramfs)
make clean      # remove build artifacts
```

## Running the daemon

```bash
splash-drm <drm_device> [options]
```

| Option | Description |
|--------|-------------|
| `--config <file\|json>` | Load the scene document (fonts, background, elements). Inline JSON or a file path. |
| `-D NAME=value` | Define a substitution variable; `${NAME}` in the config is expanded before parsing. Repeatable. |
| `--fork` | Fork to background; parent exits immediately. **Recommended for initramfs use** — the child calls `setsid()` after forking, so `switch_root` cannot deliver `SIGHUP` to the daemon. Without this flag, no `&` is needed but session detachment is best-effort only. |
| `--timeout <seconds>` | Exit if no message arrives for this long (watchdog). |
| `--headless` | Initialise DRM but never program the CRTC, so the console stays visible; renders off-screen only. |
| `--check` | Validate `--config` without opening DRM, then exit (`0` = ok). |
| `--dump <file.png>` | Render one frame from `--config` to a PNG file, then exit (no daemon loop, no control socket). Pair with `--headless` to render without touching the live console. |
| `-q`, `--quiet` | Silence all output (even errors). |
| `-v`, `-vv`, `-vvv` | Log threshold: info / debug / trace (each includes the lower levels, so warnings already show at `-v`). |
| `--debug` | Alias for `-vv` (debug verbosity). |
| `--log <target>` | Log sink: `auto` (default), `stderr`, `syslog`, or `kmsg`. |
| `-V`, `--version` | Print the version and exit. |
| `-h`, `--help` | Print usage summary and exit. |
| `--help <type>` | Show all parameters for an element type or system action. |

### Logging

The daemon has five log levels — `ERROR < WARN < INFO < DEBUG < TRACE`. The
threshold shows every message at or below the chosen level, so each level
includes the ones beneath it. The default (no flag) is `ERROR`, so a normal run
is silent except for genuine failures. `-v` raises the threshold to `INFO`,
`-vv` to `DEBUG`, and `-vvv` to `TRACE`; `--debug` is an alias for `-vv`. There
is no separate flag for `WARN` because `INFO` already includes it — **warnings
are visible from `-v` upward.** The flags are additive (`-v -vv` sums to `-vvv`
= trace). `-q` / `--quiet` silences everything, including errors.

The `--log` sink controls where messages go. A foreground run logs to
`stderr`. A `--fork`ed daemon has its stdio redirected to `/dev/null`, so it
automatically falls back to the syslog socket `/dev/log` and then to the
kernel log `/dev/kmsg`. `--log auto` (the default) follows that order; `stderr`,
`syslog`, or `kmsg` force a specific sink. Syslog output is tagged
`splash-drm[pid]` at the `daemon` facility.

### Headless and config-check modes

`--headless` initialises DRM and reads the connected display's mode (so it
needs a connected display to size its buffers) but never programs the CRTC.
The kernel console and live log output therefore stay visible — useful during
the initramfs→rootfs boot, where you can watch boot messages without the
splash covering them. Rendering still happens off-screen, so the render path
is exercised even though nothing reaches the panel.

`--check` validates a boot configuration (`--config` JSON — structure,
known commands, and parameters) on a build host without opening DRM or touching
the referenced asset files. It exits non-zero on structural problems, so it is
a quick way to catch a typo before deploying to an initramfs.

`--dump <file.png>` renders a single frame from `--config` to a PNG file
and exits, using the connected display's resolution for the image. It does not
run the daemon loop or open the control socket, so it never collides with a
running daemon. Pair it with `--headless` to render without touching the live
console — useful to preview a boot configuration or to capture golden frames for
testing.

The scene document carries the fonts, the backdrop, and the elements drawn at
startup:

```json
{
  "version": 1,
  "fonts": [
    {"slot": 0, "path": "DejaVuSans.ttf", "size": 24},
    {"slot": 1, "path": "DejaVuSans-Bold.ttf", "size": 18}
  ],
  "background": "#0b0e14",
  "elements": [
    {"image": {"path": "splash.png", "mode": 1}},
    {"text":  {"id": 0, "y": "70%", "text": "Booting…"}}
  ]
}
```

Font and image paths are searched in order:
1. The path as given (absolute or relative to the working directory)
2. `/usr/share/splash/fonts/<name>` (fonts) / `/usr/share/splash/images/<name>` (images)
3. `/usr/share/splash/<name>`
4. `/usr/share/fonts/<name>` (fonts only)

## Controlling the splash

`splash-ctl` sends one message — a single-key object or an array of them — to
the running daemon and prints the reply. The JSON comes from an inline argument,
`--file <path>` (`-` for stdin), or a piped stdin:

```bash
splash-ctl '<json>'
splash-ctl --file scene.json
echo '<json>' | splash-ctl                       # piped stdin, no flag
```

```bash
splash-ctl '{"image":{"path":"splash.png","crossfade":600}}'
splash-ctl '{"arc":{"id":0,"value":0.75}}'
splash-ctl '[{"arc":{"id":0,"value":1.0}},{"system":"exit"}]'
splash-ctl '{"text":{"id":1,"text":"${MSG}"}}' -D MSG="Boot failed"
```

Useful client flags: `--raw` prints the daemon's raw JSON reply, `--file` reads
the JSON from a file (`-` = stdin), `-D NAME=value` expands `${NAME}` tokens,
`-V` / `--version` prints **this client's** build, and `--daemon-version` asks
the **running daemon** what version it is:

```bash
splash-ctl --daemon-version      # → splash-drm daemon v4.0.1
```

The system operations also have shorthand flags, so init scripts can keep them
visually distinct from the drawing messages instead of burying them in JSON:

```bash
splash-ctl --status              # print the daemon status JSON
splash-ctl --running             # "running" / "not running", exit 0 / 1
splash-ctl --suspend             # {"system":"suspend"} — freeze rendering
splash-ctl --resume              # {"system":"resume"}  — resume rendering
splash-ctl --exit                # {"system":"exit"}    — shut the daemon down
splash-ctl --exit --timeout 3    # exit after 3 s
```

`--running` is a liveness probe that never fails with a connection error: it
prints `running` / `not running` and exits `0` / `1`. The raw
`{"system":"running"}` form always prints `{"running":true}` or
`{"running":false}` instead, so a script gets a boolean even when the daemon is
down:

```bash
if splash-ctl --running >/dev/null; then echo "splash is up"; fi
```

This catches a stale initramfs: if you upgrade splash-drm on disk but forget to
rebuild the initramfs, the old daemon keeps running and newer commands silently
do nothing. SSH in (when the screen is blocked) and ask the live daemon. A
daemon older than 4.0.1 doesn't know the command and says so — which is itself
the answer. The version is also included in the `status` reply.

## Command reference

Every message is a single-key JSON object — `{"<type>": {…}}` for an element,
`{"system": …}` for daemon control, `{"background": …}` for the backdrop. Use
`splash-drm --help <type>` for a quick parameter list at the terminal, or see
[`REFERENCE.md`](REFERENCE.md) for the full documentation including parameter
types, default values, and detailed descriptions.

Elements are addressed by a caller-chosen integer `id`; sending a message for
an existing `id` updates that element in place.

### Creating, updating, hiding, removing (merge updates)

Every element type uses one message shape for create / update / hide / animate /
remove, keyed by an `id`:

- **New `id`** — the element is created. Any field you omit takes its default.
- **Existing `id` (merge)** — only the fields you supply change; every omitted
  field keeps its current value. After
  `{"text":{"id":0,"x":100,"y":50,"color":"red","size":32,"text":"hello"}}`,
  sending `{"text":{"id":0,"text":"world"}}` redraws "world" at the same
  position, colour, and size.
- **`"replace": true`** — reset the element to its defaults first, then apply
  the supplied fields.
- **`"remove": true`** — delete the element and free its slot.
- **`"hidden": true`** — stop drawing the element but keep its slot and state;
  reveal it again with `"hidden": false`.
- **`"animate": {…}`** — tween a property after the fields are applied (see
  the [Animation](REFERENCE.md#animation) section in the reference).

A merge update does **not** cancel a running opacity animation unless you supply
`opacity` explicitly, so you can change e.g. a progress `value` in the middle of
a fade.

### System operations

Sent under the `system` key — a string shorthand or `{"action": "…", …}`:

| `{"system": …}` | Purpose |
|-----------------|---------|
| `"clear"` (or `{"action":"clear","color":…}`) | Full reset: remove every element, set the backdrop. Loaded fonts are kept. |
| `"suspend"` / `"resume"` | Freeze or resume rendering. |
| `"status"` | Daemon state: `state`, `ready`, `hidden`, `width`, `height`. |
| `"version"` | Running daemon version. Since 4.0.1; older daemons reply `unknown command`. |
| `"running"` | Liveness probe; a live daemon returns `{"status":"ok","running":true}`. Backs `splash-ctl --running`, which never fails on a connection error. |
| `"ready"` | Mark the daemon as ready (for external polling). |
| `{"action":"exit","timeout":N}` | Shut down cleanly. Optional `timeout` (seconds) keeps rendering first; add `"fade":true` (or `"fade":"#rrggbb"`) to fade the screen out over that window. |
| `{"action":"query","type":T,"id":N}` | Read back element state — position, `value`/`text`, `opacity`, and type-specific fields. |

### Backdrop

`{"background": "#rrggbb"}` (or `{"background": {"color": "…"}}`) sets the solid
backdrop drawn before the elements. For a background **image**, use the `image`
element below.

### Element types

Each is a single-key message `{"<type>": {…}}`. On an existing `id` the fields
merge; `"remove"` / `"hidden"` / `"replace"` / `"animate"` work as above. See
[`REFERENCE.md`](REFERENCE.md) for full per-type parameter tables.

| Type | Purpose |
|------|---------|
| `image` | Full-screen background image (`path`, `mode`, `scale`, `filter`, `crossfade`). |
| `text` | Text label (`text`, `x`, `y`, `align`, `valign`, `color`, `font`, `size`, `wrap`, `wrap_width`, `shadow…`, `outline`, `outline_color`, `opacity`). |
| `rect` | Rectangle (`x`, `y`, `w`, `h`, `color`, `fill`, `radius`, `border_color`, `border_width`, `grad_color`, `grad_dir`, `shadow…`, `opacity`). |
| `ellipse` / `circle` | Ellipse or circle (`x`, `y`, `radius`, `rx`, `ry`, `thickness`, `color`, `opacity`). `circle` is an alias. |
| `line` | Line / divider (`x1`, `y1`, `x2`, `y2`, `thickness`, `cap`, `color`, `opacity`). |
| `stepper` | Step / boot-stage indicator (`x`, `y`, `align`, `valign`, `count`, `current`, `style`, `size`, `length`, `gap`, `thickness`, `color_done`, `color_todo`, `opacity`). |
| `marquee` | Horizontally-scrolling text (`x`, `y`, `w`, `h`, `text`, `font`, `size`, `color`, `speed`, `gap`, `opacity`). |
| `sprite` | Frame animation of cycled images (`frames`, `x`, `y`, `w`, `h`, `align`, `valign`, `filter`, `fps`, `loop`, `opacity`). |
| `overlay` | Image overlay (`path`, `x`, `y`, `w`, `h`, `align`, `valign`, `filter`, `radius`, `angle`, `tint`, `opacity`). `radius` clips rounded corners, `angle` rotates, `tint` multiplies. |
| `progress` | Horizontal progress bar (`x`, `y`, `w`, `h`, `value`, `style`, `bg_color`, `bar_color`, `bar_color2`, `bar_gradient`, `border_color`, `text_color`, `border_width`, `radius`, `font_slot`, `font_size`, `show_percent`, `indeterminate`, `shadow`, `opacity`). Re-send with `value` to update. |
| `arc` | Circular progress bar (`x`, `y`, `radius`, `thickness`, `value`, `start_angle`, `sweep`, `cap`, `bg_color`, `bar_color`, `bar_color2`, `bar_gradient`, `font_slot`, `font_size`, `text_color`, `show_percent`, `indeterminate`, `opacity`). |
| `spinner` | Apple-style rotating spinner (`x`, `y`, `radius`, `spokes`, `color`, `period`, `action`, `opacity`). |
| `console` | Scrolling log area (`x`, `y`, `w`, `h`, `font_slot`, `size`, `color`, `bg_color`, `padding`, `max_lines`, `opacity`); a `"write":"line"` field appends text (`\n` splits lines). |
| `qr` | QR code (`text`, `x`, `y`, `align`, `valign`, `module_px`, `border`, `ecc`, `color`, `bg_color`, `opacity`). |

### Colours

Colours accept CSS-style names or hex strings:

| Form | Example |
|------|---------|
| Named | `"white"`, `"black"`, `"transparent"`, `"red"`, `"green"`, `"blue"`, `"yellow"`, `"cyan"`, `"magenta"`, `"orange"`, `"gray"` / `"grey"`, `"darkgray"`, `"lightgray"` |
| 3-digit hex | `"#fff"` |
| 6-digit hex | `"#ffffff"` (fully opaque) |
| 8-digit hex | `"#ffffff80"` (with alpha) |

Alpha is in the last byte (AA); 00 = fully transparent, ff = fully opaque.

### Coordinates and sizes

`x`, `y`, `w`, `h` accept:

- An integer pixel value, bare or quoted: `960` or `"960"` (handy when the
  surrounding fields are percentage strings)
- A percentage string resolved against the display dimension: `"50%"` → half
  the screen width (for `x`/`w`) or height (for `y`/`h`)
- A negative integer for `x` or `y`: anchors to the screen centre on that
  axis (e.g. `"x": -1` → horizontally centred regardless of resolution)

`align` and `valign` control how the element sits relative to its anchor:

| Value | `align` | `valign` |
|-------|---------|----------|
| `0` or `"left"` / `"top"` | left edge at x | top edge at y |
| `1` or `"center"` / `"middle"` | centred on x | centred on y |
| `2` or `"right"` / `"bottom"` | right edge at x | bottom edge at y |

Most elements default to `align=center`, `valign=middle`, so supplying only
`"y": 600` places the element centred horizontally at y=600.

### Progress bar styles

`style` selects a built-in colour theme:

| Style | Theme |
|-------|-------|
| `0` | Blue |
| `1` | Green |
| `2` | Amber |
| `3` | Red |
| `4` | Purple |
| `5` | Cyan |

## Initramfs integration

`splash-drm` is built statically (`make static`) and placed in the initramfs.
The abstract socket survives the switch-root, so `splash-ctl` on the real rootfs
can keep driving the same daemon instance.

```sh
# Start the daemon (--fork detaches cleanly; no & needed).
splash-drm /dev/dri/card0 --fork \
    --config /etc/splash/scene.json \
    --timeout 120

# Drive progress from boot scripts.
splash-ctl '{"arc":{"id":0,"value":0.5}}'

# Suspend before switch_root so the daemon is idle during the transition.
splash-ctl '{"system":"suspend"}'
exec switch_root /sysroot /sbin/init
```

The `--config` scene document (`scene.json`) declares the elements every later
`splash-ctl` call drives, so it must create the ones those calls target. For the
OpenWrt auto-progress hook (the boot-progress section below) that means a
`progress` and a `console` element, both `id=0` —
[`examples/openwrt-boot.json`](examples/openwrt-boot.json) is ready to use as
that document.

In the new root's init scripts:

```sh
# Resume the daemon that survived switch_root.
splash-ctl '{"system":"resume"}'

# Query current value before updating (avoids going backwards).
val=$(splash-ctl '{"system":{"action":"query","type":"arc","id":0}}')

# Show a final message and exit after 3 seconds.
splash-ctl '{"text":{"id":99,"text":"System ready","size":48}}'
splash-ctl '{"system":{"action":"exit","timeout":3}}'
```

See `examples/` for complete configuration examples and `contrib/` for
integration scripts.

### OpenWrt / Alpine Linux boot progress

Two drop-in companions in `contrib/` drive the progress bar from the OpenWrt
`/etc/rc.common` init framework (both are installed to `/usr/share/splash/`
by `make install`):

- **[`openwrt-boot-progress-auto.sh`](contrib/openwrt-boot-progress-auto.sh)**
  — *zero-touch*. Every OpenWrt init script flows through `/etc/rc.common`,
  which is re-sourced once per script, so sourcing this file there — right
  after its `shift 2` — makes every `/etc/rc.d/S*` boot script advance a
  deterministic *N-of-total* bar and log a `starting <svc>` line, with no
  per-service edits:

  ```sh
  [ -f /usr/share/splash/openwrt-boot-progress-auto.sh ] && \
      . /usr/share/splash/openwrt-boot-progress-auto.sh
  ```

  Your scene document must declare a `progress` (or `arc`) element with
  `id=0` and a `console` element with `id=0`. The target ids, bar type,
  colour and matched actions are overridable via environment variables
  documented at the top of the script. It self-limits: each update is a
  `splash-ctl` call that fails silently once the daemon exits, so it does
  nothing after boot (e.g. a later `/etc/init.d/network restart`).

- **[`openwrt-boot-progress-simple.sh`](contrib/openwrt-boot-progress-simple.sh)**
  — *manual*. Provides `splash_progress_done` / `splash_status_text` /
  `splash_boot_done` helpers you call yourself, for full control over when
  progress advances and what text is shown.

A ready-made layout for the automatic companion ships in
[`examples/openwrt-boot.json`](examples/openwrt-boot.json): it creates exactly
the `progress` (`id=0`) and `console` (`id=0`) elements the script drives — a
dark screen with a title, a scrolling `starting <svc>` log and a percentage
bar — so

```sh
splash-drm /dev/dri/card0 \
    --config /usr/share/splash/examples/openwrt-boot.json
```

plus the one-line `rc.common` hook above is a complete, no-configuration boot
splash. The document's `fonts` section loads slot 0; adjust the path to a font
present on your system.

The same approach works on Alpine Linux with minor adjustments for OpenRC.

## Project structure

```
splash-drm/
├── include/
│   ├── splash.h              # Shared declarations and types
│   └── log.h                 # Unified logging: levels + sinks
├── src/
│   ├── main.c                # Entry point, event loop, signals, watchdog
│   ├── log.c                 # Unified logging: levels + sinks (stderr/syslog/kmsg)
│   ├── drm.c                 # DRM/KMS interface (raw ioctls, no libdrm runtime)
│   ├── render.c              # Drawing primitives and frame composition
│   ├── anim.c                # Time-based animation engine
│   ├── font.c                # TrueType text and console rendering (stb_truetype)
│   ├── image.c               # PNG/JPEG loading (stb_image)
│   ├── elements.c            # Element allocation, lookup, cleanup
│   ├── socket.c              # Abstract UNIX socket server
│   ├── cmd.c                 # JSON command parsing and dispatch
│   ├── utils.c               # Colour parsing, path resolution, themes
│   ├── kbd.c                 # evdev keyboard input (ESC toggle)
│   ├── qr.c                  # QR code rendering
│   ├── usage.c               # Shared command help text (splash-drm + splash-ctl)
│   └── splash-ctl.c          # JSON control client
├── cJSON/                    # Submodule: lightweight JSON parser
├── stb/                      # Submodule: stb_image.h, stb_truetype.h
├── qrcodegen/                # Submodule: nayuki/QR-Code-generator
├── drm/                      # Vendored libdrm UAPI headers (MIT) — no libdrm-dev needed
├── examples/
│   ├── simple-boot.json      # Minimal boot splash (background + arc + text)
│   ├── openwrt-boot.json     # Plug-and-play layout for the OpenWrt auto hook
│   ├── full-featured.json    # Showcase of all element types
│   └── initramfs-init.sh     # Example initramfs /init (USB-aware root wait)
├── contrib/
│   ├── openwrt-boot-progress-auto.sh    # OpenWrt rc.common hook (automatic)
│   └── openwrt-boot-progress-simple.sh  # OpenWrt rc.common helpers (manual)
├── CHANGELOG.md
├── Makefile
└── README.md
```

## License

splash-drm is released under the MIT License. Vendored dependencies keep their
own (MIT-compatible) licenses: cJSON is MIT, stb is public domain, qrcodegen is
MIT, and the libdrm UAPI headers under `drm/` are MIT/X11 (each header carries
its own notice). The `drm/` headers are vendored so the build needs no system
libdrm / `libdrm-dev` — see [`drm/README.md`](drm/README.md).
