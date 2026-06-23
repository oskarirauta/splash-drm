# splash-drm Command Reference

Detailed parameter documentation for every command accepted by the daemon.
For a quick overview see `README.md`; for per-command help at the terminal
run `splash-drm --help <cmd>`.

---

## Common concepts

### Element IDs

Every element command requires an `"id"` (integer). The id is chosen freely
by the caller. Sending a command with an existing id reconfigures that element
in place; sending one with a new id creates it. There is no auto-increment —
the caller is responsible for tracking ids.

### Creating vs. updating elements (merge updates)

This applies to every element command that takes an `id`: `text`, `rect`,
`overlay`, `progress`, `arc`, `spinner`, `console`, and `qr`.

| Case | Behaviour |
|------|-----------|
| **new `id`** | The element is created. Any omitted field takes its default. |
| **existing `id` (merge)** | Only the fields you supply are changed; every omitted field keeps its **current** value. |
| **existing `id` + `"replace": true`** | The element is reset to its defaults first, then the supplied fields are applied — so every field you do not supply returns to its default. |
| **existing `id` + `"remove": true`** | The element is deleted in place, exactly like the matching `remove_*` command. |

For example, after creating

```json
{"cmd":"text","id":0,"x":100,"y":50,"color":"red","size":32,"text":"hello"}
```

sending `{"cmd":"text","id":0,"text":"world"}` redraws "world" at the same
position, colour, and size — only the text changes. Sending
`{"cmd":"text","id":0,"text":"world","replace":true}` instead resets the label
to its defaults (screen centre, white, default font) before applying the new
text.

A merge update does **not** cancel a running opacity animation unless you supply
`opacity` explicitly. This lets you change a field such as a progress `value`
in the middle of a fade without interrupting the animation.

The dedicated `update_progress`, `update_arc`, `hide_progress`, `hide_arc`, and
`remove_*` commands still work and are kept as explicit aliases for callers that
prefer them.

### Coordinates and sizes (`x`, `y`, `w`, `h`)

| Value | Meaning |
|-------|---------|
| positive integer | exact pixel position / size |
| `"50%"` (string) | percentage of the display width (for `x`/`w`) or height (for `y`/`h`) |
| negative integer (`-1`) | **screen centre** on that axis — the element is placed at `(screen_dim - element_dim) / 2` regardless of resolution |

Most elements default `x` and `y` to `-1` (centred). Percentage strings are
resolved at the moment the command is received, so a layout written for one
resolution scales correctly on any display.

### `align` and `valign`

Control which part of the element the `x`/`y` anchor refers to.

| Value | `align` | `valign` |
|-------|---------|----------|
| `0` or `"left"` / `"top"` | left edge at `x` | top edge at `y` |
| `1` or `"center"` / `"middle"` | element centred on `x` | element centred on `y` |
| `2` or `"right"` / `"bottom"` | right edge at `x` | bottom edge at `y` |

Example: `"x": -1, "align": "center"` centres the element horizontally
regardless of its width.

### `opacity`

Float in the range `0.0` (fully transparent) to `1.0` (fully opaque).
Applied as a master alpha multiplier on top of any per-pixel alpha already
in the element's colour or image. Default: `1.0`.

### `color` and colour fields

See the [Colours](#colours) appendix for accepted formats. Short summary:
named colours (`"white"`, `"transparent"`, …), `"#RGB"`, `"#RRGGBB"`, or
`"#RRGGBBAA"`. The last two hex digits are alpha (00 = transparent, ff = opaque).

### Drop shadow (`shadow`, `shadow_dx`, `shadow_dy`, `shadow_blur`, `shadow_color`)

Available on `text`, `rect`, and `progress`.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `shadow` | bool | `false` | Enable drop shadow. |
| `shadow_dx` | int | `2` | Horizontal shadow offset in pixels. Positive moves the shadow right. |
| `shadow_dy` | int | `2` | Vertical shadow offset in pixels. Positive moves the shadow down. |
| `shadow_blur` | int | `4` | Blur radius in pixels. `0` = hard shadow, larger values = softer/more diffuse. |
| `shadow_color` | color | `#00000080` | Shadow colour. Semi-transparent black is the usual choice. |

### Animation (`animate` command)

An element's opacity, position (`x`/`y`), size (`w`/`h`), or colour can be
animated after creation, selected by the `property` field. See the
[`animate`](#animate) section.

---

## Commands

### `clear`

Full reset. Removes every element (texts, rects, overlays, progress bars,
spinners, consoles, arcs, QR codes) and the background image. Loaded fonts
are preserved and do not need to be reloaded.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `color` | color | `#000000` | Backdrop colour to set after clearing. |

---

### `bg_color`

Set the solid background colour that is drawn before any elements.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `color` | color | `#000000` | Background colour. |

---

### `image`

Set or replace the background image.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `path` | string | — | Image file path. PNG and JPEG are supported. Bare names are searched in standard prefixes (see README). |
| `mode` | int or string | `1` | Scaling mode — see table below. |
| `scale` | float | `1.0` | Scale factor. Only used when `mode` is `4` (custom). |
| `filter` | int or string | `3` | Resampling filter — see table below. |
| `crossfade` | int | `0` | Fade duration in milliseconds from the previous background image. `0` = instant replace. |

**Scaling modes (`mode`)**

| Value | Name | Behaviour |
|-------|------|-----------|
| `0` | Cover | Scale to fill the screen, cropping the image if the aspect ratios differ. No letterboxing. |
| `1` | Contain | Scale to fit entirely within the screen. Letterbox bars are filled with `bg_color`. |
| `2` | Stretch | Scale to exactly fill the screen, ignoring the aspect ratio. |
| `3` | None | Draw at native pixel size, centred. |
| `4` | Custom | Scale by the `scale` factor, centred. |

**Resampling filters (`filter`)**

| Value | Name | Notes |
|-------|------|-------|
| `0` | Nearest | Fastest. Pixelated at non-integer scales. |
| `1` | Bilinear | Fast. Slightly blurry. |
| `2` | Bicubic | Mitchell-Netravali kernel. Good balance of sharpness and speed. |
| `3` | Lanczos-3 | Best quality. Slightly slower. **Default.** |

---

### `text` / `remove_text`

Add, update, or remove a text label. Supports UTF-8, kerning, and
sub-pixel positioning. On an existing `id` the supplied fields are **merged**
into the current element (see [merge updates](#creating-vs-updating-elements-merge-updates));
`"replace": true` resets to defaults first and `"remove": true` deletes the
element (equivalent to `remove_text`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `text` | string | — | Content. `\n` inserts a hard line break. |
| `x` | coord | `-1` | Anchor x position. |
| `y` | coord | `-1` | Anchor y position. |
| `align` | 0–2 | `1` (center) | Horizontal alignment relative to `x`. |
| `valign` | 0–2 | `1` (middle) | Vertical alignment relative to `y`. |
| `color` | color | `white` | Text colour. |
| `font` | int | `0` | Font slot index (0–4). Slots are loaded via `--config`. |
| `size` | float | slot default | Font size in pixels. Overrides the slot's default size. |
| `wrap` | bool | `false` | Enable word wrap. Long lines are broken at word boundaries so they stay within `wrap_width`. |
| `wrap_width` | int | `0` | Maximum line width in pixels. `0` uses the display width. Only used when `wrap` is true. |
| `shadow` | bool | `false` | Drop shadow behind the text. |
| `shadow_dx` | int | `2` | Shadow x offset in pixels. |
| `shadow_dy` | int | `2` | Shadow y offset in pixels. |
| `shadow_blur` | int | `4` | Shadow blur radius. `0` = hard shadow. |
| `shadow_color` | color | `#000000a0` | Shadow colour. |
| `outline` | int | `0` | Stroke width in pixels drawn around the glyphs. `0` = no outline. Drawn under the text for legibility over busy or low-contrast backgrounds. |
| `outline_color` | color | `black` | Outline (stroke) colour. |
| `opacity` | float | `1.0` | Master alpha. |

---

### `rect` / `remove_rect`

Add, update, or remove a rectangle. Can be filled, outlined, or both.
Supports rounded corners, gradient fills, and drop shadows. On an existing `id`
the supplied fields are **merged** into the current element (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the element (equivalent to
`remove_rect`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `-1` | Anchor x position. |
| `y` | coord | `-1` | Anchor y position. |
| `w` | coord | `0` | Width in pixels. |
| `h` | coord | `0` | Height in pixels. |
| `align` | 0–2 | `1` | Horizontal anchor. |
| `valign` | 0–2 | `1` | Vertical anchor. |
| `color` | color | `white` | Fill colour (or the first gradient stop). |
| `fill` | bool | `true` | Fill the rectangle. Set to `false` for an outline-only rect. |
| `radius` | int | `0` | Corner radius in pixels. `0` = sharp corners. |
| `border_color` | color | `white` | Border (outline) colour. |
| `border_width` | int | `0` | Border thickness in pixels. `0` = no border. |
| `grad_color` | color | — | Second gradient stop. Only used when `grad_dir` is non-zero. |
| `grad_dir` | int | `0` | Gradient direction: `0`=solid, `1`=vertical (top→bottom), `2`=horizontal (left→right), `3`=diagonal (top-left→bottom-right). `gradient` is accepted as a legacy alias. |
| `shadow` | bool | `false` | Drop shadow behind the rectangle. |
| `shadow_dx` | int | `4` | Shadow x offset. |
| `shadow_dy` | int | `6` | Shadow y offset. |
| `shadow_blur` | int | `10` | Shadow blur radius. |
| `shadow_color` | color | `#00000082` | Shadow colour. |
| `opacity` | float | `1.0` | Master alpha. |

---

### `ellipse` / `circle` / `remove_ellipse` / `remove_circle`

Add, update, or remove an ellipse or circle. `circle` is an alias for `ellipse`
and produces the same element. Can be filled or drawn as an outline ring. The
centre is at `(x, y)`. On an existing `id` the supplied fields are **merged**
into the current element (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the element (equivalent to
`remove_ellipse` / `remove_circle`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `-1` | Centre x. Negative = screen centre on that axis. |
| `y` | coord | `-1` | Centre y. Negative = screen centre on that axis. |
| `radius` | int | `0` | Circle radius in pixels. Shorthand for setting `rx` (and `ry` mirrors it). |
| `rx` | int | `0` | Horizontal radius in pixels. |
| `ry` | int | `0` | Vertical radius in pixels. `0` mirrors `rx`, producing a circle. |
| `thickness` | int | `0` | `0` = filled (default); `>0` draws an outline ring of this width in pixels. |
| `color` | color | `white` | Fill colour (filled) or ring colour (outline). |
| `opacity` | float | `1.0` | Master alpha. |

`query` (type `ellipse` or `circle`) returns `x`, `y`, `rx`, `ry`, `thickness`,
and `opacity`. Opacity can be animated with the [`animate`](#animate) command.

---

### `line` / `remove_line`

Add, update, or remove a straight line, typically used as a divider. The line
runs from `(x1, y1)` to `(x2, y2)`. On an existing `id` the supplied fields are
**merged** into the current element (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the element (equivalent to
`remove_line`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x1` | coord | `-1` | Start point x. Negative = screen centre on that axis. |
| `y1` | coord | `-1` | Start point y. Negative = screen centre on that axis. |
| `x2` | coord | `-1` | End point x. Negative = screen centre on that axis. |
| `y2` | coord | `-1` | End point y. Negative = screen centre on that axis. |
| `thickness` | int | `2` | Line width in pixels. |
| `cap` | int | `0` | End cap style: `0` = flat/butt ends (default), `1` = round. |
| `color` | color | `white` | Line colour. |
| `opacity` | float | `1.0` | Master alpha. |

`query` (type `line`) returns `x1`, `y1`, `x2`, `y2`, `thickness`, and
`opacity`. Opacity can be animated with the [`animate`](#animate) command.

---

### `stepper` / `remove_stepper`

A step / boot-stage indicator: a centred row of dots or pills where the first
`current` of `count` steps are drawn as "done". Advance `current` as boot
progresses — typically with a merge update such as
`{"cmd":"stepper","id":0,"current":3}`. On an existing `id` the supplied fields
are **merged** into the current element (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the element (equivalent to
`remove_stepper`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `-1` | Row anchor x. Negative = screen centre on that axis. |
| `y` | coord | `-1` | Row anchor y. Negative = screen centre on that axis. |
| `align` | 0–2 | `1` | Horizontal positioning anchor of the row. |
| `valign` | 0–2 | `1` | Vertical positioning anchor of the row. |
| `count` | int | `3` | Number of steps. |
| `current` | int | `0` | Steps marked done, `0..count`. |
| `style` | int | `0` | Step shape: `0` = dots (default), `1` = bars (pills). |
| `size` | int | `12` | Dot radius (style 0) or bar height (style 1) in pixels. |
| `length` | int | `0` | Bar length for style 1, in pixels. `0` = `size * 3`. |
| `gap` | int | — | Spacing between steps in pixels. |
| `thickness` | int | `0` | Todo (remaining) steps: `0` = filled (default), `>0` = outline width in pixels. |
| `color_done` | color | `white` | Colour of completed steps. |
| `color_todo` | color | dim grey | Colour of remaining steps. |
| `opacity` | float | `1.0` | Master alpha. |

`query` (type `stepper`) returns `x`, `y`, `count`, `current`, and `opacity`.
Opacity can be animated with the [`animate`](#animate) command.

---

### `marquee` / `remove_marquee`

A single line of text that scrolls horizontally inside a clip box. The text
repeats with `gap` spacing for a seamless loop and is clipped to the box, so
content wider than the box scrolls smoothly across it. Like `text` and
`console`, it requires a font slot to be loaded (via `--config` or a config
file). On an existing `id` the supplied fields are **merged** into the current
element (see [merge updates](#creating-vs-updating-elements-merge-updates));
`"replace": true` resets to defaults first and `"remove": true` deletes the
element (equivalent to `remove_marquee`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `-1` | Clip box x position. Negative = screen centre on that axis. |
| `y` | coord | `-1` | Clip box y position. Negative = screen centre on that axis. |
| `w` | coord | `400` | Clip box width in pixels. |
| `h` | coord | `40` | Clip box height in pixels. |
| `text` | string | — | The text to scroll. |
| `font` | int | `0` | Font slot index. |
| `size` | float | slot default | Font size in pixels. `0` = use the font slot's loaded size. |
| `color` | color | `white` | Text colour. |
| `speed` | int | `60` | Scroll speed in px/sec: `>0` scrolls left, `<0` scrolls right, `0` = static. |
| `gap` | int | `60` | Gap between repetitions in pixels, for the seamless loop. |
| `opacity` | float | `1.0` | Master alpha. |

`query` (type `marquee`) returns `text`, `x`, `y`, `w`, `h`, `speed`, and
`opacity`. Opacity can be animated with the [`animate`](#animate) command.

---

### `sprite` / `remove_sprite`

A frame animation: cycles through a list of loaded images (frames) at a fixed
frame rate — for example an animated logo or a custom spinner. The frames are
loaded once when the element is created. On an existing `id` the supplied fields
are **merged** into the current element (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the element (equivalent to
`remove_sprite`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `frames` | array | — | List of image file paths, one per frame (PNG / JPEG, up to 32). |
| `x` | coord | `-1` | Anchor x position. Negative = screen centre on that axis. |
| `y` | coord | `-1` | Anchor y position. Negative = screen centre on that axis. |
| `w` | coord | `0` | Draw width. `0` = native frame size; if only one of `w`/`h` is given, the other is derived from the frame aspect ratio. |
| `h` | coord | `0` | Draw height. `0` = native frame size; derived from `w` and aspect ratio when only `w` is given. |
| `align` | 0–2 | `1` | Horizontal positioning anchor. |
| `valign` | 0–2 | `1` | Vertical positioning anchor. |
| `filter` | int | `3` (lanczos) | Scaling filter: `0`=nearest, `1`=bilinear, `2`=bicubic, `3`=lanczos. |
| `fps` | int | `12` | Frames per second. |
| `loop` | bool | `true` | Repeat the animation. `false` = play once, then hold the last frame. |
| `opacity` | float | `1.0` | Master alpha. |

On a merge update, `frames` reloads **only** when supplied — omitting it keeps
the running animation. `query` (type `sprite`) returns `x`, `y`, the frame
`count`, the current frame, `fps`, and `opacity`. Opacity can be animated with
the [`animate`](#animate) command.

---

### `overlay` / `remove_overlay`

Add, update, or remove a bitmap image drawn on top of the background. On an
existing `id` the supplied fields are **merged** into the current element (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the element (equivalent to
`remove_overlay`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `path` | string | — | Image file path (PNG / JPEG). Required for a new overlay; optional on a merge update (see below). |
| `x` | coord | `-1` | Anchor x position. |
| `y` | coord | `-1` | Anchor y position. |
| `w` | coord | `0` | Display width. `0` derives from `h` and the image's aspect ratio, or uses native width if `h` is also 0. |
| `h` | coord | `0` | Display height. `0` derives from `w` and aspect ratio, or uses native height. |
| `align` | 0–2 | `1` | Horizontal anchor. |
| `valign` | 0–2 | `1` | Vertical anchor. |
| `filter` | int or string | `3` (lanczos) | Resampling filter (same values as `image`). |
| `radius` | int | `0` | Rounded-corner clip radius in pixels. `0` = sharp corners (no clip). |
| `angle` | float | `0` | Rotation around the image's centre, in degrees. `0` = no rotation. |
| `tint` | color | — | Multiply tint applied to the image; the tint's alpha is the strength (omit, or alpha `0`, = no tint). A white tint is a no-op; a coloured tint multiplies the image toward that colour. |
| `opacity` | float | `1.0` | Master alpha. |

On a merge update of an existing overlay, `path` may be **omitted** to keep the
current image and change only other fields (e.g. just the `angle`). A fresh
overlay still requires `path`. The `radius`, `angle`, and `tint` effects use
bilinear sampling.

---

### `progress` / `update_progress` / `hide_progress` / `remove_progress`

Horizontal progress bar. Supports built-in colour themes, custom colours,
gradient fill, percentage label, and indeterminate (sweeping highlight) mode.
On an existing `id` the supplied fields are **merged** into the current bar (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the bar (equivalent to
`remove_progress`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `-1` | Anchor x position. |
| `y` | coord | `-1` | Anchor y position. |
| `w` | coord | `100` | Width in pixels. |
| `h` | coord | `20` | Height in pixels. |
| `align` | 0–2 | `1` | Horizontal anchor. |
| `valign` | 0–2 | `1` | Vertical anchor. |
| `value` | float | `0.0` | Fill level: `0.0` = empty, `1.0` = full. |
| `style` | int | `0` | Built-in colour theme (0–5). Set to `-1` to use custom colour fields. When a custom colour field is given alongside a style, the bar switches to fully custom colours automatically. |
| `bg_color` | color | theme | Background track colour. |
| `bar_color` | color | theme | Fill colour (or first gradient stop). |
| `bar_color2` | color | — | Second gradient stop. Only used when `bar_gradient` is non-zero. |
| `bar_gradient` | int | `0` | Gradient direction along the fill: same values as `grad_dir` on `rect`. `gradient` is accepted as a legacy alias. |
| `border_color` | color | theme | Border colour. |
| `text_color` | color | theme | Percentage label colour. |
| `border_width` | int | `2` | Border thickness in pixels. `0` = no border. On a very thin bar the border is capped so the fill always keeps at least 1px. |
| `radius` | int | `0` | Corner radius in pixels. |
| `font_slot` | int | `0` | Font slot for the percentage label. `font` is accepted as an alias. |
| `font_size` | float | `0` | Font size for the percentage label. `0` = auto (roughly half the bar height). `size` is accepted as an alias. |
| `show_percent` | bool | `false` | Render the percentage value as text inside the bar. |
| `indeterminate` | bool | `false` | Sweeping highlight mode: the fill animates back and forth regardless of `value`. Useful for "busy" states where actual progress is unknown. |
| `indet_period_ms` | int | `1100` | Duration of one full sweep cycle in indeterminate mode. |
| `shadow` | bool | `false` | Drop shadow behind the whole bar. |
| `shadow_dx` | int | `0` | Shadow x offset. |
| `shadow_dy` | int | `4` | Shadow y offset. |
| `shadow_blur` | int | `12` | Shadow blur radius. |
| `shadow_color` | color | `#00000078` | Shadow colour. |
| `opacity` | float | `1.0` | Master alpha. |

**`update_progress`** accepts `id`, `value`, and optionally `indeterminate`.
All other fields are unchanged.

**`hide_progress`** accepts `id` only. The element is deactivated (hidden) but
its configuration is preserved, so a later `progress` command with the same id
can restore it.

**`remove_progress`** accepts `id` only. Fully clears the slot and returns it
to the pool.

**Built-in styles**

| `style` | Theme |
|---------|-------|
| `0` | Blue |
| `1` | Green |
| `2` | Amber |
| `3` | Red |
| `4` | Purple |
| `5` | Cyan |

---

### `arc` / `update_arc` / `hide_arc` / `remove_arc`

Circular or arc-shaped progress indicator. The centre is at `(x, y)`. On an
existing `id` the supplied fields are **merged** into the current arc (see
[merge updates](#creating-vs-updating-elements-merge-updates)); `"replace": true`
resets to defaults first and `"remove": true` deletes the arc (equivalent to
`remove_arc`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `-1` | Centre x. Negative = screen centre on that axis. |
| `y` | coord | `-1` | Centre y. Negative = screen centre on that axis. |
| `radius` | int | `80` | Outer radius in pixels. |
| `thickness` | int | `0` | Stroke width in pixels. `0` = `radius / 4`. |
| `value` | float | `0.0` | Fill level: `0.0` = empty, `1.0` = full sweep. |
| `start_angle` | float | `-90` | Angle where the arc starts, in degrees. `0` = right (3 o'clock), `-90` = top (12 o'clock), `90` = bottom, `180` = left. The arc sweeps clockwise. |
| `sweep` | float | `360` | Total arc length in degrees. `360` = full circle. `270` = three-quarters. `180` = semicircle. Values ≤ 0 or ≥ 360 all produce a full circle. |
| `bg_color` | color | `#80808080` | Colour of the unfilled (background) portion of the arc. Alpha `0` hides the background arc entirely. |
| `bar_color` | color | `white` | Colour of the filled portion. |
| `bar_color2` | color | — | Second colour stop for a sweep gradient. Only used when `bar_gradient` is `1`. |
| `bar_gradient` | int | `0` | `0` = solid fill, `1` = gradient that sweeps from `bar_color` to `bar_color2` along the arc. |
| `cap` | int | `0` | End cap style: `0` = flat (angular cutoff), `1` = round (semicircle at each end of the filled arc). |
| `font_slot` | int | `-1` | Font slot for the centre percentage label. `-1` = no label. |
| `font_size` | float | `0` | Font size for the centre label in pixels. |
| `text_color` | color | `white` | Colour of the centre label. |
| `show_percent` | bool | `false` | Render the current percentage in the centre of the arc. Requires `font_slot` and `font_size`. |
| `indeterminate` | bool | `false` | Spinning mode: a fixed-length arc segment (1/3 of the sweep) rotates continuously. `value` is ignored. |
| `indet_period_ms` | int | `1200` | Duration of one full rotation in indeterminate mode. |
| `opacity` | float | `1.0` | Master alpha. |

**`update_arc`** accepts `id`, `value`, and optionally `bar_color`.

**`hide_arc`** accepts `id` only. Deactivates the element without removing it;
the slot is preserved and a later `arc` command with the same `id` restores it.

**`remove_arc`** accepts `id` only. Fully clears the slot and returns it to
the pool.

---

### `spinner` / `remove_spinner`

Apple-style rotating spinner made of fading spokes. The centre is at `(x, y)`.
On an existing `id` the supplied fields are **merged** into the current spinner
(see [merge updates](#creating-vs-updating-elements-merge-updates));
`"replace": true` resets to defaults first and `"remove": true` deletes the
spinner (equivalent to `remove_spinner`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `-1` | Centre x. Negative = screen centre. |
| `y` | coord | `-1` | Centre y. Negative = screen centre. |
| `radius` | int | `36` | Outer radius (tip of the longest spoke) in pixels. |
| `spokes` | int | `12` | Number of spokes. The oldest spoke is the most transparent. |
| `color` | color | `white` | Spoke colour. The opacity of each spoke fades towards the oldest. |
| `period` | int | `900` | Duration of one full rotation in milliseconds. |
| `action` | string | `"show"` | Visibility action — see table below. |
| `duration` | int | `300` | Fade duration in milliseconds for `"show_animated"` / `"hide_animated"`. Ignored by the instant `"show"` / `"hide"` actions. |
| `easing` | string or int | `"ease_in_out"` | Easing curve for the animated actions (same values as the [`animate`](#animate) command). |
| `hidden` | bool | `false` | Alternative to `action`: configure the spinner without making it visible yet, ready to be revealed later with `"show_animated"`. |
| `opacity` | float | `1.0` | Master alpha. |

**`action`**

| Value | Behaviour |
|-------|-----------|
| `"show"` | Make the spinner visible and start it immediately. |
| `"hide"` | Stop and hide it immediately. The slot (configuration) is preserved. |
| `"show_animated"` | Reveal the spinner with an opacity fade-in over `duration` ms using `easing`. |
| `"hide_animated"` | Fade the spinner out over `duration` ms using `easing`, then stop rendering. |

**`remove_spinner`** accepts `id` only. Deactivates the spinner in place, like
the other `remove_*` commands. (Use `action: "hide"` instead if you want to keep
the slot configuration for a later reveal.)

---

### `console` / `console_write` / `remove_console`

A fixed-position scrolling text area. Lines are stored in a circular ring
buffer; when the buffer is full the oldest line is discarded. Lines are
anchored to the bottom of the area — as the count grows from zero, new lines
appear at the bottom and earlier lines stack upward.

On an existing `id` the supplied `console` fields are **merged** into the current
console (see [merge updates](#creating-vs-updating-elements-merge-updates));
`"replace": true` resets to defaults first and `"remove": true` deletes the
console (equivalent to `remove_console`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `x` | coord | `0` | Left edge of the area. Negative = centred horizontally. |
| `y` | coord | `0` | Top edge of the area. Negative = centred vertically. |
| `w` | coord | `400` | Width in pixels. |
| `h` | coord | `200` | Height in pixels. |
| `font_slot` | int | `0` | Font slot. |
| `size` | float | `0` | Font size in pixels. `0` = use the font slot's loaded size. |
| `color` | color | `white` | Text colour. |
| `bg_color` | color | `transparent` | Background fill. Alpha `0` = fully transparent (no background). |
| `padding` | int | `4` | Inner margin between the area's edges and the text, in pixels. |
| `max_lines` | int | `32` | Ring buffer capacity. Maximum `64`. Reducing this on an existing console clears the buffer. |
| `opacity` | float | `1.0` | Master alpha applied to both text and background. |

**`console_write`**

| Parameter | Type | Description |
|-----------|------|-------------|
| `id` | int | Console element to write to (required). |
| `text` | string | Text to append. A `\n` in the string splits the content into multiple separate lines, each occupying one ring buffer slot. |
| `color` | color | Colour for the line(s) being written. Omit to use the console's own default colour. Each line keeps the colour it was written with, so a single console can mix colours — handy for severity-coloured boot logs (green ok / yellow warn / red fail). |

---

### `qr` / `remove_qr`

Encodes a text payload as a QR Code and renders it as a grid of filled
rectangles. The QR Code version (size) is chosen automatically to fit the
content. On an existing `id` the supplied fields are **merged** into the current
element (see [merge updates](#creating-vs-updating-elements-merge-updates));
`"replace": true` resets to defaults first and `"remove": true` deletes the
element (equivalent to `remove_qr`).

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `id` | int | — | Element id (required). |
| `text` | string | — | Payload to encode (required). URLs, plain text, and any UTF-8 content are accepted. |
| `x` | coord | `0` | Position of the top-left corner (after alignment). Negative = centred on that axis. |
| `y` | coord | `0` | Position of the top-left corner. |
| `align` | 0–2 | `0` | Horizontal anchor applied after `x` is resolved. |
| `valign` | 0–2 | `0` | Vertical anchor applied after `y` is resolved. |
| `module_px` | int | `0` | Pixel size of each QR module (one data square). `0` = auto: sized so the total code fills roughly ¼ of the shorter screen dimension. |
| `border` | int | `4` | Quiet zone width in modules around the code. The QR standard requires at least `4`. |
| `ecc` | int | `1` | Error correction level: `0`=LOW (~7% damage tolerance), `1`=MEDIUM (~15%), `2`=QUARTILE (~25%), `3`=HIGH (~30%). Higher levels produce a larger/denser code. |
| `color` | color | `black` | Dark module colour. |
| `bg_color` | color | `white` | Light module (background) colour. Alpha `0` = transparent background, dark modules only. |
| `opacity` | float | `1.0` | Master alpha. |

---

### `animate`

Animate an element property over time. The animation runs in the background;
the daemon continues to process commands while it plays. The `property` field
selects what is animated — opacity (the default), position, size, or colour.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `type` | string | — | Element type to animate: `"text"`, `"rect"`, `"overlay"`, `"progress"`, `"arc"`, `"spinner"`, `"console"`, `"qr"` (required). |
| `id` | int | — | Element id (required). |
| `property` | string | `"opacity"` | What to animate — see the property table below. |
| `from` | float / int / color | current value | Start value. If omitted, the element's current value for the chosen property is used. Type depends on `property` (see below). |
| `to` | float / int / color | — | End value (required). Type depends on `property`. |
| `duration` | int | — | Animation duration in milliseconds (required). |
| `easing` | string or int | `"ease_out"` | Easing curve — see table below. |
| `repeat` | bool | `false` | Loop the animation in ping-pong fashion (`from`→`to`→`from`→…). |
| `remove_on_end` | bool | `false` | Deactivate (remove) the element when the animation reaches its end. Useful for fade-out transitions. |

**Properties (`property`)**

| Value | `from`/`to` type | Effect |
|-------|------------------|--------|
| `"opacity"` | float `0.0`–`1.0` | Master alpha (the default; behaves as before). |
| `"x"` | int (pixels) | Move the element horizontally. |
| `"y"` | int (pixels) | Move the element vertically. |
| `"w"` | int (pixels) | Resize / grow the element's width. |
| `"h"` | int (pixels) | Resize / grow the element's height. |
| `"color"` | color | Colour transition, interpolated per channel (RGBA lerp). |

For `x`/`y`/`w`/`h` the `from`/`to` values are pixel integers; for `color` they
are colours (named or hex); for `opacity` they are `0.0`–`1.0` floats as before.
When `from` is omitted it defaults to the element's current value for that
property. `duration`, `easing`, `repeat`, and `remove_on_end` work identically
for every property.

Each element type can animate only the properties it actually has. Most
elements support `opacity`, `x`, and `y`. `w`/`h` additionally work on `rect`,
`marquee`, `console`, `sprite`, `progress`, `overlay`, and `ellipse`. `color`
additionally works on `text`, `rect`, `ellipse`, `line`, `marquee`, `console`,
`spinner`, and `qr`. Requesting a property an element does not support returns
an error.

**Easing curves**

| Value | Behaviour |
|-------|-----------|
| `"linear"` | Constant rate throughout. |
| `"ease_in"` | Starts slow, accelerates. |
| `"ease_out"` | Starts fast, decelerates. Feels natural for fade-outs. |
| `"ease_in_out"` | Slow at both ends, fast in the middle. Smooth for fades. |

---

### `query`

Read back the current state of any named element. Useful for scripts that need
to know the current progress value before deciding whether to send an update
(e.g. to avoid driving a value backwards when parallel init scripts race).

| Parameter | Type | Description |
|-----------|------|-------------|
| `type` | string | Element type: `"text"`, `"rect"`, `"overlay"`, `"progress"`, `"arc"`, `"spinner"`, `"console"`, `"qr"` (required). |
| `id` | int | Element id (required). |

The response always includes `"status": "ok"`, `"x"`, `"y"`, and `"opacity"`.
Type-specific fields:

| Type | Extra fields |
|------|-------------|
| `text` | `"text"` (current string) |
| `rect` | `"w"`, `"h"` |
| `overlay` | `"w"`, `"h"` |
| `progress` | `"value"`, `"w"`, `"h"` |
| `arc` | `"value"`, `"radius"` |
| `spinner` | `"active"` |
| `console` | `"w"`, `"h"`, `"line_count"` |
| `qr` | `"text"` (encoded payload) |

---

### `status`

Query the daemon's current state. Takes no parameters.

Response fields:

| Field | Type | Description |
|-------|------|-------------|
| `state` | string | `"running"` or `"suspended"` (after `suspend`). |
| `ready` | bool | `true` after the `ready` command has been sent. |
| `hidden` | bool | `true` when the display is blanked by the ESC keyboard toggle. |
| `width` | int | Display width in pixels. |
| `height` | int | Display height in pixels. |

---

### `suspend` / `resume`

`suspend` freezes rendering: the daemon stops compositing frames. The display
retains whatever was last shown. `resume` restarts rendering. Both commands
take no parameters.

---

### `ready`

Marks the daemon as ready. Sets `"ready": true` in the `status` response.
Intended for external health checks — the daemon's behaviour is otherwise
unaffected. Takes no parameters.

---

### `exit`

Requests a clean shutdown: the daemon restores the display to the state saved
at startup, then exits.

| Parameter | Type | Default | Description |
|-----------|------|---------|-------------|
| `delay` | int | `0` | Seconds to wait before exiting. `0` exits immediately. |

Without `delay` the daemon exits as soon as it has sent the reply. With
`delay` it continues rendering and accepting commands for that many seconds,
then shuts down automatically. This allows displaying a final message or
running a fade-out animation before the process ends.

```json
{"cmd": "text", "id": 99, "text": "System ready", "size": 48}
{"cmd": "exit", "delay": 3}
```

The client receives an `ok` reply immediately in both cases; the delay only
affects when the daemon process actually terminates.

---

## Appendix: Colour formats

| Format | Example | Notes |
|--------|---------|-------|
| Named | `"transparent"` | Alpha 0, all channels 0. |
| Named | `"black"` | Opaque black. |
| Named | `"white"` | Opaque white. |
| Named | `"red"`, `"green"`, `"blue"` | Full-intensity primaries. |
| Named | `"yellow"`, `"cyan"`, `"magenta"` | Full-intensity secondaries. |
| Named | `"orange"` | `#FF8000`. |
| Named | `"gray"` / `"grey"` | `#808080`, opaque. |
| Named | `"darkgray"` / `"darkgrey"` | `#404040`, opaque. |
| Named | `"lightgray"` / `"lightgrey"` | `#C0C0C0`, opaque. |
| `#RGB` | `"#fff"` | Each channel nibble doubled: `#ffffff`. |
| `#RRGGBB` | `"#1a2b3c"` | Fully opaque. |
| `#RRGGBBAA` | `"#ffffff80"` | The last two hex digits are alpha. `80` ≈ 50% opacity. |

Alpha is always in the **last** byte of the 8-digit form. `00` = fully
transparent, `ff` = fully opaque.

---

## Appendix: Daemon startup options

```
splash-drm <drm_device> [options]
```

| Option | Description |
|--------|-------------|
| `--config <file\|json>` | Load font configuration at startup (see [Font slots](#appendix-font-slots)). Accepts a file path or an inline JSON string. |
| `--cmds <file\|json>` | Execute a batch of commands immediately after startup, before the event loop begins. Accepts a file path or an inline JSON string. |
| `--fork` | Fork to the background before entering the event loop. The parent process exits immediately, returning control to the caller. **Recommended for initramfs use** — the child calls `setsid()` after forking, creating a new session with no controlling terminal, so `switch_root` and shell exit cannot deliver `SIGHUP` to the daemon. Without `--fork`, a best-effort `setsid()` is attempted, but it silently fails when the shell's job-control places the process in its own process group. |
| `--timeout <seconds>` | Watchdog: exit automatically if no command arrives within this many seconds. Useful as a safety net so a stuck boot script cannot leave the splash on screen indefinitely. |
| `--headless` | Initialise DRM but never program the CRTC, so the console stays visible; renders off-screen only. See [below](#headless-and-config-check-modes). |
| `--check` | Validate `--config`/`--cmds` without opening DRM, then exit (`0` = ok). See [below](#headless-and-config-check-modes). |
| `--dump <file.png>` | Render one frame from `--config`/`--cmds` to a PNG file, then exit — no daemon loop and no control socket. Uses the connected display's resolution. Pair with `--headless` to render without touching the live console. See [below](#headless-and-config-check-modes). |
| `-q`, `--quiet` | Silence all output (even errors). |
| `-v`, `-vv`, `-vvv` | Increase log verbosity (info / debug / trace). See [Logging](#logging). |
| `--debug` | Alias for `-vv` (debug verbosity). |
| `--log <target>` | Log sink: `auto` (default), `stderr`, `syslog`, or `kmsg`. See [Logging](#logging). |
| `-V`, `--version` | Print version and exit. |
| `-h`, `--help` | Print usage summary and exit. |
| `--help <cmd>` | Print full parameter list for a command (e.g. `--help arc`). |

### Recommended initramfs invocation

```sh
splash-drm /dev/dri/card0 --fork --config /etc/splash/config.json \
    --cmds '[{"cmd":"image","path":"/etc/splash/bg.png"},{"cmd":"spinner","id":0}]'

# … mount real root, etc. …

splash-ctl '{"cmd":"suspend"}'
exec switch_root /sysroot /sbin/init
```

In the new root's init scripts:

```sh
splash-ctl '{"cmd":"resume"}'
# … update progress as services start …
splash-ctl '{"cmd":"exit"}'
```

`--fork` makes the `&` shell operator unnecessary and guarantees the daemon
survives `switch_root` regardless of the shell's job-control configuration.

### Logging

The daemon has five log levels, ordered `ERROR < WARN < INFO < DEBUG < TRACE`.
The default level is `ERROR`: a normal run is silent except for genuine
failures. Verbosity flags raise the level — `-v` enables `INFO`, `-vv` enables
`DEBUG`, and `-vvv` enables `TRACE`; `--debug` is an alias for `-vv`. `-q` /
`--quiet` silences everything, including errors.

`--log` selects the sink. A foreground run logs to `stderr`. A `--fork`ed
daemon has its stdio redirected to `/dev/null`, so it falls back to the syslog
socket `/dev/log` and then the kernel log `/dev/kmsg`. The default `auto`
follows that order; `stderr`, `syslog`, and `kmsg` force a specific sink
regardless of forking. Syslog messages are tagged `splash-drm[pid]` at the
`daemon` facility.

### Headless and config-check modes

`--headless` initialises DRM and reads the connected display's mode — it needs
a connected display to size its buffers — but never programs the CRTC. The
kernel console and live log output therefore stay visible. This is useful
during the initramfs→rootfs boot: you can watch boot messages and live logs
without the splash covering them. Rendering still runs off-screen, so the
render path is exercised even though nothing reaches the panel.

`--check` validates a boot configuration (`--config`/`--cmds` JSON: structure,
known commands, and their parameters) on a build host without opening DRM or
touching the referenced asset files. It exits non-zero on structural problems,
making it a quick way to catch a typo before deploying to an initramfs.

`--dump <file.png>` renders a single frame from `--config`/`--cmds` to a PNG file
and then exits. It sizes the image to the connected display's resolution but runs
neither the daemon loop nor the control socket, so it never collides with a
running daemon's abstract name. Combined with `--headless` it renders entirely
off-screen, leaving the live console untouched — handy for previewing a boot
configuration on a build host or capturing golden frames for regression testing.

---

## Appendix: Font slots

Fonts are loaded once at startup via `--config` and referenced by slot index
(0–4) in element commands. Up to 5 fonts can be loaded simultaneously.

```json
{"fonts": [
  {"slot": 0, "path": "DejaVuSans.ttf",      "size": 24},
  {"slot": 1, "path": "DejaVuSans-Bold.ttf", "size": 24},
  {"slot": 2, "path": "DejaVuSans.ttf",      "size": 14}
]}
```

The `path` is resolved using the same prefix search as images: bare names are
tried against `/usr/share/splash/fonts/`, `/usr/share/splash/`, and
`/usr/share/fonts/` before failing.

The `size` set in the config is the default for that slot. Individual elements
can override it with their own `size` or `font_size` field.
