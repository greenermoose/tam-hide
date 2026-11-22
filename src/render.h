/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef TAM_HIDE_RENDER_H
#define TAM_HIDE_RENDER_H

#include "lock_state.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * Render the minimal, non-disclosing lock screen into an XRGB8888 pixel buffer.
 * When blanked is true, fills the buffer with pure black (#000000) so outputs
 * without hardware DPMS (such as nested Wayland windows) still blank cleanly.
 * logical_w / logical_h are in Wayland surface coordinates; scale >= 1 is the
 * integer buffer scale, so pixels must hold (logical_w * scale) * (logical_h * scale)
 * 32-bit pixels with stride = logical_w * scale * 4.
 */
void lock_render_frame(uint32_t *pixels, uint32_t logical_w, uint32_t logical_h,
					   int32_t scale, const struct lock_state *st,
					   int64_t now_ms, bool blanked);

#endif
