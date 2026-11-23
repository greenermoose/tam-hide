/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "render.h"

#include <cairo/cairo.h>
#include <math.h>
#include <pango/pangocairo.h>
#include <stdio.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static void draw_rounded_rect(cairo_t *cr, double x, double y, double w,
							  double h, double r) {
	if (r > h / 2.0)
		r = h / 2.0;
	if (r > w / 2.0)
		r = w / 2.0;
	cairo_new_sub_path(cr);
	cairo_arc(cr, x + w - r, y + r, r, -M_PI / 2.0, 0.0);
	cairo_arc(cr, x + w - r, y + h - r, r, 0.0, M_PI / 2.0);
	cairo_arc(cr, x + r, y + h - r, r, M_PI / 2.0, M_PI);
	cairo_arc(cr, x + r, y + r, r, M_PI, 3.0 * M_PI / 2.0);
	cairo_close_path(cr);
}

static void render_picker(cairo_t *cr, double w, double h,
						  const struct lock_state *st, int64_t now_ms) {
	double cx = w / 2.0;
	double cy = h / 2.0;
	double row_h = 38.0;
	double pad = 18.0;
	double title_h = 30.0;
	size_t n = st->pick_count;
	if (n > 9)
		n = 9;

	double card_w = (w < 420.0) ? (w * 0.86) : 360.0;
	double card_h = pad * 2.0 + title_h + (double)n * row_h;
	double card_x = cx - card_w / 2.0;
	double card_y = cy - card_h / 2.0;

	draw_rounded_rect(cr, card_x, card_y, card_w, card_h, 14.0);
	cairo_set_source_rgb(cr, 0.098, 0.125, 0.157);
	cairo_fill_preserve(cr);
	cairo_set_source_rgb(cr, 0.36, 0.62, 0.88);
	cairo_set_line_width(cr, 2.0);
	cairo_stroke(cr);

	PangoLayout *layout = pango_cairo_create_layout(cr);
	PangoFontDescription *title_font =
		pango_font_description_from_string("Sans Bold 12");
	PangoFontDescription *item_font =
		pango_font_description_from_string("Sans 12");
	PangoFontDescription *hint_font =
		pango_font_description_from_string("Sans 10");

	pango_layout_set_font_description(layout, title_font);
	pango_layout_set_text(layout, "Select session", -1);
	cairo_set_source_rgb(cr, 0.88, 0.91, 0.94);
	cairo_move_to(cr, card_x + pad, card_y + pad);
	pango_cairo_show_layout(cr, layout);

	pango_layout_set_font_description(layout, item_font);
	for (size_t i = 0; i < n; i++) {
		double ry = card_y + pad + title_h + (double)i * row_h;
		bool sel = (i == st->pick_selected);
		if (sel) {
			draw_rounded_rect(cr, card_x + pad - 6.0, ry,
							  card_w - (pad - 6.0) * 2.0, row_h - 4.0, 8.0);
			cairo_set_source_rgb(cr, 0.18, 0.31, 0.45);
			cairo_fill(cr);
		}

		char label[128];
		const struct lock_pick_item *it = &st->pick_items[i];
		if (it->vt_nr > 0) {
			snprintf(label, sizeof(label), "%zu. %s (tty%d)", i + 1,
					 it->username, it->vt_nr);
		} else {
			snprintf(label, sizeof(label), "%zu. %s (new)", i + 1,
					 it->username);
		}
		pango_layout_set_text(layout, label, -1);
		if (sel)
			cairo_set_source_rgb(cr, 0.96, 0.98, 1.0);
		else
			cairo_set_source_rgb(cr, 0.78, 0.82, 0.86);
		cairo_move_to(cr, card_x + pad + 4.0, ry + 7.0);
		pango_cairo_show_layout(cr, layout);
	}

	char status[96];
	lock_state_status_text(st, now_ms, status, sizeof(status));
	if (status[0] != '\0') {
		pango_layout_set_font_description(layout, hint_font);
		pango_layout_set_text(layout, status, -1);
		int tw = 0, th = 0;
		pango_layout_get_pixel_size(layout, &tw, &th);
		cairo_set_source_rgb(cr, 0.65, 0.70, 0.76);
		cairo_move_to(cr, cx - (double)tw / 2.0, card_y + card_h + 14.0);
		pango_cairo_show_layout(cr, layout);
	}

	pango_font_description_free(hint_font);
	pango_font_description_free(item_font);
	pango_font_description_free(title_font);
	g_object_unref(layout);
}

void lock_render_frame(uint32_t *pixels, uint32_t logical_w, uint32_t logical_h,
					   int32_t scale, const struct lock_state *st,
					   int64_t now_ms, bool blanked) {
	if (!pixels || logical_w == 0 || logical_h == 0)
		return;
	if (scale < 1)
		scale = 1;

	int phys_w = (int)logical_w * scale;
	int phys_h = (int)logical_h * scale;
	int stride = phys_w * 4;

	if (blanked) {
		memset(pixels, 0, (size_t)stride * (size_t)phys_h);
		return;
	}

	cairo_surface_t *surf = cairo_image_surface_create_for_data(
		(unsigned char *)pixels, CAIRO_FORMAT_RGB24, phys_w, phys_h, stride);
	if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
		cairo_surface_destroy(surf);
		return;
	}

	cairo_t *cr = cairo_create(surf);
	cairo_scale(cr, (double)scale, (double)scale);

	double w = (double)logical_w;
	double h = (double)logical_h;
	double cx = w / 2.0;
	double cy = h / 2.0;

	/* Solid dark slate background — non-disclosing full-output cover. */
	cairo_set_source_rgb(cr, 0.059, 0.078, 0.098); /* #0f1419 */
	cairo_paint(cr);

	enum lock_ui_state ui = st ? st->ui_state : LOCK_UI_IDLE;
	if (ui == LOCK_UI_PICKER && st && st->pick_count > 0) {
		render_picker(cr, w, h, st, now_ms);
		cairo_destroy(cr);
		cairo_surface_flush(surf);
		cairo_surface_destroy(surf);
		return;
	}

	/* Centered passcode pill. */
	double pill_w = (w < 360.0) ? (w * 0.78) : 280.0;
	double pill_h = 48.0;
	double pill_x = cx - pill_w / 2.0;
	double pill_y = cy - pill_h / 2.0;

	/* Pill fill */
	draw_rounded_rect(cr, pill_x, pill_y, pill_w, pill_h, 24.0);
	cairo_set_source_rgb(cr, 0.098, 0.125, 0.157); /* #192028 */
	cairo_fill_preserve(cr);

	/* Accent border reflects UI state cleanly without leaking account info. */
	switch (ui) {
	case LOCK_UI_TYPING:
		cairo_set_source_rgb(cr, 0.36, 0.62, 0.88);
		break;
	case LOCK_UI_VERIFYING:
		cairo_set_source_rgb(cr, 0.85, 0.68, 0.28);
		break;
	case LOCK_UI_FAILED:
	case LOCK_UI_LOCKOUT:
		cairo_set_source_rgb(cr, 0.82, 0.30, 0.30);
		break;
	case LOCK_UI_IDLE:
	default:
		cairo_set_source_rgb(cr, 0.24, 0.30, 0.36);
		break;
	}
	cairo_set_line_width(cr, 2.0);
	cairo_stroke(cr);

	/* Passcode dot indicators inside pill. */
	size_t dots = st ? st->char_count : 0;
	const size_t max_visible_dots = 16;
	size_t shown = (dots > max_visible_dots) ? max_visible_dots : dots;
	if (shown > 0) {
		double dot_r = 4.5;
		double spacing = 14.0;
		double total_w = (double)(shown - 1) * spacing;
		double start_x = cx - total_w / 2.0;
		cairo_set_source_rgb(cr, 0.88, 0.91, 0.94);
		for (size_t i = 0; i < shown; i++) {
			cairo_new_sub_path(cr);
			cairo_arc(cr, start_x + (double)i * spacing, cy, dot_r, 0.0,
					  2.0 * M_PI);
			cairo_fill(cr);
		}
	}

	/* Optional non-disclosing status text below pill. */
	char status[96];
	lock_state_status_text(st, now_ms, status, sizeof(status));
	if (status[0] != '\0') {
		PangoLayout *layout = pango_cairo_create_layout(cr);
		PangoFontDescription *desc =
			pango_font_description_from_string("Sans 12");
		pango_layout_set_font_description(layout, desc);
		pango_layout_set_text(layout, status, -1);

		int tw = 0, th = 0;
		pango_layout_get_pixel_size(layout, &tw, &th);
		if (ui == LOCK_UI_FAILED || ui == LOCK_UI_LOCKOUT)
			cairo_set_source_rgb(cr, 0.88, 0.42, 0.42);
		else if (ui == LOCK_UI_VERIFYING)
			cairo_set_source_rgb(cr, 0.88, 0.76, 0.44);
		else
			cairo_set_source_rgb(cr, 0.65, 0.70, 0.76);

		cairo_move_to(cr, cx - (double)tw / 2.0, pill_y + pill_h + 16.0);
		pango_cairo_show_layout(cr, layout);

		pango_font_description_free(desc);
		g_object_unref(layout);
	}

	cairo_destroy(cr);
	cairo_surface_flush(surf);
	cairo_surface_destroy(surf);
}
