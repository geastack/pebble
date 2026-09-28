// SPDX-License-Identifier: Apache-2.0
// A reactive field's cell on a compiled Pebble UI.
//
// With the engine, a write notifies the effects the mounted template
// subscribed, and each effect patches its node. A compiled UI has no nodes:
// its draw method reads the fields directly, so a write that changes a value
// only has to ask the watch for a redraw. The cell is the value and nothing
// else -- no subscriber list per field.
//
// Found ahead of the engine's own `ui/signal.h` because this directory comes
// first on the include path; an engine build takes the engine's.
#pragma once

#ifndef GEA_PEBBLE_COMPILED_UI
#include_next "ui/signal.h"
#else

#include <cstddef>
#include <functional>
#include <string>
#include <utility>

#include "pebble_host.h"

namespace gea::embedded::ui {

// A CompiledStore's registry: every notification is a redraw.
class SignalHub {
public:
	std::size_t subscribe(const char *, std::function<void()>) { return 0; }
	void unsubscribe(std::size_t) {}
	void notify(const char *) const { gea_pebble_request_redraw(); }
	void notify(const std::string &) const { gea_pebble_request_redraw(); }
};

template <typename T>
class Signal {
public:
	Signal() = default;
	Signal(T value) : value_(std::move(value)) {}  // NOLINT(google-explicit-constructor)

	operator const T &() const { return value_; }  // NOLINT(google-explicit-constructor)
	const T &get() const { return value_; }

	Signal &operator=(const T &next)
	{
		if (value_ == next) return *this;
		value_ = next;
		gea_pebble_request_redraw();
		return *this;
	}
	Signal &operator=(T &&next)
	{
		if (value_ == next) return *this;
		value_ = std::move(next);
		gea_pebble_request_redraw();
		return *this;
	}

	friend bool operator==(const Signal &lhs, const T &rhs) { return lhs.value_ == rhs; }
	friend bool operator==(const T &lhs, const Signal &rhs) { return lhs == rhs.value_; }
	friend bool operator!=(const Signal &lhs, const T &rhs) { return !(lhs.value_ == rhs); }
	friend bool operator!=(const T &lhs, const Signal &rhs) { return !(lhs == rhs.value_); }

	// Nothing subscribes on a compiled UI; kept so code that names them compiles.
	std::size_t subscribe(std::function<void()>) const { return 0; }
	void unsubscribe(std::size_t) const {}
	void notify() const { gea_pebble_request_redraw(); }

private:
	T value_{};
};

}  // namespace gea::embedded::ui

#endif
