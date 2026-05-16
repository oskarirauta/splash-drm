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
- **Anti-aliased rounded geometry** — rectangles and progress bars are
  rendered with a signed-distance field, so corners stay crisp at any size.
- **Gradient fills and soft drop shadows** for rectangles and progress bars.
- **High-quality image scaling** — nearest, bilinear, Mitchell bicubic, or
  Lanczos-3 resampling.
- **TrueType text** — sub-pixel positioning, kerning, UTF-8, multi-line text
  (`\n`), and an optional soft text shadow.
- **Animation engine** — time-based opacity fades, background crossfades, and
  an Apple-style rotating boot spinner.
- **Progress bars** — determinate (0–100%) or indeterminate (sweeping
  highlight), with six built-in colour themes or fully custom colours.
- **JSON control protocol** over an abstract UNIX socket.
- **Clean shutdown** on `SIGTERM`/`SIGINT`/`SIGHUP`, plus an optional
  inactivity watchdog so a stuck boot script can never leave the splash up
  forever.
- **Vendored dependencies** — cJSON and stb are git submodules; nothing
  external is needed at runtime.

## Building

### Requirements

- A C compiler (GCC or Clang).
- Linux kernel headers and libdrm headers — needed only to compile.

### Get the submodules

cJSON and stb are git submodules in the repository root. Clone with them, or
populate them afterwards:

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
| `--timeout <seconds>` | Exit if no command arrives for this long (watchdog). |
| `-q`, `--quiet` | Suppress all output. |
| `--debug` | Enable debug output. |
| `-v`, `--version` | Print the version and exit. |
| `-h`, `--help` | Print usage and exit. |

The configuration JSON currently carries font slots:

```json
{"fonts": [
  {"slot": 0, "path": "/usr/share/fonts/dejavu/DejaVuSans.ttf", "size": 24}
]}
```

Example:

```bash
splash-drm /dev/dri/card0 \
  --config /etc/splash-fonts.json \
  --cmds   /etc/splash-boot.json \
  --timeout 120
```

## Controlling the splash

`splash-ctl` sends one command — a single JSON object or an array of objects —
to the running daemon and prints the reply.

```bash
splash-ctl '<json-string>'
splash-ctl --file <json-file>
```

| Option | Description |
|--------|-------------|
| `--file` | Read the JSON from a file instead of the argument. |
| `--raw` | Print the raw JSON response. |
| `--debug` | Print debug output. |

```bash
splash-ctl '{"cmd":"image","path":"/boot/splash.png","crossfade":600}'
splash-ctl '{"cmd":"progress","id":0,"x":-1,"y":600,"w":400,"value":0.0}'
splash-ctl '[{"cmd":"update_progress","id":0,"value":1.0},{"cmd":"exit"}]'
```

## Command reference

Every command is a JSON object with a `cmd` field. Elements are addressed by
a caller-chosen integer `id`; sending a command for an existing `id` updates
that element.

| Command | Purpose |
|---------|---------|
| `clear` | Full reset: remove every element and the background, set the backdrop colour (`color`, default black). Loaded fonts are kept. |
| `bg_color` | Set the backdrop colour. |
| `image` | Set the background image (`path`, `mode`, `scale`, `filter`, `crossfade`). |
| `text` | Add or update a text element (`text`, `x`, `y`, `align`, `valign`, `color`, `font`, `size`, shadow, `opacity`). |
| `remove_text` | Remove a text element. |
| `rect` | Add or update a rectangle (`x`, `y`, `w`, `h`, `color`, `fill`, `radius`, border, gradient, shadow, `opacity`). |
| `remove_rect` | Remove a rectangle. |
| `overlay` | Add or update an image overlay (`path`, `x`, `y`, `w`, `h`, alignment, `filter`, `opacity`). |
| `remove_overlay` | Remove an image overlay. |
| `progress` | Create or reconfigure a progress bar (geometry, `style`, `value`, `indeterminate`, colours, shadow). |
| `update_progress` | Update only a bar's `value` (and optionally toggle `indeterminate`). |
| `hide_progress` | Hide a progress bar. |
| `animate` | Animate an element's opacity (`type`, `id`, `from`, `to`, `duration`, `easing`, `repeat`, `remove_on_end`). |
| `spinner` | Create / show / hide an Apple-style boot spinner (`x`, `y`, `radius`, `color`, `period`, `spokes`, `action`). |
| `suspend` / `resume` | Freeze or resume rendering. |
| `status` | Query the daemon state. |
| `ready` | Mark the daemon as ready. |
| `exit` | Tell the daemon to shut down cleanly. |

### Colours

Colours are hex strings: `#RGB`, `#RRGGBB`, or `#RRGGBBAA`.

### Progress bar styles

`style` selects a built-in colour theme; any explicit colour field switches
the bar to fully custom colours.

| Style | Theme |
|-------|-------|
| `0` | Blue |
| `1` | Green |
| `2` | Amber |
| `3` | Red |
| `4` | Purple |
| `5` | Cyan |

### Examples

```json
{"cmd": "text", "id": 0, "x": -1, "y": 400, "align": "center",
 "text": "Starting up\nplease wait", "color": "#ffffff", "shadow": true}
```

```json
{"cmd": "spinner", "id": 0, "x": -1, "y": -1, "radius": 40,
 "color": "#ffffff", "period": 900, "hidden": true}
```

```json
{"cmd": "animate", "type": "text", "id": 0, "to": 0,
 "duration": 400, "easing": "ease_out", "remove_on_end": true}
```

### Positioning

Every element defaults to the centre of the screen. `x` and `y` set an
anchor point — a negative value (the default) anchors to the screen centre
on that axis — and `align` / `valign` decide how the element sits relative
to that anchor (both default to centre/middle). For example, giving only
`"y": 40` places the element's vertical centre at y=40 while it stays
horizontally centred. `"align": "left", "valign": "top"` restores classic
top-left positioning.

For an `overlay`, giving only `w` or only `h` scales the image
proportionally — the missing dimension is derived from the image's aspect
ratio. Giving neither draws it at native size; giving both uses both.

## Initramfs integration

The daemon is started early from the initramfs and driven with `splash-ctl`.
Because the control socket lives in the abstract namespace, the same socket
keeps working across the switch to the real rootfs — there is nothing to move.

```sh
# Start the splash daemon (statically linked binary in the initramfs).
splash-drm /dev/dri/card0 \
    --config '{"fonts":[{"slot":0,"path":"/font.ttf","size":24}]}' \
    --cmds   '[{"cmd":"image","path":"/splash.png"}]' &

# Update progress as boot proceeds.
splash-ctl '{"cmd":"progress","id":0,"x":-1,"y":600,"w":400,"value":0.0}'
splash-ctl '{"cmd":"update_progress","id":0,"value":0.5}'

# Just before handing control to the real init, tear the splash down.
splash-ctl '{"cmd":"exit"}'
```

If the boot script might die without sending `exit`, start the daemon with
`--timeout <seconds>` so it removes itself after that much inactivity.

## Project structure

```
splash-drm/
├── include/
│   └── splash.h          # Shared declarations
├── src/
│   ├── main.c            # Entry point, event loop, signals, watchdog
│   ├── drm.c             # DRM/KMS interface (raw ioctls)
│   ├── render.c          # Drawing primitives and frame composition
│   ├── anim.c            # Time-based animation engine
│   ├── font.c            # TrueType text rendering (stb_truetype)
│   ├── image.c           # PNG loading (stb_image)
│   ├── elements.c        # Element allocation, lookup, cleanup
│   ├── socket.c          # Abstract UNIX socket server
│   ├── cmd.c             # JSON command parsing and dispatch
│   ├── utils.c           # Colour parsing, themes, clamping
│   └── splash-ctl.c      # JSON control client
├── cJSON/                # Submodule: JSON parser
├── stb/                  # Submodule: stb_image.h, stb_truetype.h
├── Makefile
└── README.md
```

## License

splash-drm is released under the MIT License. The vendored dependencies
keep their own licenses: cJSON is MIT-licensed, and stb is public domain.
