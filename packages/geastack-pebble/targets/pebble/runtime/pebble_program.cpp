// SPDX-License-Identifier: Apache-2.0
// The blob's entry: the header the shell finds at offset 0 of the image, and
// the start-up the PebbleOS loader would have done for a native app image.
#include "pebble_host.h"

#include <cstddef>

const GeaPebbleHost *gea_pebble_host = nullptr;

extern "C" {
// Bounds of the constructor tables, from pebble_program.ld.
extern void (*__preinit_array_start[])(void);
extern void (*__preinit_array_end[])(void);
extern void (*__init_array_start[])(void);
extern void (*__init_array_end[])(void);
}

namespace {

void start(const GeaPebbleHost *host)
{
	gea_pebble_host = host;
	// Nothing runs a Pebble app's static constructors -- the SDK's apps are
	// C -- so the blob runs its own, once the heap they allocate from is bound.
	for (auto entry = __preinit_array_start; entry != __preinit_array_end; ++entry) (*entry)();
	for (auto entry = __init_array_start; entry != __init_array_end; ++entry) (*entry)();
}

}  // namespace

extern "C" [[gnu::section(".gea_program"), gnu::used]] const GeaPebbleProgram gea_pebble_program = {
	GEA_PEBBLE_PROGRAM_MAGIC,
	GEA_PEBBLE_ABI,
	start,
	gea_pebble_ui_init,
	gea_pebble_ui_frame,
	gea_pebble_ui_draw,
	gea_pebble_ui_button,
	gea_pebble_ui_touch,
	gea_pebble_ui_deinit,
};
