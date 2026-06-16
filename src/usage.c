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
		"  opacity     float    master alpha 0.0-1.0\n");
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
		"  opacity      float   master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "overlay") == 0) {
		fprintf(stderr,
		"overlay — add or update an image overlay\n\n"
		"  id           int     element id (required)\n"
		"  path         string  image file path (PNG / JPEG)\n"
		"  x, y         coord   anchor position\n"
		"  w, h         coord   display size (0 = auto from aspect ratio)\n"
		"  align/valign 0-2     positioning anchor\n"
		"  filter       int     0=nearest 1=bilinear 2=bicubic 3=lanczos\n"
		"  opacity      float   master alpha 0.0-1.0\n");
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
		"  borderless      bool   suppress border\n"
		"  border_width    int    border thickness\n"
		"  radius          int    corner radius\n"
		"  font_slot       int    font for percentage label\n"
		"  font_size       float  size of percentage label\n"
		"  show_percent    bool   show percentage text\n"
		"  indeterminate   bool   sweeping highlight mode\n"
		"  indet_period_ms int    sweep cycle time in ms\n"
		"  shadow          bool   drop shadow on the whole bar\n"
		"  opacity         float  master alpha 0.0-1.0\n");
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
		"  opacity         float  master alpha 0.0-1.0\n");
	} else if (strcmp(cmd, "spinner") == 0) {
		fprintf(stderr,
		"spinner — create/show/hide an Apple-style rotating spinner\n\n"
		"  id          int     element id (required)\n"
		"  x, y        coord   centre (-1 = screen centre on that axis)\n"
		"  radius      int     outer radius in pixels (default: 36)\n"
		"  spokes      int     number of spokes (default: 12)\n"
		"  color       color   spoke color (default: white)\n"
		"  period      int     full rotation time in ms (default: 900)\n"
		"  action      string  \"show\" or \"hide\" (default: show)\n"
		"  opacity     float   master alpha 0.0-1.0\n");
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
		"  max_lines   int     ring buffer capacity (max: 64)\n"
		"  opacity     float   master alpha 0.0-1.0\n\n"
		"console_write — push text into a console element\n\n"
		"  id          int     console id (required)\n"
		"  text        string  text to append; \\n splits into multiple lines\n");
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
		"  opacity     float   master alpha 0.0-1.0\n");
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
		"animate — animate an element's opacity\n\n"
		"  type         string  element type: text, rect, overlay, progress,\n"
		"                       arc, spinner, console, qr\n"
		"  id           int     element id\n"
		"  from         float   start opacity (default: current)\n"
		"  to           float   end opacity\n"
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
		"  state       string  \"running\" or \"suspended\"\n"
		"  ready       bool    set by the ready command\n"
		"  hidden      bool    true if ESC-toggled to blank\n"
		"  width       int     display width in pixels\n"
		"  height      int     display height in pixels\n");
	} else if (strcmp(cmd, "exit") == 0) {
		fprintf(stderr,
		"exit — request a clean shutdown of the daemon\n\n"
		"  delay       int     seconds to wait before exiting (default: 0)\n\n"
		"Without 'delay' the daemon exits immediately after sending the reply.\n"
		"With 'delay' the daemon keeps rendering for that many seconds, then\n"
		"shuts down. Animations and commands continue to work during the delay,\n"
		"so you can display a final message or fade out before the process ends.\n\n"
		"Example: show 'System ready' and exit after 3 seconds\n"
		"  splash-ctl '{\"cmd\":\"text\",\"id\":99,\"text\":\"System ready\"}'\n"
		"  splash-ctl '{\"cmd\":\"exit\",\"delay\":3}'\n");
	} else {
		fprintf(stderr, "No detailed help available for '%s'.\n"
		                "Run with -h for the command list.\n", cmd);
	}
}
