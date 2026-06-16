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
  optional word wrap, and a soft text shadow.
- **Animation engine** — time-based opacity fades, background crossfades, and
  an Apple-style rotating boot spinner.
- **Progress bars** — horizontal (determinate or indeterminate), with six
  built-in colour themes or fully custom colours.
- **Arc/circular progress bar** — circular ring indicator with optional round
  end caps, sweep gradient, and indeterminate spinning mode.
- **Scrolling log console** — a fixed-position text area that displays the
  most recent N lines pushed via `console_write`. Grows from the bottom.
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
- Linux kernel headers and libdrm headers — needed only to compile.

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
| `--config <file\|json>` | Load configuration (font slots). Inline JSON or a file path. |
| `--cmds <file\|json>` | Run an initial batch of commands on startup. |
| `--fork` | Fork to background; parent exits immediately. **Recommended for initramfs use** — the child calls `setsid()` after forking, so `switch_root` cannot deliver `SIGHUP` to the daemon. Without this flag, no `&` is needed but session detachment is best-effort only. |
| `--timeout <seconds>` | Exit if no command arrives for this long (watchdog). |
| `-q`, `--quiet` | Suppress all output. |
| `--debug` | Enable debug output. |
| `-v`, `--version` | Print the version and exit. |
| `-h`, `--help` | Print usage summary and exit. |
| `--help <cmd>` | Show all parameters for a specific command. |

The configuration JSON carries font slots and optional startup defaults:

```json
{"fonts": [
  {"slot": 0, "path": "DejaVuSans.ttf", "size": 24},
  {"slot": 1, "path": "DejaVuSans-Bold.ttf", "size": 18}
]}
```

Font and image paths are searched in order:
1. The path as given (absolute or relative to the working directory)
2. `/usr/share/splash/fonts/<name>` (fonts) / `/usr/share/splash/images/<name>` (images)
3. `/usr/share/splash/<name>`
4. `/usr/share/fonts/<name>` (fonts only)

## Controlling the splash

`splash-ctl` sends one command — a JSON object or array of objects — to the
running daemon and prints the reply.

```bash
splash-ctl '<json>'
splash-ctl --file <json-file>
```

```bash
splash-ctl '{"cmd":"image","path":"splash.png","crossfade":600}'
splash-ctl '{"cmd":"update_arc","id":0,"value":0.75}'
splash-ctl '[{"cmd":"update_arc","id":0,"value":1.0},{"cmd":"exit"}]'
```

## Command reference

Every command is a JSON object with a `cmd` field. Use `splash-drm --help <cmd>`
for a quick parameter list at the terminal, or see
[`REFERENCE.md`](REFERENCE.md) for the full documentation including parameter
types, default values, and detailed descriptions.

Elements are addressed by a caller-chosen integer `id`; sending a command for
an existing `id` updates that element in place.

### Lifecycle

| Command | Purpose |
|---------|---------|
| `clear` | Full reset: remove every element and the background. Loaded fonts are kept. |
| `suspend` / `resume` | Freeze or resume rendering. |
| `status` | Query daemon state. Returns `state`, `ready`, `hidden`, `width`, `height`. |
| `ready` | Mark the daemon as ready (for external polling). |
| `exit` | Tell the daemon to shut down cleanly. Optional `delay` (seconds) keeps it rendering before exiting. |

### Background

| Command | Purpose |
|---------|---------|
| `bg_color` | Set the solid backdrop colour (`color`). |
| `image` | Set the background image (`path`, `mode`, `scale`, `filter`, `crossfade`). |

### Elements

| Command | Purpose |
|---------|---------|
| `text` | Add or update a text label (`text`, `x`, `y`, `align`, `valign`, `color`, `font`, `size`, `wrap`, `wrap_width`, `shadow`, `shadow_dx`, `shadow_dy`, `shadow_blur`, `shadow_color`, `opacity`). |
| `remove_text` | Remove a text element by `id`. |
| `rect` | Add or update a rectangle (`x`, `y`, `w`, `h`, `color`, `fill`, `radius`, `border_color`, `border_width`, `grad_color`, `grad_dir`, `shadow`, `shadow_dx`, `shadow_dy`, `shadow_blur`, `shadow_color`, `opacity`). |
| `remove_rect` | Remove a rectangle by `id`. |
| `overlay` | Add or update an image overlay (`path`, `x`, `y`, `w`, `h`, `align`, `valign`, `filter`, `opacity`). |
| `remove_overlay` | Remove an image overlay by `id`. |
| `progress` | Create or reconfigure a horizontal progress bar (`x`, `y`, `w`, `h`, `align`, `valign`, `value`, `style`, `bg_color`, `bar_color`, `bar_color2`, `bar_gradient`, `border_color`, `text_color`, `borderless`, `border_width`, `radius`, `font_slot`, `font_size`, `show_percent`, `indeterminate`, `indet_period_ms`, `shadow`, `opacity`). |
| `update_progress` | Update a bar's `value` and optionally toggle `indeterminate`. |
| `hide_progress` | Hide a progress bar (preserves slot configuration). |
| `remove_progress` | Remove a progress bar and free the slot. |
| `arc` | Create or reconfigure a circular progress bar (`x`, `y`, `radius`, `thickness`, `value`, `start_angle`, `sweep`, `cap`, `bg_color`, `bar_color`, `bar_color2`, `bar_gradient`, `font_slot`, `font_size`, `text_color`, `show_percent`, `indeterminate`, `indet_period_ms`, `opacity`). |
| `update_arc` | Update an arc's `value` and optionally `bar_color`. |
| `hide_arc` | Hide an arc element (preserves slot configuration). |
| `remove_arc` | Remove an arc element and free the slot. |
| `spinner` | Create / show / hide an Apple-style rotating spinner (`x`, `y`, `radius`, `spokes`, `color`, `period`, `action`, `opacity`). |
| `console` | Create or reconfigure a scrolling log area (`x`, `y`, `w`, `h`, `font_slot`, `size`, `color`, `bg_color`, `padding`, `max_lines`, `opacity`). |
| `console_write` | Push one or more lines of text into a console (`id`, `text`; `\n` splits into separate lines). |
| `remove_console` | Remove a console element by `id`. |
| `qr` | Create or update a QR code element (`text`, `x`, `y`, `align`, `valign`, `module_px`, `border`, `ecc`, `color`, `bg_color`, `opacity`). |
| `remove_qr` | Remove a QR code element by `id`. |
| `animate` | Animate an element's opacity (`type`, `id`, `from`, `to`, `duration`, `easing`, `repeat`, `remove_on_end`). |
| `query` | Read back current element state (`type`, `id`). Returns position, `value`/`text`, `opacity`, and type-specific fields. |

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

- An integer pixel value: `960`
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
    --config /etc/splash/config.json \
    --cmds   /etc/splash/boot.json \
    --timeout 120

# Drive progress from boot scripts.
splash-ctl '{"cmd":"update_arc","id":0,"value":0.5}'

# Suspend before switch_root so the daemon is idle during the transition.
splash-ctl '{"cmd":"suspend"}'
exec switch_root /sysroot /sbin/init
```

In the new root's init scripts:

```sh
# Resume the daemon that survived switch_root.
splash-ctl '{"cmd":"resume"}'

# Query current value before updating (avoids going backwards).
val=$(splash-ctl '{"cmd":"query","type":"arc","id":0}')

# Show a final message and exit after 3 seconds.
splash-ctl '{"cmd":"text","id":99,"text":"System ready","size":48}'
splash-ctl '{"cmd":"exit","delay":3}'
```

See `examples/` for complete configuration examples and `contrib/` for
integration scripts.

### OpenWrt / Alpine Linux boot progress

See [`contrib/openwrt-boot-progress.sh`](contrib/openwrt-boot-progress.sh)
for a drop-in hook that drives the arc progress bar from the OpenWrt
`/etc/rc.common` init framework. The same approach works on Alpine Linux
with minor adjustments for OpenRC.

## Project structure

```
splash-drm/
├── include/
│   └── splash.h              # Shared declarations and types
├── src/
│   ├── main.c                # Entry point, event loop, signals, watchdog
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
├── examples/
│   ├── simple-boot.json      # Minimal boot splash (background + arc + text)
│   └── full-featured.json    # Showcase of all element types
├── contrib/
│   └── openwrt-boot-progress.sh  # OpenWrt rc.common progress hook
├── CHANGELOG.md
├── Makefile
└── README.md
```

## License

splash-drm is released under the MIT License. Vendored dependencies keep their
own licenses: cJSON is MIT, stb is public domain, qrcodegen is MIT.
