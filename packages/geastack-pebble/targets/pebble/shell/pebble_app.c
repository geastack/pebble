// SPDX-License-Identifier: Apache-2.0
// The PebbleOS side of a Gea app: one full-screen window whose layer the Gea
// program paints, the system fonts it sets text in, and the buttons, touch
// and timers that drive it. This is the only file that includes the SDK.
//
// The program is a position-independent blob (see pebble_bridge.h), compiled
// into this image as data; this file relocates it and drives it through
// GeaPebbleProgram.
#include <pebble.h>

#include "pebble_bridge.h"

static const GeaPebbleProgram *s_program;
static Window *s_window;
static Layer *s_layer;
static AppTimer *s_frame_timer;

// ---- Fonts ----------------------------------------------------------------

typedef struct {
	const char *key;
	int16_t size;
	bool bold;
} FontChoice;

// The full-alphabet system fonts, by the CSS pixel size each best stands in
// for. Number-only faces (LECO, BITHAM *_NUMBERS) are left out: a font picked
// by size alone has to be able to set whatever string it is handed.
static const FontChoice s_fonts[] = {
	{FONT_KEY_GOTHIC_09, 9, false},
	{FONT_KEY_GOTHIC_14, 14, false},
	{FONT_KEY_GOTHIC_14_BOLD, 14, true},
	{FONT_KEY_GOTHIC_18, 18, false},
	{FONT_KEY_GOTHIC_18_BOLD, 18, true},
	{FONT_KEY_ROBOTO_CONDENSED_21, 21, false},
	{FONT_KEY_GOTHIC_24, 24, false},
	{FONT_KEY_GOTHIC_24_BOLD, 24, true},
	{FONT_KEY_GOTHIC_28, 28, false},
	{FONT_KEY_GOTHIC_28_BOLD, 28, true},
	{FONT_KEY_BITHAM_30_BLACK, 30, true},
	{FONT_KEY_BITHAM_42_LIGHT, 42, false},
	{FONT_KEY_BITHAM_42_BOLD, 42, true},
};
#define FONT_COUNT ((int)(sizeof(s_fonts) / sizeof(s_fonts[0])))
static GFont s_loaded[FONT_COUNT];

static GFont font_at(int index)
{
	if (index < 0 || index >= FONT_COUNT) index = 3;
	if (!s_loaded[index]) s_loaded[index] = fonts_get_system_font(s_fonts[index].key);
	return s_loaded[index];
}

static int host_font_for(int size_px, int weight)
{
	const bool bold = weight >= 600;
	int best = 3;
	int best_score = 1 << 30;
	for (int i = 0; i < FONT_COUNT; ++i) {
		const int delta = s_fonts[i].size - size_px;
		// Never pick a face noticeably larger than asked when a smaller one
		// exists: text that overflows its box reads worse than text that is
		// slightly small.
		int score = (delta > 2 ? delta * 3 : (delta < 0 ? -delta : delta)) * 4;
		if (s_fonts[i].bold != bold) score += 3;
		if (score < best_score) {
			best_score = score;
			best = i;
		}
	}
	return best;
}

static void host_measure_text(const char *text, int font, int max_width, int *out_width, int *out_height)
{
	const int16_t bound = max_width > 0 ? (int16_t)max_width : 2000;
	const GSize size = graphics_text_layout_get_content_size(text ? text : "", font_at(font), GRect(0, 0, bound, 2000),
	                                                         GTextOverflowModeWordWrap, GTextAlignmentLeft);
	*out_width = size.w;
	*out_height = size.h;
}

// ---- Drawing --------------------------------------------------------------

static GCornerMask corners_for(int radius)
{
	return radius > 0 ? GCornersAll : GCornerNone;
}

static void host_fill_rect(void *ctx, int x, int y, int w, int h, int radius, uint8_t color)
{
	GContext *g = (GContext *)ctx;
	graphics_context_set_fill_color(g, (GColor8){.argb = color});
	// A square whose radius is half its side is a circle (`border-radius:
	// 50%`). The firmware's rounded rectangle draws its corners from a table
	// that leaves such a box visibly square; a radial fill of the circle the
	// box fits is the circle CSS draws.
	if (w == h && w > 0 && 2 * radius >= w) {
		graphics_fill_radial(g, GRect(x, y, w, h), GOvalScaleModeFitCircle, (uint16_t)(w / 2 + 1), 0, TRIG_MAX_ANGLE);
		return;
	}
	graphics_fill_rect(g, GRect(x, y, w, h), (uint16_t)radius, corners_for(radius));
}

static void host_stroke_rect(void *ctx, int x, int y, int w, int h, int radius, int width, uint8_t color)
{
	GContext *g = (GContext *)ctx;
	graphics_context_set_stroke_color(g, (GColor8){.argb = color});
	graphics_context_set_stroke_width(g, (uint8_t)(width < 1 ? 1 : width));
	// Pebble strokes are centered on the path; inset by half the width so the
	// border lies inside the box, as CSS draws it.
	const int inset = width / 2;
	graphics_draw_round_rect(g, GRect(x + inset, y + inset, w - 2 * inset, h - 2 * inset), (uint16_t)radius);
}

static void host_draw_text(void *ctx, const char *text, int font, int x, int y, int w, int h, int align, uint8_t color)
{
	GContext *g = (GContext *)ctx;
	GTextAlignment alignment = GTextAlignmentLeft;
	if (align == GEA_PEBBLE_ALIGN_CENTER) alignment = GTextAlignmentCenter;
	else if (align == GEA_PEBBLE_ALIGN_RIGHT) alignment = GTextAlignmentRight;
	graphics_context_set_text_color(g, (GColor8){.argb = color});
	graphics_draw_text(g, text, font_at(font), GRect(x, y, w, h), GTextOverflowModeWordWrap, alignment, NULL);
}

// ---- Frame loop -----------------------------------------------------------

static uint32_t host_now_ms(void)
{
	time_t seconds = 0;
	uint16_t millis = 0;
	time_ms(&seconds, &millis);
	return (uint32_t)seconds * 1000u + millis;
}

static double host_epoch_ms(void)
{
	time_t seconds = 0;
	uint16_t millis = 0;
	time_ms(&seconds, &millis);
	return (double)seconds * 1000.0 + millis;
}

static uint64_t host_random_u64(void)
{
	// C guarantees at least 15 random bits per rand() call. Math.random needs
	// a full-width sample but makes no cryptographic guarantee.
	uint64_t value = 0;
	for (int i = 0; i < 5; ++i) value = (value << 15) | ((unsigned)rand() & 0x7fffu);
	return value;
}

static void host_log(const char *message)
{
	APP_LOG(APP_LOG_LEVEL_INFO, "%s", message);
}

static void host_request_redraw(void)
{
	if (s_layer) layer_mark_dirty(s_layer);
}

static void frame_tick(void *data)
{
	(void)data;
	s_frame_timer = NULL;
	s_program->ui_frame(host_now_ms());
}

static void host_request_frame(int delay_ms)
{
	if (delay_ms < 1) delay_ms = 1;
	if (s_frame_timer) {
		app_timer_reschedule(s_frame_timer, (uint32_t)delay_ms);
		return;
	}
	s_frame_timer = app_timer_register((uint32_t)delay_ms, frame_tick, NULL);
}

static void layer_update(Layer *layer, GContext *ctx)
{
	(void)layer;
	s_program->ui_draw(ctx);
}

// ---- Input ----------------------------------------------------------------

static void button_handler(ClickRecognizerRef recognizer, void *context)
{
	(void)context;
	s_program->ui_button((int)click_recognizer_get_button_id(recognizer));
}

static void click_config(void *context)
{
	(void)context;
	window_single_click_subscribe(BUTTON_ID_UP, button_handler);
	window_single_click_subscribe(BUTTON_ID_SELECT, button_handler);
	window_single_click_subscribe(BUTTON_ID_DOWN, button_handler);
}

#if defined(PBL_TOUCH)
static void touch_handler(const TouchEvent *event, void *context)
{
	(void)context;
	int phase = GEA_PEBBLE_TOUCH_MOVE;
	if (event->type == TouchEvent_Touchdown) phase = GEA_PEBBLE_TOUCH_DOWN;
	else if (event->type == TouchEvent_Liftoff) phase = GEA_PEBBLE_TOUCH_UP;
	s_program->ui_touch(phase, event->x, event->y);
}
#endif

// ---- Program loading ------------------------------------------------------

static void host_abort(void)
{
	APP_LOG(APP_LOG_LEVEL_ERROR, "gea: program aborted");
	// A fault on the app task is how PebbleOS ends a crashed app: it reports
	// it and reclaims everything the app held.
	__builtin_trap();
}

static GeaPebbleHost s_host = {
	.abi = GEA_PEBBLE_ABI,
	.malloc = malloc,
	.realloc = realloc,
	.free = free,
	.log = host_log,
	.abort = host_abort,
	.now_ms = host_now_ms,
	.epoch_ms = host_epoch_ms,
	.random_u64 = host_random_u64,
	.font_for = host_font_for,
	.measure_text = host_measure_text,
	.fill_rect = host_fill_rect,
	.stroke_rect = host_stroke_rect,
	.draw_text = host_draw_text,
	.request_redraw = host_request_redraw,
	.request_frame = host_request_frame,
};

// The program is compiled into the app image (tools/pack-program.mjs writes
// gea_program_image.inc): the firmware loads it as part of the app, into memory
// it has made executable, and the shell only binds the image to where it
// landed.
#include "gea_program_image.inc"

static bool load_program(void)
{
	uint8_t *memory = (uint8_t *)gea_program_memory;
	for (uint32_t i = 0; i < GEA_PEBBLE_PROGRAM_RELOC_COUNT; ++i) {
		const uint32_t offset = gea_program_relocs[i];
		if (offset > GEA_PEBBLE_PROGRAM_IMAGE_SIZE - sizeof(uint32_t) || (offset & 3u)) {
			APP_LOG(APP_LOG_LEVEL_ERROR, "gea: bad relocation %u", (unsigned)offset);
			return false;
		}
		*(uint32_t *)(memory + offset) += (uint32_t)(uintptr_t)memory;
	}
	s_program = (const GeaPebbleProgram *)memory;
	if (s_program->magic != GEA_PEBBLE_PROGRAM_MAGIC || s_program->abi != GEA_PEBBLE_ABI) {
		APP_LOG(APP_LOG_LEVEL_ERROR, "gea: program header mismatch");
		return false;
	}
	s_program->start(&s_host);
	APP_LOG(APP_LOG_LEVEL_DEBUG, "gea: program %u bytes, %u bytes of heap left", (unsigned)GEA_PEBBLE_PROGRAM_MEMORY_SIZE,
	        (unsigned)heap_bytes_free());
	return true;
}

// ---- Lifecycle ------------------------------------------------------------

static void window_load(Window *window)
{
	Layer *root = window_get_root_layer(window);
	const GRect bounds = layer_get_bounds(root);
	s_layer = layer_create(bounds);
	layer_set_update_proc(s_layer, layer_update);
	layer_add_child(root, s_layer);
	s_program->ui_init(bounds.size.w, bounds.size.h);
	s_program->ui_frame(host_now_ms());
}

static void window_unload(Window *window)
{
	(void)window;
	s_program->ui_deinit();
	layer_destroy(s_layer);
	s_layer = NULL;
}

int main(void)
{
	srand(host_now_ms());
	if (!load_program()) return 1;
	s_window = window_create();
	window_set_click_config_provider(s_window, click_config);
	window_set_window_handlers(s_window, (WindowHandlers){.load = window_load, .unload = window_unload});
	window_stack_push(s_window, true);
#if defined(PBL_TOUCH)
	touch_service_subscribe(touch_handler, NULL);
#endif
	app_event_loop();
#if defined(PBL_TOUCH)
	touch_service_unsubscribe();
#endif
	if (s_frame_timer) app_timer_cancel(s_frame_timer);
	window_destroy(s_window);
	return 0;
}
