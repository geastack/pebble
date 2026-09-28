// SPDX-License-Identifier: Apache-2.0
// C and C++ runtime support for the Gea program blob.
//
// The blob links newlib-nano and libstdc++ with no operating system beneath
// them: its code and its heap share the 128 KB PebbleOS gives an app. What
// those libraries reach for beyond themselves -- memory, output, abort -- is
// answered here through the host table, each in the smallest form that is
// still correct on a single-task watch app.
#include "pebble_host.h"

#include <chrono>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <initializer_list>
#include <new>
#include <sys/stat.h>
#include <sys/time.h>

namespace {

void logText(const char *text, std::size_t length)
{
	char line[128];
	while (length > 0) {
		const std::size_t take = length < sizeof(line) - 1 ? length : sizeof(line) - 1;
		std::memcpy(line, text, take);
		std::size_t chunk = take;
		// The app log adds its own line breaks.
		while (chunk > 0 && (line[chunk - 1] == '\n' || line[chunk - 1] == '\r')) --chunk;
		line[chunk] = '\0';
		if (chunk) gea_pebble_log(line);
		text += take;
		length -= take;
	}
}

struct Output {
	char *at;
	char *end;
	int written;
	void put(char character)
	{
		if (at < end) *at++ = character;
		++written;
	}
};

void putPadded(Output &out, const char *text, int length, int width, bool left, char pad)
{
	if (!left)
		for (int fill = length; fill < width; ++fill) out.put(pad);
	for (int at = 0; at < length; ++at) out.put(text[at]);
	if (left)
		for (int fill = length; fill < width; ++fill) out.put(' ');
}

int digitsOf(unsigned long long value, unsigned base, bool upper, char *buffer)
{
	const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	int length = 0;
	do {
		buffer[length++] = digits[value % base];
		value /= base;
	} while (value);
	for (int low = 0, high = length - 1; low < high; ++low, --high) {
		const char swap = buffer[low];
		buffer[low] = buffer[high];
		buffer[high] = swap;
	}
	return length;
}

}  // namespace

// The printf family, formatted here rather than by newlib. Every number a
// program shows goes through the runtime's own formatter
// (`GEA_CPP_COMPACT_NUMBER_FORMAT`); what reaches printf is the runtime's
// diagnostics and this file's messages, which need integers, strings and
// pointers. A floating conversion prints the value's integer part and three
// decimals -- enough for a diagnostic line, and 3 KB less than newlib's engine.
extern "C" __attribute__((used)) int vsnprintf(char *buffer, size_t size, const char *format, va_list args)
{
	Output out{buffer, buffer + (size ? size - 1 : 0), 0};
	for (const char *at = format; *at; ++at) {
		if (*at != '%') {
			out.put(*at);
			continue;
		}
		++at;
		bool left = false;
		char pad = ' ';
		for (;; ++at) {
			if (*at == '-') left = true;
			else if (*at == '0') pad = '0';
			else if (*at != '+' && *at != ' ' && *at != '#') break;
		}
		int width = 0;
		if (*at == '*') {
			width = va_arg(args, int);
			++at;
		} else
			while (*at >= '0' && *at <= '9') width = width * 10 + (*at++ - '0');
		int precision = -1;
		if (*at == '.') {
			++at;
			precision = 0;
			if (*at == '*') {
				precision = va_arg(args, int);
				++at;
			} else
				while (*at >= '0' && *at <= '9') precision = precision * 10 + (*at++ - '0');
		}
		int longs = 0;
		bool size_t_length = false;
		for (;; ++at) {
			if (*at == 'l') ++longs;
			else if (*at == 'z' || *at == 't') size_t_length = true;
			else if (*at == 'j') longs = 2;
			else if (*at != 'h') break;
		}
		char digits[48];
		switch (*at) {
		case 'd':
		case 'i': {
			const long long value = longs >= 2 ? va_arg(args, long long)
			                        : longs == 1 ? va_arg(args, long)
			                        : size_t_length ? static_cast<long long>(va_arg(args, ptrdiff_t))
			                                        : va_arg(args, int);
			const unsigned long long magnitude = value < 0 ? 0ull - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
			int length = 0;
			if (value < 0) digits[length++] = '-';
			length += digitsOf(magnitude, 10, false, digits + length);
			putPadded(out, digits, length, width, left, pad);
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o': {
			const unsigned long long value = longs >= 2 ? va_arg(args, unsigned long long)
			                                 : longs == 1 ? va_arg(args, unsigned long)
			                                 : size_t_length ? va_arg(args, size_t)
			                                                 : va_arg(args, unsigned);
			const unsigned base = *at == 'u' ? 10 : *at == 'o' ? 8 : 16;
			putPadded(out, digits, digitsOf(value, base, *at == 'X', digits), width, left, pad);
			break;
		}
		case 'p': {
			digits[0] = '0';
			digits[1] = 'x';
			const int length = 2 + digitsOf(reinterpret_cast<uintptr_t>(va_arg(args, void *)), 16, false, digits + 2);
			putPadded(out, digits, length, width, left, ' ');
			break;
		}
		case 'c':
			digits[0] = static_cast<char>(va_arg(args, int));
			putPadded(out, digits, 1, width, left, ' ');
			break;
		case 's': {
			const char *text = va_arg(args, const char *);
			if (!text) text = "(null)";
			int length = 0;
			while (text[length] && (precision < 0 || length < precision)) ++length;
			putPadded(out, text, length, width, left, ' ');
			break;
		}
		case 'f':
		case 'F':
		case 'e':
		case 'E':
		case 'g':
		case 'G': {
			double value = va_arg(args, double);
			int length = 0;
			if (value != value) {
				putPadded(out, "nan", 3, width, left, ' ');
				break;
			}
			if (value < 0) {
				digits[length++] = '-';
				value = -value;
			}
			if (value > 1e18) {
				putPadded(out, length ? "-huge" : "huge", length ? 5 : 4, width, left, ' ');
				break;
			}
			const unsigned long long whole = static_cast<unsigned long long>(value);
			length += digitsOf(whole, 10, false, digits + length);
			const unsigned long long thousandths = static_cast<unsigned long long>((value - static_cast<double>(whole)) * 1000.0 + 0.5);
			if (thousandths) {
				digits[length++] = '.';
				char fraction[8];
				const int count = digitsOf(thousandths > 999 ? 999 : thousandths, 10, false, fraction);
				for (int zero = count; zero < 3; ++zero) digits[length++] = '0';
				for (int index = 0; index < count; ++index) digits[length++] = fraction[index];
			}
			putPadded(out, digits, length, width, left, ' ');
			break;
		}
		case '%':
			out.put('%');
			break;
		case '\0':
			--at;
			break;
		default:
			out.put('%');
			out.put(*at);
		}
	}
	if (size) *out.at = '\0';
	return out.written;
}

extern "C" __attribute__((used)) int snprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int written = vsnprintf(buffer, size, format, args);
	va_end(args);
	return written;
}

extern "C" __attribute__((used)) int sniprintf(char *buffer, size_t size, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int written = vsnprintf(buffer, size, format, args);
	va_end(args);
	return written;
}

extern "C" __attribute__((used)) int vsniprintf(char *buffer, size_t size, const char *format, va_list args)
{
	return vsnprintf(buffer, size, format, args);
}

// Everything the program writes to stdout or stderr (console.log, runtime
// diagnostics) goes to the app log, which `pebble logs` shows. Defining these
// keeps the C library's stdio and its printf engines out of the app.
extern "C" int fputs(const char *text, FILE *)
{
	logText(text, std::strlen(text));
	return 0;
}

extern "C" int puts(const char *text)
{
	logText(text, std::strlen(text));
	return 0;
}

extern "C" size_t fwrite(const void *data, size_t size, size_t count, FILE *)
{
	logText(static_cast<const char *>(data), size * count);
	return count;
}

extern "C" int fputc(int character, FILE *)
{
	const char text = static_cast<char>(character);
	logText(&text, 1);
	return character;
}

static int logFormatted(const char *format, va_list args)
{
	char line[128];
	const int written = vsnprintf(line, sizeof(line), format, args);
	logText(line, std::strlen(line));
	return written;
}

extern "C" int vfprintf(FILE *, const char *format, va_list args) { return logFormatted(format, args); }

extern "C" int fprintf(FILE *stream, const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int written = vfprintf(stream, format, args);
	va_end(args);
	return written;
}

extern "C" int printf(const char *format, ...)
{
	va_list args;
	va_start(args, format);
	const int written = logFormatted(format, args);
	va_end(args);
	return written;
}

// A failed C `assert` (newlib's gdtoa has some) ends the app. The default
// reports through `fiprintf`, a second printf engine the app has no other
// use for.
extern "C" void __assert_func(const char *file, int line, const char *, const char *expression)
{
	char message[128];
	std::snprintf(message, sizeof(message), "assertion failed: %s (%s:%d)", expression, file, line);
	gea_pebble_log(message);
	std::abort();
}

// `std::system_error` names an errno through this; a watch app has no errno
// worth a 2.7 KB table of messages, so the number stands in for the text.
extern "C" int __xpg_strerror_r(int error, char *buffer, size_t size)
{
	if (size) std::snprintf(buffer, size, "error %d", error);
	return 0;
}

// An uncaught exception ends the app. The default handler demangles the
// exception's type name for its message, which costs 33 KB of demangler.
namespace __gnu_cxx {
void __verbose_terminate_handler()
{
	gea_pebble_log("terminate: uncaught exception");
	std::abort();
}
}  // namespace __gnu_cxx

// libstdc++ swaps the terminate handler with an atomic exchange, an
// LDREX/STREX loop. On the Pebble Time 2's SF32LB52 a store-exclusive to app
// RAM never succeeds, so that loop spins forever -- the app hung in its first
// static constructor, before it drew anything (the emulator's QEMU does not
// model the difference). A Pebble app is one task, so the handler is a plain
// variable.
namespace {
std::terminate_handler terminateHandler = __gnu_cxx::__verbose_terminate_handler;
}  // namespace
__attribute__((used)) std::terminate_handler std::set_terminate(std::terminate_handler handler) noexcept
{
	const std::terminate_handler previous = terminateHandler;
	terminateHandler = handler ? handler : __gnu_cxx::__verbose_terminate_handler;
	return previous;
}
__attribute__((used)) std::terminate_handler std::get_terminate() noexcept { return terminateHandler; }
__attribute__((used)) void std::terminate() noexcept
{
	terminateHandler();
	std::abort();
}

// No exception is ever caught on the watch: a `throw` ends the app where it
// happens. These replace the C++ runtime's throw and unwinding entry points,
// so its unwinder (~9 KB) never links, and the linker script discards the
// unwind tables (`.ARM.extab`/`.ARM.exidx`, ~4 KB) that only it read. The
// program is still compiled with exceptions because the Gea runtime and the
// emitted code spell `try`/`throw`; a `catch` there simply never runs. What
// is lost is a JS program catching its own error -- which the counter-class
// apps this target is for do not do -- and an uncaught one ended the app
// anyway.
// `used`: calls to these appear only once LTO has generated code, too late
// for it to keep a definition nothing referenced before.
extern "C" {
[[noreturn]] static void thrown(const char *what)
{
	gea_pebble_log(what);
	std::abort();
}
__attribute__((used)) void *__cxa_allocate_exception(size_t) noexcept { thrown("uncaught exception (a throw ends a Pebble app)"); }
__attribute__((used)) void __cxa_free_exception(void *) noexcept {}
[[noreturn]] __attribute__((used)) void __cxa_throw(void *, void *, void (*)(void *)) { thrown("uncaught exception (a throw ends a Pebble app)"); }
[[noreturn]] __attribute__((used)) void __cxa_rethrow() { thrown("uncaught exception (rethrown)"); }
__attribute__((used)) void *__cxa_begin_catch(void *exception) noexcept { return exception; }
__attribute__((used)) void __cxa_end_catch() {}
__attribute__((used)) void *__cxa_get_exception_ptr(void *exception) noexcept { return exception; }
[[noreturn]] __attribute__((used)) void __cxa_end_cleanup() { thrown("unwinding is not supported"); }
[[noreturn]] __attribute__((used)) void _Unwind_Resume(void *) { thrown("unwinding is not supported"); }
[[noreturn]] __attribute__((used)) void __cxa_call_unexpected(void *) { thrown("unexpected exception"); }
__attribute__((used)) int __gxx_personality_v0(...) { thrown("unwinding is not supported"); }
__attribute__((used)) int __aeabi_unwind_cpp_pr0(...) { thrown("unwinding is not supported"); }
__attribute__((used)) int __aeabi_unwind_cpp_pr1(...) { thrown("unwinding is not supported"); }
__attribute__((used)) int __aeabi_unwind_cpp_pr2(...) { thrown("unwinding is not supported"); }
}

// Where every JS throw lands (`GEA_THROW`, `gea::host::throwRuntimeError`):
// the runtime's own errors arrive with their kind and message.
[[noreturn]] void gea_runtime_uncaught(const char *kind, const char *message)
{
	char line[128];
	std::size_t length = 0;
	for (const char *part : {"uncaught ", kind, ": ", message})
		for (; *part && length + 1 < sizeof(line); ++part) line[length++] = *part;
	line[length] = 0;
	gea_pebble_log(line);
	std::abort();
}

// With nothing ever caught, an `exception_ptr` is always empty: the runtime's
// promise machinery names the type, which alone would link `eh_ptr.o` and the
// unwinder behind it.
__attribute__((used)) void std::__exception_ptr::exception_ptr::_M_addref() noexcept {}
__attribute__((used)) void std::__exception_ptr::exception_ptr::_M_release() noexcept {}
__attribute__((used)) std::exception_ptr std::current_exception() noexcept { return {}; }
__attribute__((used)) __cxxabiv1::__cxa_refcounted_exception *__cxxabiv1::__cxa_init_primary_exception(void *object, std::type_info *,
                                                                                          void (*)(void *)) noexcept
{
	return static_cast<__cxa_refcounted_exception *>(object);
}
__attribute__((used, noreturn)) void std::rethrow_exception(std::exception_ptr) { thrown("uncaught exception (rethrown)"); }

// The program allocates from the app heap PebbleOS gives the shell, through
// the host table. Newlib's own allocator (and the `_sbrk` it would need) never
// links; its internal `_r` entry points land here too.
#ifdef GEA_PEBBLE_ALLOC_TRACE
// Heap accounting for size work (`GEA_PEBBLE_ALLOC_TRACE=1` on the build):
// every allocation logs its address, size and the first program return
// addresses found on the stack, as offsets `addr2line` resolves against
// program.elf; every free logs its address.
extern "C" const struct GeaPebbleProgram gea_pebble_program;
extern "C" char __gea_trace_text_end __asm__("_etext");
namespace {
void traceHex(char *&out, std::uintptr_t value)
{
	*out++ = ' ';
	for (int shift = 28; shift >= 0; shift -= 4) *out++ = "0123456789abcdef"[(value >> shift) & 15];
}
[[gnu::noinline]] void traceAllocation(char kind, void *pointer, std::size_t size)
{
	char line[96];
	char *out = line;
	*out++ = 'H';
	*out++ = kind;
	traceHex(out, reinterpret_cast<std::uintptr_t>(pointer));
	if (kind != 'F') {
		traceHex(out, size);
		const std::uintptr_t base = reinterpret_cast<std::uintptr_t>(&gea_pebble_program);
		const std::uintptr_t end = reinterpret_cast<std::uintptr_t>(&__gea_trace_text_end);
		const std::uintptr_t *frame = reinterpret_cast<const std::uintptr_t *>(__builtin_frame_address(0));
		int found = 0;
		for (int word = 0; word < 160 && found < 6; ++word) {
			const std::uintptr_t value = frame[word];
			if ((value & 1) && value > base && value < end) {
				traceHex(out, value - 1 - base);
				++found;
			}
		}
	}
	*out = '\0';
	gea_pebble_log(line);
}
}
#define GEA_TRACE_ALLOCATION(kind, pointer, size) traceAllocation(kind, pointer, size)
#else
#define GEA_TRACE_ALLOCATION(kind, pointer, size) ((void)0)
#endif

extern "C" {
void *malloc(size_t size)
{
	void *pointer = gea_pebble_host->malloc(size ? size : 1);
	GEA_TRACE_ALLOCATION('A', pointer, size);
	if (!pointer) {
		// Spelled by hand: `snprintf` is otherwise unreferenced, and its
		// engine (with the double arithmetic of `%f`) is 4 KB of the app.
		char message[48] = "out of memory allocating ";
		char digits[11];
		int count = 0;
		do digits[count++] = static_cast<char>('0' + size % 10);
		while ((size /= 10) != 0);
		std::size_t length = std::strlen(message);
		while (count > 0) message[length++] = digits[--count];
		std::strcpy(message + length, " bytes");
		gea_pebble_log(message);
	}
	return pointer;
}
void free(void *pointer)
{
	GEA_TRACE_ALLOCATION('F', pointer, 0);
	if (pointer) gea_pebble_host->free(pointer);
}
void *realloc(void *pointer, size_t size)
{
	GEA_TRACE_ALLOCATION('F', pointer, 0);
	void *resized = gea_pebble_host->realloc(pointer, size ? size : 1);
	GEA_TRACE_ALLOCATION('A', resized, size);
	if (!resized) gea_pebble_log("out of memory reallocating");
	return resized;
}
void *calloc(size_t count, size_t size)
{
	const size_t total = count * size;
	if (size && total / size != count) return nullptr;
	void *pointer = malloc(total);
	if (pointer) std::memset(pointer, 0, total);
	return pointer;
}
void *_malloc_r(struct _reent *, size_t size) { return malloc(size); }
void _free_r(struct _reent *, void *pointer) { free(pointer); }
void *_realloc_r(struct _reent *, void *pointer, size_t size) { return realloc(pointer, size); }
void *_calloc_r(struct _reent *, size_t count, size_t size) { return calloc(count, size); }

// newlib's `memalign`, which libstdc++'s aligned `operator new` reaches,
// carves its block out of a larger one with newlib's own heap bookkeeping and
// frees the pieces back through `_free_r` -- which here is PebbleOS's heap,
// and it reports the second piece as a double free. The heap hands out
// 4-byte aligned blocks, so a larger alignment over-allocates and keeps the
// block's own address just before the aligned one.
[[gnu::noinline]] void *gea_pebble_aligned_alloc(size_t size, size_t alignment)
{
	if (alignment <= 4) return malloc(size);
	void *block = malloc(size + alignment + sizeof(void *));
	if (!block) return nullptr;
	const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(block) + sizeof(void *);
	void **aligned = reinterpret_cast<void **>((start + alignment - 1) & ~(alignment - 1));
	aligned[-1] = block;
	return aligned;
}
[[gnu::noinline]] void gea_pebble_aligned_free(void *pointer, size_t alignment)
{
	if (!pointer) return;
	free(alignment <= 4 ? pointer : static_cast<void **>(pointer)[-1]);
}

// `abort` (an uncaught exception, a failed assertion) ends the app; the
// shell turns that into a crash PebbleOS reports and cleans up after.
void abort(void)
{
	gea_pebble_host->abort();
	__builtin_unreachable();
}
void _exit(int)
{
	gea_pebble_host->abort();
	__builtin_unreachable();
}
int _kill(int, int) { return -1; }
int _getpid(void) { return 1; }
}  // extern "C"

// Newlib's stdio is linked (the formatting engine lives there) but has no
// files: everything the program prints goes through the overrides above, so
// these only answer the calls stdio's bookkeeping can make.
extern "C" {
int _write(int, const char *text, int length)
{
	logText(text, static_cast<std::size_t>(length));
	return length;
}
int _read(int, char *, int) { return 0; }
int _close(int) { return -1; }
int _lseek(int, int, int) { return -1; }
int _fstat(int, struct stat *) { return -1; }
int _isatty(int) { return 1; }

// `std::chrono::system_clock` (`Date.now()`) reads the watch's clock; the
// definitions of the clocks' `now()` below answer straight from the host, and
// this remains only for anything that asks libc directly.
int _gettimeofday(struct timeval *time, void *)
{
	const double now = gea_pebble_host->epoch_ms();
	const long long millis = static_cast<long long>(now);
	time->tv_sec = static_cast<time_t>(millis / 1000);
	time->tv_usec = static_cast<suseconds_t>((millis % 1000) * 1000);
	return 0;
}

// The runtime flushes stdout before it terminates and parses exponents with
// `atoi`; newlib's versions bring all of stdio's FILE bookkeeping and the
// locale-aware `strtol`/ctype tables. Everything printed is already unbuffered
// (it goes straight to the log), and the text `atoi` sees is ASCII digits.
__attribute__((used)) int fflush(FILE *) { return 0; }
__attribute__((used)) int atoi(const char *text)
{
	while (*text == ' ') ++text;
	const bool negative = *text == '-';
	if (*text == '-' || *text == '+') ++text;
	int value = 0;
	while (*text >= '0' && *text <= '9') value = value * 10 + (*text++ - '0');
	return negative ? -value : value;
}
__attribute__((used)) int tolower(int character) { return character >= 'A' && character <= 'Z' ? character + ('a' - 'A') : character; }
__attribute__((used)) int toupper(int character) { return character >= 'a' && character <= 'z' ? character - ('a' - 'A') : character; }

// Static destructors never run: the app's memory is released whole when it
// exits, so registering them would only link the atexit machinery.
__attribute__((used)) int __cxa_atexit(void (*)(void *), void *, void *) { return 0; }

// Cortex-M3 has no 64-bit atomic instructions, and a Pebble app is a single
// task that nothing else writes into, so a plain read-modify-write is the
// whole of the atomicity these need.
unsigned long long __atomic_fetch_add_8(volatile void *pointer, unsigned long long value, int)
{
	auto *slot = static_cast<volatile unsigned long long *>(pointer);
	const unsigned long long previous = *slot;
	*slot = previous + value;
	return previous;
}
unsigned long long __atomic_fetch_sub_8(volatile void *pointer, unsigned long long value, int)
{
	auto *slot = static_cast<volatile unsigned long long *>(pointer);
	const unsigned long long previous = *slot;
	*slot = previous - value;
	return previous;
}
unsigned long long __atomic_load_8(const volatile void *pointer, int)
{
	return *static_cast<const volatile unsigned long long *>(pointer);
}
void __atomic_store_8(volatile void *pointer, unsigned long long value, int)
{
	*static_cast<volatile unsigned long long *>(pointer) = value;
}
bool __atomic_compare_exchange_8(volatile void *pointer, void *expected, unsigned long long desired, bool, int, int)
{
	auto *slot = static_cast<volatile unsigned long long *>(pointer);
	auto *wanted = static_cast<unsigned long long *>(expected);
	if (*slot == *wanted) {
		*slot = desired;
		return true;
	}
	*wanted = *slot;
	return false;
}
}  // extern "C"

// Pebble apps are a single position-independent image with no dynamic
// objects; `__dso_handle` only has to exist for static destructors to name.
extern "C" {
void *__dso_handle = nullptr;
}

// libstdc++'s clocks go through `gettimeofday`/`clock_gettime` and 64-bit
// arithmetic on timespecs; the host already counts in milliseconds.
__attribute__((used)) std::chrono::steady_clock::time_point std::chrono::steady_clock::now() noexcept
{
	return time_point(std::chrono::milliseconds(gea_pebble_host->now_ms()));
}
__attribute__((used)) std::chrono::system_clock::time_point std::chrono::system_clock::now() noexcept
{
	return time_point(std::chrono::milliseconds(static_cast<long long>(gea_pebble_host->epoch_ms())));
}

void *operator new(std::size_t size, std::align_val_t alignment) { return gea_pebble_aligned_alloc(size, static_cast<std::size_t>(alignment)); }
void *operator new[](std::size_t size, std::align_val_t alignment) { return gea_pebble_aligned_alloc(size, static_cast<std::size_t>(alignment)); }
void operator delete(void *pointer, std::align_val_t alignment) noexcept { gea_pebble_aligned_free(pointer, static_cast<std::size_t>(alignment)); }
void operator delete[](void *pointer, std::align_val_t alignment) noexcept { gea_pebble_aligned_free(pointer, static_cast<std::size_t>(alignment)); }
void operator delete(void *pointer, std::size_t, std::align_val_t alignment) noexcept { gea_pebble_aligned_free(pointer, static_cast<std::size_t>(alignment)); }
void operator delete[](void *pointer, std::size_t, std::align_val_t alignment) noexcept { gea_pebble_aligned_free(pointer, static_cast<std::size_t>(alignment)); }
