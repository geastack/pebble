# @geastack/pebble

Build a Gea TSX app into an ordinary Pebble app (`.pbw`) for **Pebble Time 2**
(`emery`: 200×228, 64 colours, touch, four buttons). The watch firmware is not
touched: the app installs through the Pebble phone app or the SDK emulator,
next to every other Pebble app. It does the job Alloy + Poco do, but your
components are compiled to native code instead of running on a JavaScript
engine.

```bash
gea build --target pebble            # -> dist/pebble/<app-id>.pbw
gea run   --target pebble            # build, then install on the emery emulator
gea run   --target pebble --phone 192.168.1.23   # install on a watch via its phone
```

An app opts in with `"pebble": true` under `gea.targets` in its `package.json`
and a dependency on `@geastack/pebble`.

## Getting started

Use **@geastack/cli 0.1.85 or newer**. In your existing Gea app, install:

```sh
npm install @geastack/core@^0.1.26 @geastack/pebble@^0.1.0
npm install --save-dev @geastack/cli@^0.1.85
```

Preserve your existing manifest and add `"pebble": true` to `gea.targets`.
The CLI forwards `gea.entry`, including custom paths such as `src/watch.tsx`.
Use `npx gea` for the commands above when the CLI is installed locally.

The [complete quickstart](https://github.com/geastack/cli/blob/main/docs/PEBBLE-QUICKSTART.md)
covers the SDK, manifest, emulator, phone developer connection, logs and
limits. Only Pebble Time 2 (`emery`) is supported; use macOS or Linux with Bash.
The package installs its native framework dependencies automatically.

## Requirements

- The Pebble SDK: `uv tool install --python 3.13 pebble-tool`, then
  `pebble sdk install latest`. The build uses the SDK's `arm-none-eabi` GCC and
  its `pebble build`/`pebble install`.
- `@geastack/core` ≥ 0.1.26 and the engine, host, elements and geaos packages
  installed automatically (they resolve from the app or Pebble package; `GEA_CORE`
  and `GEA_{HOST,ENGINE,ELEMENTS,GEAOS_PACKAGE}_DIR` override them).
- `GEA_GEATSC_BIN` can point the build at a specific compiler `dist/cli.js`.

## How it works

A Gea app is two pieces linked apart and shipped as one ordinary app image:

| piece | what it is | built by |
| --- | --- | --- |
| **shell** (`targets/pebble/shell/pebble_app.c`) | one window, the system fonts, buttons, touch, timers; starts the program | the Pebble SDK (`pebble build`) |
| **program** (`src/c/gea_program_image.inc` in the generated project) | the compiled TSX, the Gea runtime and the Pebble UI backend, linked `-pie` by LLVM and compiled into the image as one array | `targets/pebble/build-pebble.sh` |

At launch the shell applies the program's `R_ARM_RELATIVE` relocations where
the firmware loaded it and calls its entry table (`GeaPebbleProgram`). The
program runs its own static constructors and reaches PebbleOS only through the
host table (`GeaPebbleHost`) the shell passes it. `runtime/pebble_bridge.h` is
the whole contract. PebbleOS loads at most 64 KB of app image
(`PebbleProcessInfo.load_size` is a `uint16_t`); the counter's is 14 KB and
bouncing balls' 54 KB.

On the Pebble Time 2 (SiFli SF32LB52) a store-exclusive to app RAM never
succeeds, so an LDREX/STREX retry loop -- any atomic from libstdc++ or libgcc
-- hangs the app, while the emulator runs it. `pebble_support.cpp` replaces
the one the runtime used (`std::set_terminate`), and the build refuses a
program that still contains one.

### The compiled UI

Everything the UI engine works out on the watch -- the template's shape, the
cascade over its classes, the flexbox layout, the text metrics -- is known when
the app is built. So the build loads a geatsc plugin
(`targets/pebble/plugin/`) that works it out then:

- it lays the mounted component's page out with the engine's own rules and the
  firmware's measured font metrics (`runtime/font-metrics.json`, captured by
  `tools/font-metrics` on the emulator);
- it writes the page into the program as constant tables -- the draw
  operations in the engine's order and the click targets -- and gives the
  component a draw method that walks them and a mount method that registers
  its handlers (`runtime/pebble_compiled_ui.cpp` does focus, scrolling, touch);
- text the program computes at run time (`{this.count}`) is laid out at many
  widths during the build to find the fixed box and alignment the engine would
  have put it at; if it would push another box around, the page is refused;
- reactivity is the compiler's: a reactive field is still a `Signal`, and a
  write that changes it asks the watch for a redraw (`runtime/ui/signal.h`).

A program the plugin cannot express (a list, a condition, a child component, a
style call it does not know) keeps the engine; the build prints which UI it
chose and why, and `GEA_PEBBLE_UI=engine` forces the engine. On the counter the
compiled page is pixel-identical to the engine's, before and after each button.

### The engine

The UI backend (`runtime/pebble_ui.cpp`) implements the engine's node, style
and document API over a small flexbox layout, and paints with PebbleOS graphics
and the system fonts, so no font or renderer ships in the app. Class and
element rules cascade as in a browser, and inline styles (`style={{ left: x }}`,
reactive per member) apply after them: lengths in px, %, vw/vh/vmin/vmax,
colours in hex, `rgb()`/`rgba()` and the basic names, `border-radius` (a square
with half its side as radius is drawn as a true circle), `font-weight`, and the
`display`/`position`/`flex-direction`/`text-align` keywords.
`requestAnimationFrame` rides the shell's frame timer at up to 30 frames a
second (`Display.setFrameRate` lowers it). On the watch:

- **UP / DOWN** move focus between controls (elements with a click or touch
  handler), scrolling the page to keep the focused one on screen. **SELECT**
  clicks it. **BACK** exits.
- **Touch** taps the element under the finger, as on the web.

## Release validation

The initial npm release builds `counter-jsx` with the published compiler
1.0.18, core 0.1.26 and engine 0.1.6. The resulting `.pbw` packages successfully
with the installed Pebble SDK. Physical-watch behavior was tested before this
release; the new packaging and random hook have build validation only.

`Math.random` uses the SDK's PRNG, seeded at app startup, through bridge ABI 2.
It is not a cryptographic entropy source. The shell and program are rebuilt
together so their ABI versions stay aligned.

## Size

The figures below are earlier measurements, not a guarantee for every compiler
version. Rebuild and use the size-report tool for the installed toolchain.

Code and heap share the 128 KB. With the compiled UI the counter example is a
9.9 KB program (a 19.5 KB `.pbw`) that peaks at 1.3 KB of heap, leaving 116 KB
free after it loads. `bouncing-balls-jsx` keeps the engine: a 49 KB program
with 64 nodes, each with three reactive style members, leaving 75.7 KB of heap
at launch.
`tools/size-report.mjs <program.elf>` shows where the bytes go, by area, and a
build with `GEA_PEBBLE_ALLOC_TRACE=1` logs every allocation for
`tools/heap-report.mjs`. What keeps it there:

- **LLVM when installed.** With a clang and `ld.lld` of the same release on the
  machine (`brew install llvm lld`), the program is built `-Oz` with the machine
  outliner and full LTO with virtual function elimination, against the SDK's
  newlib-nano and libstdc++. GCC remains the fallback;
  `GEA_PEBBLE_TOOLCHAIN=gcc` forces it.
- **No unwinder, no static destructors.** `GEA_RUNTIME_THROW_ENDS_PROGRAM`
  makes every `throw` (spelled `GEA_THROW` by the emitter) log and end the app
  without boxing the thrown value, so `catch` never runs and no landing pads
  are emitted (`-fignore-exceptions`). `pebble_support.cpp` stubs the C++
  exception runtime and the linker script discards the unwind tables. Static
  destructors are never registered: the process ends with the app.
- **A table-free number formatter.** `GEA_CPP_COMPACT_NUMBER_FORMAT` prints
  shortest round-trip digits and `toFixed`/`toExponential` with a small bignum
  instead of `to_chars` (128 KB of Ryu tables) or newlib's printf/dtoa (16 KB),
  and `vsnprintf` is a small integer-only engine. Reactive text of an integer
  field is printed as an integer (16-bit-limb division, no 64-bit libgcc
  divide), and a thunk that only reads one field is replaced by a direct
  member reader, so the integer never widens to `double`.
- **Fixed-point styles.** Lengths and percentages are 1/16-pixel integers; the
  float literals the stylesheet registers are decoded from their bits, so the
  style engine links no soft float.
- **Constant stylesheet tapes.** The static CSS registration is `constexpr`
  arrays in flash-like rodata rather than code that builds vectors.
- **Compact runtime code.** `GEA_RUNTIME_COMPACT_CODE` makes `Ref` release
  out of line, keeps node subscriptions in a linked list (a vector's doubling
  needs both tables alive at once), shares one live token per node, and only
  links the native-expando dropper when a program tags an expando;
  `thread_local` is compiled away (a Pebble app is one task).
- **No allocation pools.** `GEA_RUNTIME_COMPACT_ALLOCATION` gives every `Ref`
  cell its own heap block, freed at once: pooled chunks are never returned and
  only serve their own size class, which on a heap of tens of kilobytes
  strands what the program needs. Aligned `operator new` goes straight to the
  PebbleOS heap (newlib's `memalign` frees its pieces through a heap it does
  not own).
- **A collection safepoint per frame.** The shell brackets animation-frame
  callbacks with the program's cycle-collection deferral, so a frame that only
  moves numbers -- and so allocates nothing -- still lets the collector drain
  the candidates its reactive reads queued.
- **Small nodes.** Lengths are 4 bytes, tag names are shared, element ids live
  in a side table, and an inline declaration is keyed by the property it sets
  rather than by a copy of its name. libc's `atoi`, `tolower`,
  `toupper`, `fflush`, `printf` and the chrono clocks are replaced by small
  versions over the host.
- No RTTI, newlib-nano, and the Gea runtime keeps its builtin function tables
  and `Ref` operation tables constant-initialized, so the linker can drop what a
  program never touches.

For scale, the same counter written against the C SDK is a 6.8 KB `.pbw` using
0.3 KB of heap, and in Alloy (Moddable XS in firmware, Poco) a 17 KB `.pbw`
using 8.6 KB of heap; the programs are in [comparison](https://github.com/geastack/pebble/tree/main/comparison).
