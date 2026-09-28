#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Who holds the app heap, from a program built with GEA_PEBBLE_ALLOC_TRACE=1:
//
//   node heap-report.mjs <program.elf> <pebble-logs.txt> [--at peak|end]
//
// Replays the traced allocations and frees, takes the live set at the peak (or
// at the last traced event), and charges each live block to the first program
// frame on its stack that is not allocator plumbing. Sizes include PebbleOS's
// 4-byte block header and 4-byte rounding, so they add up to what the heap
// actually spends.
import { execFileSync } from 'node:child_process'
import { readFileSync } from 'node:fs'
import path from 'node:path'
import os from 'node:os'

const [elf, logFile, ...rest] = process.argv.slice(2)
if (!elf || !logFile) {
  console.error('usage: heap-report.mjs <program.elf> <pebble-logs.txt> [--at peak|end]')
  process.exit(2)
}
const at = rest.includes('end') ? 'end' : 'peak'
const sdk = process.env.PEBBLE_SDK_ROOT || path.join(os.homedir(), 'Library/Application Support/Pebble SDK/SDKs/current')
const nm = path.join(sdk, 'toolchain/arm-none-eabi/bin/arm-none-eabi-nm')

const symbols = []
for (const line of execFileSync(nm, ['-S', '-n', '-C', elf], { encoding: 'utf8', maxBuffer: 64 << 20 }).split('\n')) {
  const match = line.match(/^([0-9a-f]+) ([0-9a-f]+) [tTwW] (.+)$/)
  if (match) symbols.push([Number.parseInt(match[1], 16), Number.parseInt(match[2], 16), match[3]])
}
const symbolAt = (address) => {
  for (const [start, size, name] of symbols) if (address >= start && address < start + size) return name
  return `?${address.toString(16)}`
}
const plumbing = /^(malloc|realloc|calloc|operator new|operator delete|OUTLINED_FUNCTION|\(anonymous namespace\)::trace|std::|__gnu_cxx|void std::|gea::detail::AllocationPool|gea::Ref<[^>]*> gea::makeRef|gea::makeRef)/
const owner = (frames) => frames.map(symbolAt).find((name) => !plumbing.test(name)) ?? symbolAt(frames[0] ?? 0)

const blockBytes = (size) => ((size + 3) & ~3) + 4
const live = new Map()
let liveBytes = 0
let peakBytes = 0
let snapshot = new Map()
for (const line of readFileSync(logFile, 'utf8').split('\n')) {
  const match = line.match(/ H([AF]) ([0-9a-f]{8})((?: [0-9a-f]{8})*)/)
  if (!match) continue
  const pointer = match[2]
  if (match[1] === 'F') {
    const block = live.get(pointer)
    if (block) liveBytes -= block.bytes
    live.delete(pointer)
    continue
  }
  const [size, ...frames] = match[3].trim().split(' ').map((word) => Number.parseInt(word, 16))
  const block = { bytes: blockBytes(size), frames }
  live.set(pointer, block)
  liveBytes += block.bytes
  if (liveBytes > peakBytes) {
    peakBytes = liveBytes
    if (at === 'peak') snapshot = new Map(live)
  }
}
if (at === 'end') snapshot = live

const byOwner = new Map()
let total = 0
for (const { bytes, frames } of snapshot.values()) {
  const name = owner(frames)
  const entry = byOwner.get(name) ?? { bytes: 0, count: 0 }
  entry.bytes += bytes
  entry.count += 1
  byOwner.set(name, entry)
  total += bytes
}
console.log(`live at ${at}: ${total} B in ${snapshot.size} blocks (peak ${peakBytes} B)`)
for (const [name, { bytes, count }] of [...byOwner].sort((a, b) => b[1].bytes - a[1].bytes)) {
  console.log(`${String(bytes).padStart(6)}  ${String(count).padStart(4)}x  ${name.slice(0, 140)}`)
}
