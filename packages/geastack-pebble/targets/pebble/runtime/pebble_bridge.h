// SPDX-License-Identifier: Apache-2.0
// The seam between the Pebble app shell (pebble_app.c, the only code that
// includes the Pebble SDK) and the Gea program (everything else).
//
// The program is a position-independent blob, linked apart from the shell
// (by LLVM, against its own libc pieces) and compiled into the app image as
// one writable array (tools/pack-program.mjs). The shell relocates it where
// the firmware loaded it and calls it through the tables below. The app
// image, program included, is capped at 64 KB (`PebbleProcessInfo.load_size`
// is a uint16_t). The shell is the only code linked against the SDK; the blob
// reaches PebbleOS solely through `GeaPebbleHost`, so neither side depends on
// the other's link.
//
// Everything that crosses is a plain C type. Colors are GColor8 argb bytes.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Text alignment, in the order the engine's TextAlign property encodes it.
enum { GEA_PEBBLE_ALIGN_LEFT = 0, GEA_PEBBLE_ALIGN_CENTER = 1, GEA_PEBBLE_ALIGN_RIGHT = 2 };

// Buttons, as PebbleOS numbers them.
enum { GEA_PEBBLE_BUTTON_BACK = 0, GEA_PEBBLE_BUTTON_UP = 1, GEA_PEBBLE_BUTTON_SELECT = 2, GEA_PEBBLE_BUTTON_DOWN = 3 };

// Touch phases, as the TouchService reports them.
enum { GEA_PEBBLE_TOUCH_DOWN = 0, GEA_PEBBLE_TOUCH_UP = 1, GEA_PEBBLE_TOUCH_MOVE = 2 };

// Bumped whenever either table changes shape; the shell refuses a blob built
// against another version rather than calling through a mismatched table.
#define GEA_PEBBLE_ABI 2
#define GEA_PEBBLE_PROGRAM_MAGIC 0x50414547u  // "GEAP"

// What the shell provides. Drawing calls are valid only inside `draw`.
typedef struct GeaPebbleHost {
	uint32_t abi;
	void *(*malloc)(size_t size);
	void *(*realloc)(void *pointer, size_t size);
	void (*free)(void *pointer);
	void (*log)(const char *message);
	void (*abort)(void);
	// Milliseconds on a free-running clock, for frame timing; wraps.
	uint32_t (*now_ms)(void);
	// Wall-clock time, in milliseconds since the Unix epoch (`Date.now()`).
	double (*epoch_ms)(void);
	// Seeded platform PRNG for Math.random, not cryptographic entropy.
	uint64_t (*random_u64)(void);
	// The system font closest to a CSS font size (px) and weight (100-900).
	int (*font_for)(int size_px, int weight);
	// Size of `text` set in `font`, wrapped to `max_width` (<= 0: unbounded).
	void (*measure_text)(const char *text, int font, int max_width, int *out_width, int *out_height);
	void (*fill_rect)(void *ctx, int x, int y, int w, int h, int radius, uint8_t color);
	void (*stroke_rect)(void *ctx, int x, int y, int w, int h, int radius, int width, uint8_t color);
	void (*draw_text)(void *ctx, const char *text, int font, int x, int y, int w, int h, int align, uint8_t color);
	void (*request_redraw)(void);
	void (*request_frame)(int delay_ms);
} GeaPebbleHost;

// What the blob provides, at offset 0 of its image. Every pointer in it is
// relocated with the rest of the image.
typedef struct GeaPebbleProgram {
	uint32_t magic;
	uint32_t abi;
	// Binds the host table and runs the blob's static constructors. Called
	// once, before anything else.
	void (*start)(const GeaPebbleHost *host);
	void (*ui_init)(int width, int height);
	void (*ui_frame)(uint32_t now_ms);
	void (*ui_draw)(void *ctx);
	void (*ui_button)(int button);
	void (*ui_touch)(int phase, int x, int y);
	void (*ui_deinit)(void);
} GeaPebbleProgram;


#ifdef __cplusplus
}
#endif
