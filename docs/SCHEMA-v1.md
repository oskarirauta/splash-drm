# splash-drm schema v1 (design draft)

Status: **design draft, not yet implemented.** This document is the agreed
target for the next protocol/config redesign. It supersedes the current
`{"cmd": "..."}` command grammar. The program is still alpha and unannounced,
so this is a hard, breaking switch — there is no alias/compatibility phase, and
the same release updates the daemon, `examples/`, `contrib/` and `REFERENCE.md`.

## 1. Goals

splash-drm is a declarative, IPC-driven DRM scene compositor. A boot splash is
one application of it; the schema must not bake in boot-specific assumptions.

The v1 redesign exists to make **authoring less error-prone**, not to add
rendering features. Concretely:

- Configs read as a *scene description*, not a command script.
- One discriminator per object (the element type), impossible to "forget the
  `cmd`".
- One uniform element shape for create / update / hide / animate / remove —
  no parallel `update_*` / `hide_*` / `remove_*` verb matrix.
- One file holds everything (fonts + scene), instead of split `--config`
  (settings) and `--cmds` (scene).
- Forward-compatible: new sections, element types and fields can be added later
  without breaking existing documents.

## 2. The scene document (`--config`)

A startup config is a single JSON **object** (not a bare array):

```json
{
  "version": 1,
  "fonts": [
    { "slot": 0, "path": "/usr/share/fonts/x.ttf", "size": 24 }
  ],
  "background": "#0b0e14",
  "elements": [
    { "image":    { "path": "logo.png", "mode": 3 } },
    { "progress": { "id": 0, "x": -1, "y": "80%", "w": "60%", "h": 1,
                    "bar_color": "#1894d3" } },
    { "text":     { "id": 99, "x": -1, "y": "20%", "text": "Starting…" } }
  ]
}
```

| Key          | Role                                                          |
|--------------|--------------------------------------------------------------|
| `version`    | Schema version. Required. Gates forward-compatible evolution. |
| `fonts`      | One-time startup settings: font slots. Loaded once at boot.   |
| `background` | Scene clear colour (a colour string, or an object later).     |
| `elements`   | The scene: an array of element operations (see §3).           |

`fonts` and other settings live in the document **header** — they are
one-time, not part of the runtime-mutable element stream. This unifies the
former `--config` object (`{"fonts": [...]}`) and the former `--cmds` array
into one document. `--cmds` is removed; `--config` takes the whole document.

## 3. Message grammar

Every message — whether a line in `elements`, or a runtime message over the
socket — is an object with **exactly one top-level key**. There are two kinds.

### 3.1 Element operations

The single key is the element **type**; its value is the field object. The same
shape is used to create, update, hide, animate and remove an element. Elements
are addressed by `(type, id)`; sending an existing id **merges** (only the
fields you pass change). `replace: true` resets an element to defaults first.

```json
{ "progress": { "id": 0, "x": -1, "y": "80%", "w": "60%", "h": 1,
                "bar_color": "#1894d3", "track_color": "#000", "border": 0 } }

{ "progress": { "id": 0, "value": 0.5 } }                       // update (merge)
{ "progress": { "id": 0, "hidden": true } }                    // hide, keep state
{ "progress": { "id": 0, "animate": { "property": "opacity", "to": 0,
                                      "duration": 400, "remove_on_end": true } } }
{ "progress": { "id": 0, "remove": true } }                    // remove
{ "console":  { "id": 0, "write": "Starting network…", "color": "#9fd2ff" } }
```

Uniform element fields (available on every element type):

| Field      | Meaning                                                      |
|------------|-------------------------------------------------------------|
| `id`       | Element identity within its type. Re-using an id merges.    |
| `remove`   | `true` deletes the element.                                 |
| `hidden`   | `true` hides without deleting (keeps value/state).          |
| `replace`  | `true` resets to type defaults before applying fields.      |
| `animate`  | Start a property tween on this element (see §3.3).          |

Element types: `image`, `text`, `rect`, `ellipse` (`circle` alias),
`line`, `stepper`, `marquee`, `sprite`, `overlay`, `progress`, `spinner`,
`console`, `arc`, `qr`. Per-type fields are documented in `REFERENCE.md`.

### 3.2 System operations

The single key is `system`. Its value is either an action object
`{ "action": "...", ...params }`, or a bare string shorthand for a
parameter-less action.

```json
{ "system": "exit" }                              // shorthand
{ "system": { "action": "exit", "timeout": 4 } }  // with params
{ "system": "status" }
{ "system": "clear" }                             // remove all elements
```

Actions: `exit`, `suspend`, `resume`, `clear`, `status`, `version`,
`running`, `ready`, `query`. Queries (`status`, `version`, `running`,
`ready`, `query`) return data in the response.

### 3.3 Animations

Animation is a **field on the element**, not a separate command. There is no
top-level `animate` op. A one-shot fade-out-and-remove:

```json
{ "progress": { "id": 0,
                "animate": { "property": "opacity", "to": 0,
                             "duration": 400, "easing": "ease_out",
                             "remove_on_end": true } } }
```

Intrinsic animations (indeterminate progress, spinner spin, marquee scroll,
sprite frames) remain ordinary element fields.

### 3.4 Background at runtime

`background` may also be sent as a standalone single-key op to recolour the
scene at runtime: `{ "background": "#101418" }`. In the document it is a header
field; over the socket it is the same top-level single-key op. (It is a scene
property, not a `system` action — this keeps the runtime grammar consistent
with the single-key element ops.)

## 4. Old → new mapping

| Old `cmd`                         | New form                                              |
|-----------------------------------|-------------------------------------------------------|
| `{"cmd":"progress", ...}`         | `{"progress": { ... }}`                               |
| `{"cmd":"update_progress","id":N,"value":v}` | `{"progress": {"id":N,"value":v}}`         |
| `{"cmd":"hide_progress","id":N}`  | `{"progress": {"id":N,"hidden":true}}`                |
| `{"cmd":"remove_progress","id":N}`| `{"progress": {"id":N,"remove":true}}`                |
| `{"cmd":"arc", ...}` / `update_arc` / `hide_arc` / `remove_arc` | `{"arc": { ... }}` with `value` / `hidden` / `remove` |
| `{"cmd":"text", ...}` / `remove_text` | `{"text": { ... }}` / `{"text":{"id":N,"remove":true}}` |
| `{"cmd":"console_write","id":N,"text":t}` | `{"console": {"id":N,"write":t}}`             |
| `{"cmd":"animate","type":T,"id":N, ...}` | `{T: {"id":N,"animate": { ... }}}`             |
| `{"cmd":"bg_color","color":c}`    | header `background` / `{"background": c}`              |
| `{"cmd":"clear"}`                 | `{"system": "clear"}`                                 |
| `{"cmd":"exit","timeout":t}`      | `{"system": {"action":"exit","timeout":t}}` / `{"system":"exit"}` |
| `{"cmd":"suspend"}` / `resume` / `status` / `version` / `running` / `ready` / `query` | `{"system": "..."}` |
| every other `{"cmd":"<element>"}` | `{"<element>": { ... }}`                              |
| every `{"cmd":"remove_<element>","id":N}` | `{"<element>": {"id":N,"remove":true}}`       |

This collapses ~45 commands to ~15 element types plus the `system` namespace.

## 5. Variable substitution (orthogonal preprocessing)

Substitution is a thin text/value-templating pass that runs **below** the
schema: it fills `${...}` placeholders, and only the result is parsed as a
scene document or message. The schema itself is unaware of variables — adding
or removing them never changes the schema.

### 5.1 Where it runs

A single shared routine is used by both binaries, applied wherever a file is
loaded with values:

```sh
splash-ctl --file cmds.json -D MYVALUE="hello world"     # ctl substitutes, then sends
splash-drm --config theme.json -D MYVALUE="hello world"  # daemon substitutes, then loads
```

The daemon must support it (for the initial theme passed via `--config`); the
client uses the same code so runtime files can use variables too. No new IPC
path: the socket only ever carries already-substituted JSON. (`-D NAME=value`
is the working flag spelling, mirroring the C preprocessor; `--define
NAME=value` as the long form.)

### 5.2 Syntax and resolution

- `${NAME}` — replaced by the value of `NAME`.
- `${NAME:-fallback}` — value of `NAME`, or `fallback` if `NAME` is undefined.
- `$${` — a literal `${` (escape).

Resolution is **tree-level (type a)**: the template must itself be valid JSON,
so `${...}` may appear only inside string values. The daemon parses the
template, then walks the tree:

- A **whole-value** token, e.g. `"value": "${V}"`, is replaced by the value's
  **native** JSON type — a numeric-looking value becomes a number, `true`/
  `false` a boolean, otherwise a string. So `-D V=0.5` yields the number `0.5`.
- An **embedded** token, e.g. `"text": "Error: ${MSG}"`, is substituted
  textually and the field stays a string.

Because values are written into already-parsed string nodes, there is no
JSON-escaping hazard, and templates remain lintable JSON.

### 5.3 Undefined / empty semantics

- An undefined variable with no fallback resolves to the **empty string** and
  logs a warning. It is **never** `0`.
- Empty is meaningful per field. An empty `text` clears the text without
  removing the element. A numeric field given an empty/invalid value is treated
  as "unset" — it keeps its default (fresh) or current (merge) value. Thus an
  undefined numeric variable leaves the field unchanged rather than zeroing it.

### 5.4 Example: a themed, parameterised failure screen

```json
[
  { "rect": { "id": 50, "x": -1, "y": -1, "w": "70%", "h": 120,
              "color": "#c0182b", "radius": 12 } },
  { "text": { "id": 51, "x": -1, "y": "44%", "text": "Boot failed",
              "font_size": 32 } },
  { "text": { "id": 52, "x": -1, "y": "52%",
              "text": "${MSG:-Unknown error}", "color": "#ffd7d7" } }
]
```

```sh
splash-ctl --file fail.json -D MSG="disk /dev/sda1 not found"
```

The whole layout is themed in the file; only the dynamic string is injected.
This needs no stored macros, no component system — just substitution.

## 6. Forward compatibility

- `version` is required and drives evolution.
- Unknown element types, unknown fields and unknown `system` actions are
  **ignored with a warning**, not hard errors, so a newer document degrades
  gracefully on an older daemon and vice versa.
- The element grammar is additively extensible: a new type is a new key, a new
  field is just another key in the object.

## 7. Migration

Single hard switch in one release:

1. Replace the daemon parser: object document with `version`/`fonts`/
   `background`/`elements`; single-key dispatch; `system` namespace; the merge/
   `remove`/`hidden`/`animate` element model.
2. Add the shared substitution pass + `-D` flag to both binaries.
3. Update all in-repo configs: `examples/*.json`, `contrib/*.sh`,
   `REFERENCE.md`, `usage.c` help text.
4. Remove `--cmds`.

## 8. Out of scope for v1 (reserved for later, version-gated)

These are intentionally **not** built now; the `version` field lets them be
added without breaking v1 documents:

- **Document-level `vars` / reusable `components` / runtime `define`.** The
  `-D` + `${NAME:-default}` primitive covers current needs. If document-internal
  variables or instantiable components are ever required, they layer on the same
  substitution engine.
- **String / hierarchical element ids and per-instance namespacing.** v1 keeps
  the current numeric `(type, id)` model; reusable fragments pass concrete ids
  via `-D`.
- **Array-order z-ordering.** v1 keeps the existing type-based paint order
  (rects → ellipses → progress/arc → text → spinners on top). An optional
  explicit `z` field can be added later.
- **`style` as a string enum** (instead of integer presets) and a broader
  per-element field-naming pass for consistency. Since there is no alias phase,
  these renames should be decided and applied together when undertaken.
