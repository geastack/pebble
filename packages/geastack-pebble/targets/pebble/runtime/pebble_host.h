// SPDX-License-Identifier: Apache-2.0
// The blob's view of the shell: the host table `start` binds, and the calls
// the runtime makes through it under the names it has always used.
#pragma once

#include "pebble_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

extern const GeaPebbleHost *gea_pebble_host;

static inline int gea_pebble_font_for(int size_px, int weight) { return gea_pebble_host->font_for(size_px, weight); }
static inline void gea_pebble_measure_text(const char *text, int font, int max_width, int *out_width, int *out_height)
{
	gea_pebble_host->measure_text(text, font, max_width, out_width, out_height);
}
static inline void gea_pebble_fill_rect(void *ctx, int x, int y, int w, int h, int radius, uint8_t color)
{
	gea_pebble_host->fill_rect(ctx, x, y, w, h, radius, color);
}
static inline void gea_pebble_stroke_rect(void *ctx, int x, int y, int w, int h, int radius, int width, uint8_t color)
{
	gea_pebble_host->stroke_rect(ctx, x, y, w, h, radius, width, color);
}
static inline void gea_pebble_draw_text(void *ctx, const char *text, int font, int x, int y, int w, int h, int align, uint8_t color)
{
	gea_pebble_host->draw_text(ctx, text, font, x, y, w, h, align, color);
}
static inline void gea_pebble_request_redraw(void) { gea_pebble_host->request_redraw(); }
static inline void gea_pebble_request_frame(int delay_ms) { gea_pebble_host->request_frame(delay_ms); }
static inline uint32_t gea_pebble_now_ms(void) { return gea_pebble_host->now_ms(); }
static inline void gea_pebble_log(const char *message) { gea_pebble_host->log(message); }

// The program's side, which pebble_program.cpp publishes in its header.
void gea_pebble_ui_init(int width, int height);
void gea_pebble_ui_frame(uint32_t now_ms);
void gea_pebble_ui_draw(void *ctx);
void gea_pebble_ui_button(int button);
void gea_pebble_ui_touch(int phase, int x, int y);
void gea_pebble_ui_deinit(void);

// pebble_services.cpp: runs the animation-frame callbacks queued before this
// frame; false when none were.
bool gea_pebble_run_animation_frames(uint32_t now_ms);

#ifdef __cplusplus
}
#endif
