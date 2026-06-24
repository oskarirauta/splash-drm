/*
 * usage.c - Shared command help text for splash-drm and splash-ctl.
 *
 * print_cmd_help() is compiled into both binaries so that
 * 'splash-drm --help <cmd>' and 'splash-ctl --help <cmd>' give identical,
 * authoritative parameter lists without duplicating the text.
 */

#include "splash.h"

void print_cmd_help(const char *cmd) {
	if (strcmp(cmd, "text") == 0) {
		fprintf(stderr,
		"text — add or update a text label\n\n"
		"  id          int      element id (required)\n"
		"  text        string   content; \\n inserts a line break\n"
		"  x, y        coord    anchor position (default: screen centre)\n"
		"  align       0-2      0=left 1=center 2=right  (or string name)\n"
		"  valign      0-2      0=top  1=middle 2=bottom (or string name)\n"
		"  color       color    text color (default: white)\n"
		"  font        int      font slot index (default: 0)\n"
		"  size        float    font size in pixels\n"
		"  wrap        bool     word-wrap long lines\n"
		"  wrap_width  int      max line width in pixels (0 = screen width)\n"
		"  shadow      bool     enable soft drop shadow\n"
		"  shadow_dx/dy int     shadow offset in pixels (default: 2)\n"
		"  shadow_blur int      shadow blur radius (default: 4)\n"
		"  shadow_color color   shadow color (default: #00000080)\n"
		"  outline     int      stroke width in px around glyphs (0 = none)\n"
		"  outline_color color  stroke color (default: black)\n"
		"  opacity     float    master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the element.\n");
	} else if (strcmp(cmd, "rect") == 0) {
		fprintf(stderr,
		"rect — add or update a rectangle\n\n"
		"  id           int     element id (required)\n"
		"  x, y         coord   anchor position (default: screen centre)\n"
		"  w, h         coord   width and height\n"
		"  align/valign 0-2     positioning anchor\n"
		"  color        color   fill or border color\n"
		"  fill         bool    filled rectangle (default: true)\n"
		"  radius       int     corner radius in pixels\n"
		"  border_color color   border color\n"
		"  border_width int     border thickness in pixels\n"
		"  grad_color   color   gradient second stop\n"
		"  grad_dir     int     0=none 1=vertical 2=horizontal 3=diagonal\n"
		"  shadow       bool    soft drop shadow\n"
		"  shadow_dx/dy int     shadow offset\n"
		"  shadow_blur  int     shadow blur radius\n"
		"  shadow_color color   shadow color\n"
		"  opacity      float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the element.\n");
	} else if (strcmp(cmd, "ellipse") == 0 || strcmp(cmd, "circle") == 0) {
		fprintf(stderr,
		"ellipse (alias: circle) — add or update an ellipse / circle\n\n"
		"  id           int     element id (required)\n"
		"  x, y         coord   centre position (default: screen centre)\n"
		"  radius       int     circle radius in pixels (shorthand for rx)\n"
		"  rx, ry       int     ellipse radii in pixels (ry 0 = mirror rx)\n"
		"  thickness    int     0 = filled, >0 = outline ring width in pixels\n"
		"  color        color   fill / outline color\n"
		"  opacity      float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the element.\n");
	} else if (strcmp(cmd, "line") == 0) {
		fprintf(stderr,
		"line — add or update a straight line / divider\n\n"
		"  id           int     element id (required)\n"
		"  x1, y1       coord   start point\n"
		"  x2, y2       coord   end point\n"
		"  thickness    int     line width in pixels (default 2)\n"
		"  cap          int     0 = flat ends (default), 1 = round\n"
		"  color        color   line color\n"
		"  opacity      float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the element.\n");
	} else if (strcmp(cmd, "stepper") == 0) {
		fprintf(stderr,
		"stepper — add or update a step / boot-stage indicator\n\n"
		"  id           int     element id (required)\n"
		"  x, y         coord   row anchor (default: screen centre)\n"
		"  align/valign 0-2     positioning anchor of the row\n"
		"  count        int     number of steps (default 3)\n"
		"  current      int     steps marked done, 0..count (default 0)\n"
		"  style        int     0 = dots (default), 1 = bars (pills)\n"
		"  size         int     dot radius / bar height in px (default 12)\n"
		"  length       int     bar length for style 1 (0 = size*3)\n"
		"  gap          int     spacing between steps in px\n"
		"  thickness    int     todo steps: 0 = filled, >0 = outline width\n"
		"  color_done   color   colour of completed steps\n"
		"  color_todo   color   colour of remaining steps\n"
		"  opacity      float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the element.\n");
	} else if (strcmp(cmd, "marquee") == 0) {
		fprintf(stderr,
		"marquee — add or update horizontally-scrolling text\n\n"
		"  id           int     element id (required)\n"
		"  x, y         coord   clip box position (default: screen centre)\n"
		"  w, h         coord   clip box size (default 400 x 40)\n"
		"  text         string  the text to scroll\n"
		"  font         int     font slot (default 0)\n"
		"  size         float   font size in px (0 = slot default)\n"
		"  color        color   text color\n"
		"  speed        int     scroll px/sec: >0 left, <0 right, 0 static (def 60)\n"
		"  gap          int     gap between repetitions in px (default 60)\n"
		"  opacity      float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the element.\n");
	} else if (strcmp(cmd, "sprite") == 0) {
		fprintf(stderr,
		"sprite — add or update a frame animation (cycled images)\n\n"
		"  id           int     element id (required)\n"
		"  frames       array   list of image paths, one per frame\n"
		"  x, y         coord   anchor position (default: screen centre)\n"
		"  w, h         coord   draw size (0 = native frame size)\n"
		"  align/valign 0-2     positioning anchor\n"
		"  filter       int     0=nearest 1=bilinear 2=bicubic 3=lanczos\n"
		"  fps          int     frames per second (default 12)\n"
		"  loop         bool    repeat (default true); false = play once, hold\n"
		"  opacity      float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged; \"frames\" reloads only\n"
		"when given (otherwise the running animation is kept). \"replace\":true\n"
		"resets to defaults; \"remove\":true deletes.\n");
	} else if (strcmp(cmd, "overlay") == 0) {
		fprintf(stderr,
		"overlay — add or update an image overlay\n\n"
		"  id           int     element id (required)\n"
		"  path         string  image file path (PNG / JPEG)\n"
		"  x, y         coord   anchor position\n"
		"  w, h         coord   display size (0 = auto from aspect ratio)\n"
		"  align/valign 0-2     positioning anchor\n"
		"  filter       int     0=nearest 1=bilinear 2=bicubic 3=lanczos\n"
		"  radius       int     rounded-corner clip radius in px (0 = none)\n"
		"  angle        float   rotation around the centre, degrees (0 = none)\n"
		"  tint         color   multiply tint; alpha = strength (omit = none)\n"
		"  opacity      float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value, and the image is kept unless a new path is given).\n"
		"\"replace\":true resets to defaults first; \"remove\":true deletes the\n"
		"element.\n");
	} else if (strcmp(cmd, "progress") == 0) {
		fprintf(stderr,
		"progress — create or reconfigure a horizontal progress bar\n\n"
		"  id              int    element id (required)\n"
		"  x, y            coord  anchor position\n"
		"  w, h            coord  size\n"
		"  align/valign    0-2    positioning anchor\n"
		"  value           float  fill level 0.0-1.0\n"
		"  style           int    built-in theme: 0=blue 1=green 2=amber\n"
		"                         3=red 4=purple 5=cyan  (-1=custom)\n"
		"  bg_color        color  background track\n"
		"  bar_color       color  fill\n"
		"  bar_color2      color  gradient second stop\n"
		"  bar_gradient    int    gradient direction (GRAD_*)\n"
		"  border_color    color  border\n"
		"  text_color      color  percentage text\n"
		"  border_width    int    border thickness (0 = none)\n"
		"  radius          int    corner radius\n"
		"  font_slot       int    font for percentage label\n"
		"  font_size       float  size of percentage label\n"
		"  show_percent    bool   show percentage text\n"
		"  indeterminate   bool   sweeping highlight mode\n"
		"  indet_period_ms int    sweep cycle time in ms\n"
		"  shadow          bool   drop shadow on the whole bar\n"
		"  opacity         float  master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the bar. To update only the value, re-send with the id and\n"
		"\"value\"; \"hidden\":true hides the bar without losing its state.\n");
	} else if (strcmp(cmd, "arc") == 0) {
		fprintf(stderr,
		"arc — create or reconfigure a circular/arc progress bar\n\n"
		"  id              int    element id (required)\n"
		"  x, y            coord  centre (-1 = screen centre on that axis)\n"
		"  radius          int    outer radius in pixels\n"
		"  thickness       int    stroke width (0 = radius/4)\n"
		"  value           float  fill level 0.0-1.0\n"
		"  start_angle     float  start angle in degrees; 0=right, CW\n"
		"                         default: -90 (top of circle)\n"
		"  sweep           float  total arc in degrees (default: 360)\n"
		"  bg_color        color  background (unfilled) arc; alpha 0 = hidden\n"
		"  bar_color       color  filled arc\n"
		"  bar_color2      color  gradient second stop\n"
		"  bar_gradient    int    0=solid, 1=sweep gradient along the arc\n"
		"  cap             int    0=flat ends 1=round end caps\n"
		"  font_slot       int    font for centre label\n"
		"  font_size       float  size of centre label\n"
		"  text_color      color  centre label color\n"
		"  show_percent    bool   show percentage in the centre\n"
		"  indeterminate   bool   spinning highlight mode\n"
		"  indet_period_ms int    rotation cycle time in ms (default: 1200)\n"
		"  opacity         float  master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the arc. To update only the value, re-send with the id and\n"
		"\"value\"; \"hidden\":true hides the arc without losing its state.\n");
	} else if (strcmp(cmd, "spinner") == 0) {
		fprintf(stderr,
		"spinner — create/show/hide an Apple-style rotating spinner\n\n"
		"  id          int     element id (required)\n"
		"  x, y        coord   centre (-1 = screen centre on that axis)\n"
		"  radius      int     outer radius in pixels (default: 36)\n"
		"  spokes      int     number of spokes (default: 12)\n"
		"  color       color   spoke color (default: white)\n"
		"  period      int     full rotation time in ms (default: 900)\n"
		"  action      string  show, hide, show_animated, hide_animated\n"
		"                      (default: show)\n"
		"  duration    int     fade time in ms for the *_animated actions\n"
		"                      (default: 300)\n"
		"  easing      string  easing for the *_animated actions\n"
		"                      (linear, ease_in, ease_out, ease_in_out)\n"
		"  opacity     float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the spinner; action:\"hide\" instead keeps the slot configured\n"
		"for a later show.\n");
	} else if (strcmp(cmd, "console") == 0) {
		fprintf(stderr,
		"console — create or reconfigure a scrolling log area\n\n"
		"  id          int     element id (required)\n"
		"  x, y        coord   top-left position (-1 = centred)\n"
		"  w, h        coord   width and height\n"
		"  font_slot   int     font slot\n"
		"  size        float   font size in pixels\n"
		"  color       color   text color\n"
		"  bg_color    color   background fill; alpha 0 = transparent\n"
		"  padding     int     inner margin in pixels\n"
		"  autofit     bool    snap drawn height down to whole text rows so a\n"
		"                      full log fills the box with no leftover gap\n"
		"  max_lines   int     ring buffer capacity (default & max: 64)\n"
		"  opacity     float   master alpha 0.0-1.0\n\n"
		"  write       string  append a log line; \\n splits into multiple lines\n\n"
		"On an existing id, supplied console fields are merged (others keep\n"
		"their current value). \"replace\":true resets to defaults first;\n"
		"\"remove\":true deletes the console. When \"write\" is present, \"color\"\n"
		"colours that line instead of setting the console's default text colour.\n");
	} else if (strcmp(cmd, "qr") == 0) {
		fprintf(stderr,
		"qr — create or update a QR code element\n\n"
		"  id          int     element id (required)\n"
		"  text        string  payload to encode (required)\n"
		"  x, y        coord   top-left position (-1 = centred on that axis)\n"
		"  align       0-2     horizontal anchor on x\n"
		"  valign      0-2     vertical anchor on y\n"
		"  module_px   int     pixels per QR module (0 = auto)\n"
		"  border      int     quiet zone in modules (default: 4)\n"
		"  ecc         int     error correction: 0=low 1=medium 2=quartile 3=high\n"
		"  color       color   dark module color (default: black)\n"
		"  bg_color    color   light background; alpha 0 = transparent\n"
		"  opacity     float   master alpha 0.0-1.0\n\n"
		"On an existing id, supplied fields are merged (others keep their\n"
		"current value). \"replace\":true resets to defaults first; \"remove\":true\n"
		"deletes the element.\n");
	} else if (strcmp(cmd, "image") == 0) {
		fprintf(stderr,
		"image — set the background image\n\n"
		"  path        string  image file path (PNG / JPEG)\n"
		"  mode        int     0=cover 1=contain 2=stretch 3=none 4=custom\n"
		"  scale       float   scale factor (mode=4 only)\n"
		"  filter      int     0=nearest 1=bilinear 2=bicubic 3=lanczos\n"
		"  crossfade   int     fade duration in ms (0 = instant)\n");
	} else if (strcmp(cmd, "animate") == 0) {
		fprintf(stderr,
		"animate — tween a property of an element\n\n"
		"Not a standalone command: an \"animate\" object on an element op, e.g.\n"
		"  {\"text\":{\"id\":0,\"animate\":{\"to\":0,\"duration\":400}}}\n\n"
		"  property     string  opacity (default), x, y, w, h, or color\n"
		"  from         float   start value (default: the element's current)\n"
		"  to           float   end value\n"
		"  duration     int     duration in ms\n"
		"  easing       string  linear, ease_in, ease_out, ease_in_out\n"
		"  repeat       bool    loop animation (ping-pong)\n"
		"  remove_on_end bool   deactivate element when animation ends\n");
	} else if (strcmp(cmd, "query") == 0) {
		fprintf(stderr,
		"query — read back current element state\n\n"
		"  type        string  element type: text, rect, overlay, progress,\n"
		"                      arc, spinner, console, qr\n"
		"  id          int     element id\n\n"
		"Returns the element's current position, value/text, opacity,\n"
		"and type-specific fields (e.g. value for arc/progress, text\n"
		"for text/qr, line_count for console).\n");
	} else if (strcmp(cmd, "status") == 0) {
		fprintf(stderr,
		"status — query daemon state\n\n"
		"Returns:\n"
		"  version     string  running daemon's version (since 4.0.1)\n"
		"  state       string  \"running\" or \"suspended\"\n"
		"  ready       bool    set by the ready command\n"
		"  hidden      bool    true if ESC-toggled to blank\n"
		"  width       int     display width in pixels\n"
		"  height      int     display height in pixels\n");
	} else if (strcmp(cmd, "version") == 0) {
		fprintf(stderr,
		"version — report the running daemon's version (since 4.0.1)\n\n"
		"Takes no parameters. Returns:\n"
		"  version     string  the daemon's compiled version\n\n"
		"Use this to confirm which build is actually live: after upgrading the\n"
		"on-disk binaries, a stale initramfs can keep an old daemon running, so\n"
		"newer commands silently do nothing. Query it over the socket (e.g. via\n"
		"SSH when the screen is blocked) to catch that.\n\n"
		"The 'splash-ctl --daemon-version' shortcut prints it directly. A daemon\n"
		"older than 4.0.1 replies \"unknown command\", which is itself the answer.\n");
	} else if (strcmp(cmd, "running") == 0) {
		fprintf(stderr,
		"running — liveness probe (since 4.0.2)\n\n"
		"Takes no parameters. A live daemon always replies:\n"
		"  running     bool    always true (only a running daemon can answer)\n\n"
		"Meant for the 'splash-ctl --running' / 'splash-ctl '{\"system\":\"running\"}''\n"
		"shortcuts, which report the daemon's state without ever failing on a\n"
		"connection error: --running prints 'running' or 'not running' and exits\n"
		"0 or 1; the raw command always prints {\"running\":true} or\n"
		"{\"running\":false}, so a script gets a boolean even when the daemon is\n"
		"down.\n");
	} else if (strcmp(cmd, "exit") == 0) {
		fprintf(stderr,
		"exit — request a clean shutdown of the daemon\n\n"
		"A system op: {\"system\":\"exit\"} or, with a delay,\n"
		"{\"system\":{\"action\":\"exit\",\"timeout\":N}}.\n\n"
		"  timeout     int     seconds to keep rendering before exiting (default 0)\n\n"
		"Without 'timeout' the daemon exits immediately after sending the reply.\n"
		"With 'timeout' the daemon keeps rendering for that many seconds, then\n"
		"shuts down. Animations and commands continue to work during the wait,\n"
		"so you can display a final message or fade out before the process ends.\n\n"
		"Example: show 'System ready' and exit after 3 seconds\n"
		"  splash-ctl '{\"text\":{\"id\":99,\"text\":\"System ready\"}}'\n"
		"  splash-ctl '{\"system\":{\"action\":\"exit\",\"timeout\":3}}'\n");
	} else {
		fprintf(stderr, "No detailed help available for '%s'.\n"
		                "Run with -h for the command list.\n", cmd);
	}
}
