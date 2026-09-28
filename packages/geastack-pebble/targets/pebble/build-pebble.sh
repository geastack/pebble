#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# Build a Gea app as a Pebble app (.pbw) for Pebble Time 2 (emery).
#
#   bash targets/pebble/build-pebble.sh <app-dir> [--entry index.tsx]
#                                       [--install emulator|<phone-ip>]
#
# The app does not replace the watch firmware; it installs like any other
# Pebble app. Two pieces come out of the build (see runtime/pebble_bridge.h):
#
#   * the shell -- shell/pebble_app.c, built by the Pebble SDK as the app
#     image. It owns the window, fonts, buttons, touch and timers.
#   * the program -- the compiled TSX, the Gea runtime and the Pebble UI
#     backend, linked -pie into one blob that is compiled into the app image
#     as data, which the shell relocates at launch.
#
# Output: <app-dir>/dist/pebble/<app-id>.pbw
#
# Requires the Pebble SDK (`uv tool install pebble-tool`, then `pebble sdk
# install latest`). The framework packages resolve from the app's
# node_modules (GEA_CORE and friends override them); the compiler is the one
# @geastack/core installs unless GEA_GEATSC_BIN names another `dist/cli.js`.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

APP_DIR=""
ENTRY="index.tsx"
INSTALL=""
while [ $# -gt 0 ]; do
	case "$1" in
	--entry) ENTRY="$2"; shift 2 ;;
	--install) INSTALL="$2"; shift 2 ;;
	-*) echo "build-pebble: unknown option $1" >&2; exit 2 ;;
	*) APP_DIR="$1"; shift ;;
	esac
done
[ -n "$APP_DIR" ] || { echo "usage: build-pebble.sh <app-dir> [--entry index.tsx] [--install emulator|<phone-ip>]" >&2; exit 2; }
APP_DIR="$(cd "$APP_DIR" && pwd)"

# The framework comes from the app's own dependencies, as for every other
# target: @geastack/core and the engine, host, elements and geaos packages
# installed beside it.
resolve_package() {
	node -e 'try { console.log(require("path").dirname(require.resolve(process.argv[1] + "/package.json", { paths: [process.argv[3], process.argv[2]] }))) } catch { console.error("build-pebble: cannot resolve " + process.argv[1] + " (run npm install in the app)"); process.exit(1) }' "$1" "$APP_DIR" "$HERE"
}
GEA_CORE="${GEA_CORE:-$(resolve_package @geastack/core)}"
[ -n "$GEA_CORE" ] && [ -f "$GEA_CORE/package.json" ] || { echo "build-pebble: cannot resolve @geastack/core from $APP_DIR (run npm install there)" >&2; exit 1; }
GEA_HOST_DIR="${GEA_HOST_DIR:-$(resolve_package @geastack/host)}"
GEA_ENGINE_DIR="${GEA_ENGINE_DIR:-$(resolve_package @geastack/engine)}"
GEA_ELEMENTS_DIR="${GEA_ELEMENTS_DIR:-$(resolve_package @geastack/elements)}"
GEA_GEAOS_PACKAGE_DIR="${GEA_GEAOS_PACKAGE_DIR:-$(resolve_package @geastack/geaos)}"

APP_ID="$(node -e 'const p=require(process.argv[1]); console.log((p.gea&&p.gea.id)||p.name.replace(/^@[^/]+\//,""))' "$APP_DIR/package.json")"
APP_NAME="$(node -e 'const p=require(process.argv[1]); console.log((p.gea&&p.gea.name)||process.argv[2])' "$APP_DIR/package.json" "$APP_ID")"

SDK_ROOT="${PEBBLE_SDK_ROOT:-$HOME/Library/Application Support/Pebble SDK/SDKs/current}"
[ -d "$SDK_ROOT" ] || SDK_ROOT="$HOME/.pebble-sdk/SDKs/current"
TOOLCHAIN="$SDK_ROOT/toolchain/arm-none-eabi/bin"
CXX="$TOOLCHAIN/arm-none-eabi-g++"
[ -x "$CXX" ] || { echo "build-pebble: no Pebble SDK toolchain at $TOOLCHAIN (run: pebble sdk install latest)" >&2; exit 1; }
PEBBLE="$(command -v pebble || echo "$HOME/.local/bin/pebble")"

OUT="$APP_DIR/dist/pebble"
GEN="$OUT/.generated/$APP_ID"
OBJ="$OUT/obj"
PROJECT="$OUT/project"
mkdir -p "$GEN" "$OBJ" "$PROJECT/src/c" "$PROJECT/resources"

echo "==> compile $APP_ID (TSX -> C++)"
GEATSC_ARGS=()
[ -n "${GEA_GEATSC_BIN:-}" ] && GEATSC_ARGS=(--geatsc-bin "$GEA_GEATSC_BIN")
# The compiled UI (plugin/index.mjs) resolves the template, styles and layout
# at build time and leaves the program only its drawing. A program it cannot
# express keeps the engine; GEA_PEBBLE_UI=engine asks for the engine outright.
[ "${GEA_PEBBLE_UI:-}" = engine ] || GEATSC_ARGS+=(--extra-geatsc-plugin "$HERE/plugin/index.mjs")
rm -f "$GEN/pebble-ui.json"
node "$GEA_CORE/scripts/build-gea-vite-geatsc.mjs" \
	--app-dir "$APP_DIR" --entry "$ENTRY" --out-dir "$GEN" \
	--gea-ir-backend --pixel-panel-endian 1 \
	--font-viewport-width 200 --font-viewport-height 228 --font-device-pixel-ratio 1.0 \
	${GEATSC_ARGS[@]+"${GEATSC_ARGS[@]}"} >"$OBJ/codegen.log" 2>&1 || { tail -40 "$OBJ/codegen.log" >&2; exit 1; }

UI_UNIT=pebble_ui
if [ -f "$GEN/pebble-ui.json" ] && grep -q '"mode": "compiled"' "$GEN/pebble-ui.json"; then
	UI_UNIT=pebble_compiled_ui
	echo "    UI: compiled at build time"
else
	echo "    UI: engine ($(node -e 'try{console.log(require(process.argv[1]).reason)}catch{console.log("no compiled-UI decision")}' "$GEN/pebble-ui.json"))"
fi


# Cortex-M3, soft float, position independent: the code PebbleOS runs apps
# as. Size is the whole game -- code and heap share 128 KB -- so the program is
# link-time optimized, and a single-task app needs neither locked statics nor
# RTTI.
COMMON_FLAGS=(
	-std=c++20 -mcpu=cortex-m3 -mthumb -fPIE -fno-threadsafe-statics -fno-rtti
	-ffunction-sections -fdata-sections
	-include "$HERE/include/pebble_prelude.h"
	-DGEA_EMBEDDED_HAS_GENERATED_FONTS=1 -DNDEBUG
	-I"$GEN" -I"$HERE/runtime"
	-I"$GEA_CORE/include" -I"$GEA_CORE"
	-I"$GEA_HOST_DIR/include" -I"$GEA_HOST_DIR"
	-I"$GEA_ENGINE_DIR" -I"$GEA_ENGINE_DIR/ui"
	-I"$GEA_ELEMENTS_DIR" -I"$GEA_ELEMENTS_DIR/ui"
	-I"$GEA_GEAOS_PACKAGE_DIR"
)
# GEA_PEBBLE_ALLOC_TRACE=1 logs every heap allocation with its program call
# sites (pebble_support.cpp), for tools/heap-report.mjs.
[ "${GEA_PEBBLE_ALLOC_TRACE:-}" = 1 ] && COMMON_FLAGS+=(-DGEA_PEBBLE_ALLOC_TRACE=1)
[ "$UI_UNIT" = pebble_compiled_ui ] && COMMON_FLAGS+=(-DGEA_PEBBLE_COMPILED_UI=1)

# LLVM builds the program when a clang and ld.lld of one release are
# installed, against the SDK's own newlib-nano and libstdc++. It is ~15%
# smaller than GCC for the same source: -Oz plus the machine outliner, and
# full LTO with virtual function elimination, which drops the reflection hooks
# every class vtable names but a program never calls -- GCC keeps every
# virtual a vtable references. GEA_PEBBLE_TOOLCHAIN=gcc forces the SDK's GCC.
find_llvm() {
	[ "${GEA_PEBBLE_TOOLCHAIN:-}" = gcc ] && return 1
	local lld clang candidate
	lld="$(command -v ld.lld || true)"
	[ -n "$lld" ] || return 1
	for candidate in "${GEA_PEBBLE_CLANG:-}" "$(dirname "$lld")/clang++" /opt/homebrew/opt/llvm/bin/clang++ /usr/local/opt/llvm/bin/clang++; do
		[ -n "$candidate" ] && [ -x "$candidate" ] || continue
		# Bitcode is read by the linker's own LLVM: the majors must agree.
		if [ "$("$candidate" --version | sed -n 's/.*clang version \([0-9]*\).*/\1/p' | head -1)" = "$("$lld" --version | sed -n 's/.*LLD \([0-9]*\).*/\1/p')" ]; then
			CLANG="$candidate"
			LLD="$lld"
			return 0
		fi
	done
	return 1
}

echo "==> cross-compile program"
PROGRAM_OBJECTS=("$OBJ/pebble_program.o" "$OBJ/index.o" "$OBJ/$UI_UNIT.o" "$OBJ/pebble_support.o" "$OBJ/pebble_services.o")
if find_llvm; then
	echo "    (LLVM: $CLANG)"
	SYSROOT="$SDK_ROOT/toolchain/arm-none-eabi/arm-none-eabi"
	GCC_VERSION="$("$CXX" -dumpversion)"
	MULTILIB="$("$CXX" -mcpu=cortex-m3 -mthumb -print-multi-directory)"
	CXXFLAGS=(
		--target=thumbv7m-none-eabi -mfloat-abi=soft -nostdlibinc
		-isystem "$SYSROOT/include/newlib-nano"
		-isystem "$SYSROOT/include/c++/$GCC_VERSION"
		-isystem "$SYSROOT/include/c++/$GCC_VERSION/arm-none-eabi/$MULTILIB"
		-isystem "$SYSROOT/include/c++/$GCC_VERSION/backward"
		-isystem "$SYSROOT/include"
		-Oz -flto=full -fvisibility=hidden -fwhole-program-vtables -fvirtual-function-elimination -fno-c++-static-destructors -fignore-exceptions
		"${COMMON_FLAGS[@]}"
	)
	compile() { "$CLANG" "${CXXFLAGS[@]}" "$@"; }
else
	CXXFLAGS=(-Os -flto -Wno-psabi "${COMMON_FLAGS[@]}")
	compile() { "$CXX" "${CXXFLAGS[@]}" "$@"; }
fi
compile -c "$GEN/index.cpp" -o "$OBJ/index.o" &
# The Pebble-side runtime throws nothing, so it carries no unwind tables. A JS
# exception that escapes a handler through it was uncaught anyway.
for unit in pebble_program "$UI_UNIT" pebble_support pebble_services; do
	compile -fno-exceptions -c "$HERE/runtime/$unit.cpp" -o "$OBJ/$unit.o" &
done
wait_all() { local status=0; for job in $(jobs -p); do wait "$job" || status=1; done; return $status; }
wait_all

echo "==> link program"
if [ -n "${LLD:-}" ]; then
	# GNU ld turns the absolute words newlib's non-PIC objects hold into
	# R_ARM_RELATIVE entries by itself; lld does it only when told text
	# relocations are acceptable -- and the whole image is writable RAM.
	"$LLD" -pie -z notext --no-dynamic-linker --gc-sections --no-undefined \
		--lto-whole-program-visibility -mllvm -enable-machine-outliner \
		-T "$HERE/runtime/pebble_program.ld" -Map "$OBJ/program.map" \
		"${PROGRAM_OBJECTS[@]}" \
		-L"$SYSROOT/lib/$MULTILIB" -L"$(dirname "$("$CXX" -mcpu=cortex-m3 -mthumb -print-libgcc-file-name)")" \
		--start-group -lstdc++_nano -lsupc++_nano -lm -lc_nano -lgcc --end-group \
		-o "$OBJ/program.elf"
else
	"$CXX" -mcpu=cortex-m3 -mthumb -Os -flto -fPIE -pie --specs=nano.specs -nostartfiles \
		-Wl,--no-dynamic-linker -Wl,--no-warn-rwx-segments -Wl,--gc-sections -Wl,--no-undefined \
		-Wl,-T,"$HERE/runtime/pebble_program.ld" -Wl,-Map,"$OBJ/program.map" \
		"${PROGRAM_OBJECTS[@]}" \
		-o "$OBJ/program.elf"
fi
rm -f "$PROJECT/resources/gea_program.bin"
node "$HERE/tools/pack-program.mjs" "$OBJ/program.elf" "$PROJECT/src/c/gea_program_image.inc" "$TOOLCHAIN"

echo "==> Pebble project"
# A stable UUID per app id, so reinstalling replaces the app instead of adding
# a second copy.
UUID="$(node -e '
const h=require("crypto").createHash("sha1").update("geastack-pebble:"+process.argv[1]).digest();
h[6]=(h[6]&0x0f)|0x50; h[8]=(h[8]&0x3f)|0x80;
const x=h.subarray(0,16).toString("hex");
console.log([x.slice(0,8),x.slice(8,12),x.slice(12,16),x.slice(16,20),x.slice(20)].join("-"))' "$APP_ID")"
# enableMultiJS and a message key are what the SDK's project template writes;
# without them the watch's app menu did not list a sideloaded app (Pebble Time
# 2, firmware 4.36, installed through the iPhone app).
cat >"$PROJECT/package.json" <<JSON
{
  "name": "$APP_ID",
  "author": "geastack",
  "version": "1.0.0",
  "private": true,
  "dependencies": {},
  "pebble": {
    "displayName": "$APP_NAME",
    "uuid": "$UUID",
    "sdkVersion": "3",
    "enableMultiJS": true,
    "messageKeys": ["dummy"],
    "targetPlatforms": ["emery"],
    "watchapp": { "watchface": false },
    "resources": { "media": [] }
  }
}
JSON
cp "$HERE/shell/wscript" "$PROJECT/wscript"
cp "$HERE/shell/pebble_app.c" "$HERE/runtime/pebble_bridge.h" "$PROJECT/src/c/"

(cd "$PROJECT" && "$PEBBLE" build) >"$OBJ/pebble-build.log" 2>&1 || { tail -40 "$OBJ/pebble-build.log" >&2; exit 1; }
cp "$PROJECT/build/project.pbw" "$OUT/$APP_ID.pbw" 2>/dev/null || cp "$PROJECT"/build/*.pbw "$OUT/$APP_ID.pbw"
echo "==> $OUT/$APP_ID.pbw"

if [ -n "$INSTALL" ]; then
	if [ "$INSTALL" = "emulator" ]; then
		(cd "$PROJECT" && "$PEBBLE" install --emulator emery)
	else
		(cd "$PROJECT" && "$PEBBLE" install --phone "$INSTALL")
	fi
fi
