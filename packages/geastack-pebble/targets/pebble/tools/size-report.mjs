#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Where the bytes of a linked program blob go, by area of the stack:
//
//   node size-report.mjs <program.elf> [--top N]
//
// Code and heap share an app's 128 KB, so every area here is heap an app does
// not get. Aliases (one body under two names, as libgcc's soft float has) are
// counted once.
import { execFileSync } from 'node:child_process'
import path from 'node:path'
import os from 'node:os'

const [elf, ...rest] = process.argv.slice(2)
if (!elf) {
  console.error('usage: size-report.mjs <program.elf> [--top N]')
  process.exit(2)
}
const topIndex = rest.indexOf('--top')
const top = topIndex >= 0 ? Number(rest[topIndex + 1]) : 6
const sdk = process.env.PEBBLE_SDK_ROOT || path.join(os.homedir(), 'Library/Application Support/Pebble SDK/SDKs/current')
const nm = path.join(sdk, 'toolchain/arm-none-eabi/bin/arm-none-eabi-nm')

const areas = [
  ['pebble ui', /gea::pebble|gea_pebble|StyleSheet|Static\w*Style/],
  ['soft float / libgcc', /^__(aeabi|\w+[ds]f\d|udiv|div|mul|ashl|lshr|clz|fix|float|cmp|gnu|eq|ne|lt|gt|le|ge|unord|popcount)/],
  ['jsx', /gea::jsx|NodeHandle|embedded::ui|Subscription/],
  ['number format', /compactnumber|toString.*double|generalFormat|formatDouble|toCharsToEcma|vsnprintf|digitsOf|putPadded/],
  ['cycle collector', /[Cc]ycle|collect|[Tt]race|[Ss]afepoint/],
  ['reflection', /PropertyKey|PropertyDescriptor|ownField|Reflect|gea_own|KeyOrder|xpando|stable_sort|merge/],
  ['app code', /gea_body|gea_class|gea_construct|__gea_top|gea_fn|Plugin|__cxx_global/],
  ['functions / promises', /std::function|Callable|_Function|HostFunction|[Pp]romise|[Mm]icrotask/],
  ['refs / allocation', /Ref<|Allocation|Pool|release|destroy|malloc|free/],
  ['libc / libstdc++', /std::|basic_string|operator new|operator delete|^mem|^str|^_|abort|printf|floor|ceil/],
  ['other gea', /gea::/]
]
const totals = new Map()
const seen = new Set()
for (const line of execFileSync(nm, ['-S', '--size-sort', '-C', elf], { encoding: 'utf8', maxBuffer: 64 << 20 }).split('\n')) {
  const match = line.match(/^([0-9a-f]+) ([0-9a-f]+) \S (.+)$/)
  if (!match || seen.has(match[1])) continue
  seen.add(match[1])
  const [, , size, name] = match
  const area = areas.find(([, pattern]) => pattern.test(name))?.[0] ?? 'other'
  const entry = totals.get(area) ?? { bytes: 0, symbols: [] }
  entry.bytes += Number.parseInt(size, 16)
  entry.symbols.push([Number.parseInt(size, 16), name])
  totals.set(area, entry)
}
let sum = 0
for (const [area, { bytes, symbols }] of [...totals].sort((a, b) => b[1].bytes - a[1].bytes)) {
  sum += bytes
  console.log(`${String(bytes).padStart(6)}  ${area}`)
  for (const [size, name] of symbols.sort((a, b) => b[0] - a[0]).slice(0, top)) console.log(`        ${String(size).padStart(5)}  ${name.slice(0, 120)}`)
}
console.log(`${String(sum).padStart(6)}  total`)
