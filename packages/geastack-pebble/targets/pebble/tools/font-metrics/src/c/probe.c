// SPDX-License-Identifier: Apache-2.0
// Logs what the firmware's text layout reports for every system font the gea
// Pebble target uses, so the build can lay out text at compile time with the
// firmware's own numbers. Run it on the emulator and feed the log to
// tools/font-metrics.mjs, which writes runtime/font-metrics.json.
//
// The font table is the shell's (shell/pebble_app.c), in the same order: the
// index a metric is logged under is the index the program passes to the host.
#include <pebble.h>

static const char *const s_keys[] = {
	FONT_KEY_GOTHIC_09,        FONT_KEY_GOTHIC_14,      FONT_KEY_GOTHIC_14_BOLD, FONT_KEY_GOTHIC_18,
	FONT_KEY_GOTHIC_18_BOLD,   FONT_KEY_ROBOTO_CONDENSED_21, FONT_KEY_GOTHIC_24, FONT_KEY_GOTHIC_24_BOLD,
	FONT_KEY_GOTHIC_28,        FONT_KEY_GOTHIC_28_BOLD, FONT_KEY_BITHAM_30_BLACK, FONT_KEY_BITHAM_42_LIGHT,
	FONT_KEY_BITHAM_42_BOLD,
};
#define FONT_COUNT ((int)(sizeof(s_keys) / sizeof(s_keys[0])))

// Strings whose measured width is compared against the sum of their glyphs,
// and wrapped at a narrow width to see how lines stack.
static const char *const s_samples[] = {"Counter", "Reset", "-1", "+", "Below zero", "Back at zero", "Counting up", "WAVE Ag|j"};
#define SAMPLE_COUNT ((int)(sizeof(s_samples) / sizeof(s_samples[0])))

static GSize measure(const char *text, GFont font, int bound)
{
	return graphics_text_layout_get_content_size(text, font, GRect(0, 0, bound, 2000), GTextOverflowModeWordWrap,
	                                             GTextAlignmentLeft);
}

static void probe(void)
{
	for (int f = 0; f < FONT_COUNT; ++f) {
		GFont font = fonts_get_system_font(s_keys[f]);
		const GSize empty = measure("", font, 2000);
		const GSize line = measure("Ag", font, 2000);
		const GSize two = measure("Ag\nAg", font, 2000);
		APP_LOG(APP_LOG_LEVEL_INFO, "FM font %d empty %d %d line %d %d two %d %d", f, empty.w, empty.h, line.w, line.h, two.w,
		        two.h);
		char buffer[160];
		for (int start = 32; start < 127; start += 24) {
			int length = snprintf(buffer, sizeof(buffer), "FM glyphs %d %d", f, start);
			for (int c = start; c < start + 24 && c < 127; ++c) {
				const char text[2] = {(char)c, 0};
				length += snprintf(buffer + length, sizeof(buffer) - length, " %d", measure(text, font, 2000).w);
			}
			APP_LOG(APP_LOG_LEVEL_INFO, "%s", buffer);
		}
		for (int s = 0; s < SAMPLE_COUNT; ++s) {
			const GSize wide = measure(s_samples[s], font, 2000);
			const GSize narrow = measure(s_samples[s], font, 40);
			APP_LOG(APP_LOG_LEVEL_INFO, "FM sample %d %d %d %d %d %d", f, s, wide.w, wide.h, narrow.w, narrow.h);
		}
		psleep(20);
	}
	APP_LOG(APP_LOG_LEVEL_INFO, "FM done");
}

int main(void)
{
	Window *window = window_create();
	window_stack_push(window, false);
	probe();
	app_event_loop();
	window_destroy(window);
}
