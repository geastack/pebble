// SPDX-License-Identifier: Apache-2.0
// A mounted page, resolved: every box the engine would lay out, as the
// constant rectangles it would land on, and the draw calls the engine's draw
// pass would make for them, in its order.
//
// The page is the engine's document: `html > body > div#app >` the mounted
// component's template. Styles cascade over it exactly as the engine's
// `computeStyles` does, and `layout.mjs` places it exactly as `place` does.
//
// Text a program computes at run time is the one thing a build cannot lay
// out. Each such text node is laid out at several widths, and what moves is
// examined: if only the text itself and invisible boxes around it move, and
// the text sits at a fixed left edge, right edge or centre, the text is drawn
// at run time in a fixed box with that alignment, which is where the engine
// would have put it. Anything else (a decorated box that grows with the text,
// a sibling pushed along by it) needs layout on the watch, and the page is
// refused so the program keeps the engine.
import { computeStyle } from './styles.mjs'
import { layout, LayoutError } from './layout.mjs'
import { fontFor } from './text.mjs'

export class PlanError extends Error {}

const page = (content) => {
  const node = (tag, children, id = '') => ({ tag, id, classes: [], text: false, children, content: '', dynamic: null, click: null })
  return node('html', [node('body', [node('div', [content], 'app')])])
}

const walk = (node, visit, parent = null) => {
  visit(node, parent)
  for (const child of node.children) walk(child, visit, node)
}

const textTopInset = (fontSize) => (fontSize >= 28 ? Math.trunc(fontSize / 5) : Math.trunc(fontSize / 4))

const sameBox = (a, b) => a.x === b.x && a.y === b.y && a.w === b.w && a.h === b.h

// Widths each run-time text is tried at: even and odd, so a centring that
// rounds is seen to round.
const samples = [0, 1, 2, 3, 7, 8, 15, 16, 31, 32, 47, 48, 63, 64]
const baseWidth = 20

/** The alignment and box that put run-time text where the engine would, or a reason it cannot be. */
const fitDynamicText = (root, screen, nodes, target) => {
  const snapshots = samples.map((width) => {
    layout(root, screen, (node) => (node === target ? width : baseWidth))
    return new Map(nodes.map((node) => [node, { box: { ...node.box }, inner: node.inner ? { ...node.inner } : null }]))
  })
  const chain = new Set()
  const parents = new Map()
  walk(root, (node, parent) => parents.set(node, parent))
  for (let node = target; node; node = parents.get(node)) {
    const moved = snapshots.some((snapshot) => !sameBox(snapshot.get(node).box, snapshots[0].get(node).box))
    if (!moved) break
    chain.add(node)
  }
  for (const node of nodes) {
    if (chain.has(node)) continue
    if (snapshots.some((snapshot) => !sameBox(snapshot.get(node).box, snapshots[0].get(node).box)))
      throw new PlanError(`run-time text ${target.dynamic.expr} moves another box`)
  }
  for (const node of chain) {
    if (node === target) continue
    if (node.style.hasBackground || node.style.borderWidth > 0) throw new PlanError(`run-time text ${target.dynamic.expr} resizes a drawn box`)
    if (node.click) throw new PlanError(`run-time text ${target.dynamic.expr} resizes a control`)
  }
  const inner = samples.map((width, i) => ({ width, ...snapshots[i].get(target).inner }))
  if (inner.some((box) => box.y !== inner[0].y || box.h !== inner[0].h || box.w !== box.width))
    throw new PlanError(`run-time text ${target.dynamic.expr} is not one line in a box of its own width`)
  if (inner.every((box) => box.x === inner[0].x)) return { align: 0, x: inner[0].x, y: inner[0].y, w: Math.max(1, screen.width - inner[0].x), h: inner[0].h }
  if (inner.every((box) => box.x + box.width === inner[0].x)) return { align: 2, x: 0, y: inner[0].y, w: inner[0].x, h: inner[0].h }
  // Centred: x = left + (room - width) / 2, truncated, which is how the
  // firmware centres a line in a box of `room`. Every room that reproduces the
  // samples puts short text in the same place; the widest one that stays on
  // screen is the one long text wraps least in.
  let fit = null
  for (let room = 0; room <= 2 * screen.width; ++room) {
    const left = inner[0].x - Math.trunc(room / 2)
    if (!inner.every((box) => box.x === left + Math.trunc((room - box.width) / 2))) continue
    if (left >= 0 && left + room <= screen.width) fit = { align: 1, x: left, y: inner[0].y, w: room, h: inner[0].h }
    else fit ??= { align: 1, x: left, y: inner[0].y, w: room, h: inner[0].h }
  }
  if (fit) return fit
  throw new PlanError(`run-time text ${target.dynamic.expr} is not left, right or centre aligned`)
}

/**
 * `{ draws, controls, contentBottom }` for a component tree mounted on a
 * screen, or a PlanError / LayoutError saying what needs the engine.
 *
 * `draws` are, in the engine's draw order, `fill`, `stroke`, `focus` (the
 * ring around focused control N) and `text` (static `content` or a run-time
 * `expr`). `controls` are the click targets in the engine's focus order.
 */
export const planPage = (content, rules, screen) => {
  const root = page(content)
  const nodes = []
  walk(root, (node, parent) => {
    nodes.push(node)
    node.style = computeStyle(node, parent?.style ?? null, rules)
  })
  // The engine delivers a click to the focused control and then to every
  // ancestor listening; a watch app with nested controls needs that bubbling.
  walk(root, (node) => {
    if (!node.click) return
    walk(node, (inner) => {
      if (inner !== node && inner.click) throw new PlanError('a control inside a control')
    })
  })
  const dynamic = nodes.filter((node) => node.dynamic)
  const fits = new Map()
  try {
    for (const node of dynamic) fits.set(node, fitDynamicText(root, screen, nodes, node))
    layout(root, screen, () => baseWidth)
  } catch (error) {
    if (error instanceof LayoutError) throw new PlanError(error.message)
    throw error
  }

  const draws = []
  const controls = []
  let contentBottom = 0
  const draw = (node) => {
    if (node.style.display === 1) return
    const s = node.style
    const { x, y, w, h } = node.box
    contentBottom = Math.max(contentBottom, y + h)
    if (s.hasBackground) draws.push({ op: 'fill', x, y, w, h, radius: s.radius, color: s.background })
    if (s.borderWidth > 0) draws.push({ op: 'stroke', x, y, w, h, radius: s.radius, width: s.borderWidth, color: s.borderColor })
    if (node.click) {
      // The engine's ring sits 2 px outside the control.
      draws.push({ op: 'focus', index: controls.length, x: x - 2, y: y - 2, w: w + 4, h: h + 4, radius: s.radius + 2 })
      controls.push({ x, y, w, h, expr: node.click.expr })
    }
    if (node.text) {
      const font = fontFor(s.fontSize, s.fontWeight)
      const inset = textTopInset(s.fontSize)
      if (node.dynamic) {
        const fit = fits.get(node)
        draws.push({ op: 'text', expr: node.dynamic.expr, font, ...fit, color: s.color, inset })
      } else if (node.content) {
        draws.push({ op: 'text', content: node.content, font, ...node.inner, align: s.textAlign, color: s.color, inset })
      }
    }
    for (const child of node.children) draw(child)
  }
  draw(root)
  return { draws, controls, contentBottom }
}
