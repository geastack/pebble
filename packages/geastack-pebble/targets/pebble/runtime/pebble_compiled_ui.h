// SPDX-License-Identifier: Apache-2.0
// What a compiled Pebble UI (plugin/index.mjs) is made of.
//
// The build resolves every box of the page, so the page arrives as constant
// tables the plugin writes into the program: the draw operations in the
// engine's draw order, and the rectangle of each click target. The program's
// draw method walks the table in runs, and hands over the text it computes at
// run time for the operations that need it; its mount method gives each
// click target its handler. All coordinates are page coordinates; the runtime
// applies the scroll.
#pragma once

#include <cstdint>
#include <string>
#include <type_traits>
#include <utility>

namespace gea::host::detail {
inline std::string toString(double value);
}  // namespace gea::host::detail

namespace gea::pebble::compiled {

enum OpKind : std::uint8_t {
	kFill,    // a: radius, b: color
	kStroke,  // a: radius, b: width, c: color
	kFocus,   // the ring around a focused control -- a: control, b: radius
	kText     // a: font, b: align, c: color, d: top inset; text, or run-time text
};

struct Op {
	std::int16_t x, y, w, h;
	std::uint8_t kind, a, b, c, d;
	const char *text;
};

struct Rect {
	std::int16_t x, y, w, h;
};

// A program closure, kept as the program built it: the runtime only calls it.
struct Callback {
	void *self;
	void (*call)(void *self);
};

template <typename F>
Callback callback(F &&function)
{
	using Function = std::decay_t<F>;
	return {new Function(std::forward<F>(function)), [](void *self) { (*static_cast<Function *>(self))(); }};
}

void drawOps(const Op *ops, int count);
void drawOp(const Op &op, const char *text);
void registerScreen(const Rect *controls, Callback *handlers, int controlCount, int contentBottom, Callback draw);
// Decimal digits of `value` at the end of `buffer` (24 bytes), without the
// 64-bit division libgcc would link for it.
const char *formatInteger(long long value, char *buffer);

inline const char *text(const std::string &value) { return value.c_str(); }
inline const char *text(const char *value) { return value; }

template <typename Text>
void textAt(const Op &op, const Text &value)
{
	drawOp(op, text(value));
}

// A number's text as JavaScript writes it. An integer field (the compiler
// narrows one to an integer type) never goes through double formatting.
template <typename Number>
void numberAt(const Op &op, const Number &value)
{
	if constexpr (std::is_integral_v<decltype(+value)>) {
		char buffer[24];
		drawOp(op, formatInteger(static_cast<long long>(+value), buffer));
	} else {
		drawOp(op, gea::host::detail::toString(static_cast<double>(+value)).c_str());
	}
}

}  // namespace gea::pebble::compiled
