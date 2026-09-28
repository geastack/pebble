// SPDX-License-Identifier: Apache-2.0
// The runtime of a compiled Pebble UI (plugin/index.mjs), in place of the
// engine (pebble_ui.cpp): the build resolved every box and style into
// constant tables, so what is left on the watch is walking them -- drawing,
// focus, scrolling and delivering clicks, each the engine's own behaviour,
// minus the tree it kept to find them.
#include "pebble_compiled_ui.h"

#include "gea/embedded.h"
#include "pebble_host.h"

using namespace gea::pebble::compiled;

namespace {

struct State {
	const Rect *controls = nullptr;
	Callback *handlers = nullptr;
	int controlCount = 0;
	Callback draw{nullptr, nullptr};
	void *ctx = nullptr;
	int width = 200;
	int height = 228;
	int contentBottom = 0;
	int scrollY = 0;
	int focus = -1;
	int pressed = -1;
	bool touching = false;
};

State s;

// pebble_ui.cpp `scrollIntoView`.
void scrollIntoView(const Rect &control)
{
	constexpr int kMargin = 6;
	if (control.y - kMargin < s.scrollY) s.scrollY = control.y - kMargin;
	else if (control.y + control.h + kMargin > s.scrollY + s.height) s.scrollY = control.y + control.h + kMargin - s.height;
	const int maxScroll = s.contentBottom - s.height;
	if (s.scrollY > maxScroll) s.scrollY = maxScroll;
	if (s.scrollY < 0) s.scrollY = 0;
}

// pebble_ui.cpp `moveFocus`, over the controls in document order.
void moveFocus(int step)
{
	if (s.controlCount == 0) {
		s.focus = -1;
		return;
	}
	s.focus = s.focus < 0 ? (step > 0 ? 0 : s.controlCount - 1) : (s.focus + step + s.controlCount) % s.controlCount;
	scrollIntoView(s.controls[s.focus]);
	gea_pebble_request_redraw();
}

void activate(int control)
{
	const Callback &callback = s.handlers[control];
	if (callback.call) callback.call(callback.self);
}

int hitTest(int x, int y)
{
	for (int i = s.controlCount - 1; i >= 0; --i) {
		const Rect &c = s.controls[i];
		if (x >= c.x && x < c.x + c.w && y >= c.y && y < c.y + c.h) return i;
	}
	return -1;
}

}  // namespace

namespace gea::pebble::compiled {

void registerScreen(const Rect *controls, Callback *handlers, int controlCount, int contentBottom, Callback draw)
{
	s.controls = controls;
	s.handlers = handlers;
	s.controlCount = controlCount;
	s.contentBottom = contentBottom;
	s.draw = draw;
	gea_pebble_request_redraw();
}

// pebble_ui.cpp `draw`, one operation of it: a box's background, its border,
// the focus ring, or a text node -- centred vertically in its box and set
// higher by the font's built-in top padding.
void drawOp(const Op &op, const char *text)
{
	const int y = op.y - s.scrollY;
	if (y > s.height || y + op.h < 0) return;
	switch (op.kind) {
	case kFill: gea_pebble_fill_rect(s.ctx, op.x, y, op.w, op.h, op.a, op.b); break;
	case kStroke: gea_pebble_stroke_rect(s.ctx, op.x, y, op.w, op.h, op.a, op.b, op.c); break;
	case kFocus:
		if (s.focus == op.a && !s.touching) gea_pebble_stroke_rect(s.ctx, op.x, y, op.w, op.h, op.b, 2, 0xFF);
		break;
	case kText: {
		if (!text || !*text) return;
		int textW = 0;
		int textH = 0;
		gea_pebble_measure_text(text, op.a, op.w, &textW, &textH);
		const int offset = op.h > textH ? (op.h - textH) / 2 : 0;
		gea_pebble_draw_text(s.ctx, text, op.a, op.x, y + offset - op.d, op.w, textH + op.d + 4, op.b, op.c);
		break;
	}
	}
}

void drawOps(const Op *ops, int count)
{
	for (int i = 0; i < count; ++i) drawOp(ops[i], ops[i].text);
}

const char *formatInteger(long long value, char *buffer)
{
	char *digit = buffer + 23;
	*digit = '\0';
	unsigned long long magnitude = value < 0 ? 0ull - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
	if (magnitude <= 0xffffffffull) {
		// The Cortex-M3 divides 32 bits in hardware.
		std::uint32_t rest = static_cast<std::uint32_t>(magnitude);
		do *--digit = static_cast<char>('0' + rest % 10);
		while ((rest /= 10) != 0);
	} else {
		// Long division by ten over 16-bit limbs, most significant first.
		std::uint32_t limbs[4] = {static_cast<std::uint32_t>(magnitude >> 48), static_cast<std::uint32_t>((magnitude >> 32) & 0xffff),
		                          static_cast<std::uint32_t>((magnitude >> 16) & 0xffff), static_cast<std::uint32_t>(magnitude & 0xffff)};
		for (bool more = true; more;) {
			std::uint32_t remainder = 0;
			more = false;
			for (std::uint32_t &limb : limbs) {
				const std::uint32_t current = (remainder << 16) | limb;
				limb = current / 10;
				remainder = current % 10;
				more = more || limb != 0;
			}
			*--digit = static_cast<char>('0' + remainder);
		}
	}
	if (value < 0) *--digit = '-';
	return digit;
}

}  // namespace gea::pebble::compiled

// ---- Shell entry points ----------------------------------------------------

extern void __gea_top_level();

namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

extern "C" {

void gea_pebble_ui_init(int width, int height)
{
	s.width = width;
	s.height = height;
	__gea_top_level();
	gea::framework::app::generated::drainMicrotasks();
}

void gea_pebble_ui_frame(uint32_t now_ms)
{
	gea_pebble_run_animation_frames(now_ms);
	gea::framework::app::generated::drainMicrotasks();
}

void gea_pebble_ui_draw(void *ctx)
{
	s.ctx = ctx;
	gea_pebble_fill_rect(ctx, 0, 0, s.width, s.height, 0, 0xC0);
	if (s.draw.call) s.draw.call(s.draw.self);
}

void gea_pebble_ui_button(int button)
{
	s.touching = false;
	if (button == GEA_PEBBLE_BUTTON_UP) moveFocus(-1);
	else if (button == GEA_PEBBLE_BUTTON_DOWN) moveFocus(1);
	else if (button == GEA_PEBBLE_BUTTON_SELECT) {
		if (s.focus < 0) moveFocus(1);
		if (s.focus >= 0) activate(s.focus);
	}
	gea::framework::app::generated::drainMicrotasks();
}

// A tap is a press and release on the same control, as the engine delivers it.
void gea_pebble_ui_touch(int phase, int x, int y)
{
	s.touching = true;
	const int target = hitTest(x, y + s.scrollY);
	if (phase == GEA_PEBBLE_TOUCH_DOWN) {
		s.pressed = target;
	} else if (phase == GEA_PEBBLE_TOUCH_UP) {
		if (target >= 0 && target == s.pressed) activate(target);
		s.pressed = -1;
	}
	gea::framework::app::generated::drainMicrotasks();
}

void gea_pebble_ui_deinit(void) {}

}  // extern "C"

// ---- StyleSheet ------------------------------------------------------------
//
// The gea build still hands the program its style prelude; the build already
// applied it, so on the watch registering a rule does nothing, and LTO drops
// the tape the prelude would have walked.
namespace gea::embedded::ui {

StyleSheet &StyleSheet::instance()
{
	static StyleSheet sheet;
	return sheet;
}

void StyleSheet::beginRuleRegistrationBatch() {}
void StyleSheet::endRuleRegistrationBatch() {}
void StyleSheet::registerStaticPropertyRule(StaticStyleSelectorKind, const char *, Property, int, const char *) {}
void StyleSheet::registerStaticPropertyGroupRule(StaticStyleSelectorKind, const char *, std::initializer_list<StaticStylePropertyValue>, const char *) {}
void StyleSheet::registerStaticColorRule(StaticStyleSelectorKind, const char *, StaticStyleColorProperty, int, int, int, int, const char *) {}
void StyleSheet::registerStaticColorVarRule(StaticStyleSelectorKind, const char *, StaticStyleColorProperty, const char *, bool, int, int, int, int,
                                            const char *)
{
}
void StyleSheet::registerStaticLengthRule(StaticStyleSelectorKind, const char *, StaticStyleLengthProperty, StaticStyleLengthUnit, float, const char *) {}
void StyleSheet::registerStaticLengthSpecRule(StaticStyleSelectorKind, const char *, StaticStyleLengthProperty, StaticStyleLengthSpec, const char *) {}
void StyleSheet::registerStaticBorderRule(StaticStyleSelectorKind, const char *, StaticStyleLengthSpec, int, int, int, int, const char *) {}
void StyleSheet::registerStaticBorderRadiusRule(StaticStyleSelectorKind, const char *, StaticStyleLengthSpec, StaticStyleLengthSpec, StaticStyleLengthSpec,
                                                StaticStyleLengthSpec, const char *)
{
}
void StyleSheet::registerStaticBorderRadiusCornerRule(StaticStyleSelectorKind, const char *, StaticStyleBorderRadiusCorner, StaticStyleLengthSpec,
                                                      const char *)
{
}
void StyleSheet::registerStaticFlexRule(StaticStyleSelectorKind, const char *, int, StaticStyleLengthSpec, bool, const char *) {}
void StyleSheet::registerStaticLineHeightRule(StaticStyleSelectorKind, const char *, StaticStyleLineHeightKind, StaticStyleLengthSpec, const char *) {}
void StyleSheet::registerStaticFontFamilyRule(StaticStyleSelectorKind, const char *, const char *, const char *) {}
void StyleSheet::registerStaticBoxShadowNoneRule(StaticStyleSelectorKind, const char *, const char *) {}
void StyleSheet::registerStaticFilterBlurRule(StaticStyleSelectorKind, const char *, StaticStyleLengthSpec, const char *) {}
void StyleSheet::registerStaticCustomLengthRule(StaticStyleSelectorKind, const char *, const char *, StaticStyleLengthSpec, const char *) {}
void StyleSheet::registerStaticCustomColorRule(StaticStyleSelectorKind, const char *, const char *, int, int, int, int, const char *) {}
void StyleSheet::registerStaticRule(const char *, const char *, const char *, const char *) {}
void StyleSheet::registerStaticElementRule(const char *, const char *, const char *, const char *) {}
void StyleSheet::registerStaticSelectorRule(const char *, const char *, const char *, const char *) {}

}  // namespace gea::embedded::ui
