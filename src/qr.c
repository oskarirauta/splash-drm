/*
 * qr.c - QR code element rendering using the nayuki/qrcodegen library.
 *
 * Encodes the element's text payload each frame (the result is tiny and
 * encoding is fast), then draws each module as a filled rectangle scaled
 * by module_px. The quiet zone (border modules) is rendered in bg_color.
 */

#include "splash.h"
#include "qrcodegen.h"

static const enum qrcodegen_Ecc ecc_map[] = {
	qrcodegen_Ecc_LOW,
	qrcodegen_Ecc_MEDIUM,
	qrcodegen_Ecc_QUARTILE,
	qrcodegen_Ecc_HIGH,
};

void draw_qr_element(drm_buffer_t *buf, qr_element_t *qr) {
	if (!qr->text[0])
		return;

	uint8_t qrcode[qrcodegen_BUFFER_LEN_MAX];
	uint8_t tmp[qrcodegen_BUFFER_LEN_MAX];

	int ecc_idx = qr->ecc;
	if (ecc_idx < 0 || ecc_idx > 3)
		ecc_idx = 1;

	if (!qrcodegen_encodeText(qr->text, tmp, qrcode, ecc_map[ecc_idx],
	                          qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
	                          qrcodegen_Mask_AUTO, true))
		return;

	int qr_sz   = qrcodegen_getSize(qrcode);    /* modules, no border */
	int border  = qr->border > 0 ? qr->border : 4;
	int total   = qr_sz + border * 2;           /* total side in modules */

	/* Auto module size: fill roughly 1/4 of the smaller screen dimension. */
	int mod_px = qr->module_px;
	if (mod_px <= 0) {
		int ref = (int)((buf->width < buf->height) ? buf->width : buf->height);
		mod_px = ref / 4 / total;
		if (mod_px < 1)
			mod_px = 1;
	}

	int px_total = total * mod_px;   /* rendered size in pixels */

	/* Anchor position. Negative value = screen centre on that axis. */
	int x = (qr->x < 0) ? ((int)buf->width  - px_total) / 2 : qr->x;
	int y = (qr->y < 0) ? ((int)buf->height - px_total) / 2 : qr->y;

	if (qr->align == ALIGN_CENTER)
		x -= px_total / 2;
	else if (qr->align == ALIGN_RIGHT)
		x -= px_total;

	if (qr->valign == VALIGN_MIDDLE)
		y -= px_total / 2;
	else if (qr->valign == VALIGN_BOTTOM)
		y -= px_total;

	float op = qr->opacity;
	if (op <= 0.0f)
		return;
	if (op > 1.0f)
		op = 1.0f;

	uint32_t dark  = apply_opacity(qr->color,    op);
	uint32_t light = apply_opacity(qr->bg_color, op);

	/* Background fill for the entire QR area (quiet zone + data). */
	if ((light >> 24) > 0)
		draw_filled_rect(buf, x, y, px_total, px_total, light);

	/* Draw each module. */
	for (int my = 0; my < qr_sz; my++) {
		for (int mx = 0; mx < qr_sz; mx++) {
			if (!qrcodegen_getModule(qrcode, mx, my))
				continue;

			int px = x + (border + mx) * mod_px;
			int py = y + (border + my) * mod_px;
			draw_filled_rect(buf, px, py, mod_px, mod_px, dark);
		}
	}
}
