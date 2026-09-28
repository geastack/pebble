#!/usr/bin/env node
// SPDX-License-Identifier: Apache-2.0
// Turns the font-metrics probe's log (tools/font-metrics, run on the emulator
// with `pebble logs` capturing) into runtime/font-metrics.json, the table the
// compiled-UI plugin lays text out with:
//
//   node font-metrics.mjs <probe-log> [out.json]
//
// A line's width is the sum of its glyphs' advances -- the probe measures
// whole strings too, and this refuses a log where the two disagree, because
// then the table would not predict what the firmware draws.
import { readFileSync, writeFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const [logFile, outFile = path.join(here, '../runtime/font-metrics.json')] = process.argv.slice(2)
if (!logFile) {
  console.error('usage: font-metrics.mjs <probe-log> [out.json]')
  process.exit(2)
}
const samples = ['Counter', 'Reset', '-1', '+', 'Below zero', 'Back at zero', 'Counting up', 'WAVE Ag|j']
const fonts = []
const checks = []
for (const line of readFileSync(logFile, 'utf8').split('\n')) {
  let match = line.match(/FM font (\d+) empty \d+ \d+ line \d+ (\d+) two \d+ (\d+)/)
  if (match) {
    fonts[Number(match[1])] = { lineHeight: Number(match[2]), advances: [] }
    if (Number(match[3]) !== 2 * Number(match[2])) throw new Error(`font ${match[1]}: two lines are not twice one line`)
    continue
  }
  match = line.match(/FM glyphs (\d+) (\d+)((?: \d+)+)/)
  if (match) {
    const widths = match[3].trim().split(' ').map(Number)
    widths.forEach((width, offset) => (fonts[Number(match[1])].advances[Number(match[2]) - 32 + offset] = width))
    continue
  }
  match = line.match(/FM sample (\d+) (\d+) (\d+) (\d+)/)
  if (match) checks.push({ font: Number(match[1]), text: samples[Number(match[2])], width: Number(match[3]), height: Number(match[4]) })
}
if (fonts.length !== 13 || fonts.some((font) => !font || font.advances.length !== 95)) throw new Error('incomplete probe log')
for (const check of checks) {
  const sum = [...check.text].reduce((total, char) => total + fonts[check.font].advances[char.charCodeAt(0) - 32], 0)
  if (sum !== check.width) throw new Error(`font ${check.font} "${check.text}": glyph sum ${sum}, firmware ${check.width}`)
  if (check.height !== fonts[check.font].lineHeight) throw new Error(`font ${check.font} "${check.text}": height ${check.height}`)
}
writeFileSync(outFile, JSON.stringify({ firstCode: 32, fonts }) + '\n')
console.log(`wrote ${outFile}: ${fonts.length} fonts, ${checks.length} samples agree`)
