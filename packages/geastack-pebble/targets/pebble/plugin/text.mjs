// SPDX-License-Identifier: Apache-2.0
// Text as the watch sets it, known at build time.
//
// The font table and `fontFor` are the shell's (shell/pebble_app.c), and the
// metrics are the firmware's own: runtime/font-metrics.json is captured by
// running tools/font-metrics on the emulator, and a line's width there is
// exactly the sum of its glyph advances (the capture checks that against
// whole-string measurements before writing the table).
import { readFileSync } from 'node:fs'
import path from 'node:path'
import { fileURLToPath } from 'node:url'

const here = path.dirname(fileURLToPath(import.meta.url))
const metrics = JSON.parse(readFileSync(path.join(here, '../runtime/font-metrics.json'), 'utf8'))

// { size, bold } in the shell's s_fonts order; the index is what the host takes.
const fonts = [
  [9, false],
  [14, false],
  [14, true],
  [18, false],
  [18, true],
  [21, false],
  [24, false],
  [24, true],
  [28, false],
  [28, true],
  [30, true],
  [42, false],
  [42, true]
]

/** The shell's `host_font_for`. */
export const fontFor = (sizePx, weight) => {
  const bold = weight >= 600
  let best = 3
  let bestScore = 1 << 30
  fonts.forEach(([size, isBold], index) => {
    const delta = size - sizePx
    let score = (delta > 2 ? delta * 3 : delta < 0 ? -delta : delta) * 4
    if (isBold !== bold) score += 3
    if (score < bestScore) {
      bestScore = score
      best = index
    }
  })
  return best
}

export const lineHeight = (font) => metrics.fonts[font].lineHeight

const advance = (font, char) => {
  const code = char.charCodeAt(0) - metrics.firstCode
  const width = metrics.fonts[font].advances[code]
  if (width === undefined) throw new Error(`no metrics for ${JSON.stringify(char)}`)
  return width
}

export const textWidth = (font, text) => [...text].reduce((total, char) => total + advance(font, char), 0)

/**
 * `graphics_text_layout_get_content_size` with word wrap, as the host calls it
 * (`host_measure_text`: a 2000 px bound when no width is given). Returns null
 * when the text would wrap: the firmware's line breaking is not modelled, and
 * a layout that depends on it is not one this build can state.
 */
export const measureText = (font, text, maxWidth) => {
  const bound = maxWidth > 0 ? maxWidth : 2000
  if (text.includes('\n')) return null
  const width = textWidth(font, text)
  if (width > bound) return null
  return { w: text.length ? width : 0, h: text.length ? lineHeight(font) : 0 }
}
