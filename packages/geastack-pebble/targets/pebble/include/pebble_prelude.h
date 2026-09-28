// SPDX-License-Identifier: Apache-2.0
// Force-included (-include) into every translation unit of a Pebble build.
//
// The Pebble SDK's arm-none-eabi libstdc++ is built without thread support, so
// <mutex> and <condition_variable> declare no std::mutex, std::recursive_mutex
// or std::condition_variable. A Pebble app runs on exactly one task: nothing can
// contend for a lock and nothing can notify a waiter, so the framework's locks
// are satisfied by types that do nothing.
#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <variant>
#include <vector>

#if defined(__GLIBCXX__) && !defined(_GLIBCXX_HAS_GTHREADS)
namespace std {

class mutex {
public:
	constexpr mutex() noexcept = default;
	mutex(const mutex &) = delete;
	mutex &operator=(const mutex &) = delete;
	void lock() {}
	bool try_lock() { return true; }
	void unlock() {}
};

class recursive_mutex {
public:
	recursive_mutex() = default;
	recursive_mutex(const recursive_mutex &) = delete;
	recursive_mutex &operator=(const recursive_mutex &) = delete;
	void lock() {}
	bool try_lock() { return true; }
	void unlock() {}
};

enum class cv_status { no_timeout, timeout };

class condition_variable {
public:
	condition_variable() = default;
	condition_variable(const condition_variable &) = delete;
	condition_variable &operator=(const condition_variable &) = delete;
	void notify_one() noexcept {}
	void notify_all() noexcept {}
	// An unbounded wait with nothing else running can never be woken.
	template <typename Lock>
	void wait(Lock &) { std::abort(); }
	template <typename Lock, typename Predicate>
	void wait(Lock &, Predicate predicate) {
		if (!predicate()) std::abort();
	}
	template <typename Lock, typename Rep, typename Period>
	cv_status wait_for(Lock &, const chrono::duration<Rep, Period> &) { return cv_status::timeout; }
	template <typename Lock, typename Rep, typename Period, typename Predicate>
	bool wait_for(Lock &, const chrono::duration<Rep, Period> &, Predicate predicate) { return predicate(); }
	template <typename Lock, typename Clock, typename Duration, typename Predicate>
	bool wait_until(Lock &, const chrono::time_point<Clock, Duration> &, Predicate predicate) { return predicate(); }
};

}  // namespace std
#endif

// `std::to_chars(double)` carries 128 KB of Ryu tables, and the runtime's
// `%g` + `strtod` fallback another 16 KB of newlib. The compact formatter is
// the same text from exact integer arithmetic and no tables.
#define GEA_CPP_USE_TO_CHARS_DOUBLE 0
#define GEA_CPP_COMPACT_NUMBER_FORMAT 1

// Code size over speed wherever the runtime offers the choice: a reference
// count released out of line, once per handle type, rather than inlined at
// every handle that dies.
#define GEA_RUNTIME_COMPACT_CODE 1

// The app links no unwinder, so nothing can catch: a throw logs and ends the
// app at the throw (pebble_support.cpp's `gea_runtime_uncaught`) instead of
// building the thrown value first.
#define GEA_RUNTIME_THROW_ENDS_PROGRAM 1

// No Ref pools: every cell is its own heap block, freed at once. A pool's
// chunks are never returned and a freed cell only serves its own size class,
// so on a heap of a few tens of kilobytes the pools stranded what a scene of
// 64 bouncing balls needed (17 KB of 256-byte chunks, and a one-off large
// record kept its cells for good).
#define GEA_RUNTIME_COMPACT_ALLOCATION 1

// Tells the UI backend which node a delegated JSX event handler belongs to,
// so the buttons can move focus between controls (pebble_ui.cpp).
extern "C" void gea_pebble_note_listener(int node, const char *type);
#define GEA_JSX_NODE_LISTENER_HOOK(node, type) gea_pebble_note_listener((node), (type))

// One task, so per-thread storage is just storage. arm-none-eabi has no TLS:
// every `thread_local` access went through `__emutls_get_address` (a call and
// a heap-allocated control block per variable), and the runtime's allocation
// pools and cycle state are read on every allocation and release. Defined
// after the standard headers above so none of them sees it.
#define thread_local
