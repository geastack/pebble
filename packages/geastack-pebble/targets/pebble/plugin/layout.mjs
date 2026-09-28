// SPDX-License-Identifier: Apache-2.0
// runtime/pebble_ui.cpp's layout, run at build time.
//
// `intrinsic`, `layoutChildren` and `place` are ported line for line, with the
// engine's truncating integer division, so a node lands on exactly the pixel
// the engine would have put it on. The one input the build cannot know is the
// width of text a program computes at run time; `layout` asks `dynamicWidth(node)` for it,
// and the caller lays the page out several times to see
// what that text moves.
import { Unit, kFixedOne } from './styles.mjs'
import { fontFor, lineHeight, measureText } from './text.mjs'

const Align = { Stretch: 0, Center: 1, End: 2, Between: 3, Around: 4, Baseline: 5, Start: 6 }

const div = (a, b) => Math.trunc(a / b)
const isSet = (length) => length.unit !== Unit.Unset && length.unit !== Unit.Auto

export class LayoutError extends Error {}

export const layout = (root, screen, dynamicWidth) => {
  const resolve = (length, basis) => {
    switch (length.unit) {
      case Unit.Px:
        return div(length.value, kFixedOne)
      case Unit.Percent:
        return div(length.value * basis, 100 * kFixedOne)
      case Unit.Vw:
        return div(length.value * screen.width, 100 * kFixedOne)
      case Unit.Vh:
        return div(length.value * screen.height, 100 * kFixedOne)
      default:
        return 0
    }
  }
  const padH = (s, basis) => resolve(s.padding[1], basis) + resolve(s.padding[3], basis) + 2 * s.borderWidth
  const padV = (s, basis) => resolve(s.padding[0], basis) + resolve(s.padding[2], basis) + 2 * s.borderWidth
  const clampTo = (value, min, max, basis) => {
    if (isSet(max)) value = Math.min(value, resolve(max, basis))
    if (isSet(min)) value = Math.max(value, resolve(min, basis))
    return value
  }
  const inFlow = (node) => node.style.display !== 1 && !node.style.absolute
  const measure = (node, availW) => {
    const font = fontFor(node.style.fontSize, node.style.fontWeight)
    if (node.dynamic) return { w: dynamicWidth(node), h: lineHeight(font) }
    const size = measureText(font, node.content, availW)
    if (!size) throw new LayoutError(`text ${JSON.stringify(node.content)} wraps at ${availW}px`)
    return size
  }

  const intrinsic = (node, availW, availH) => {
    if (node.style.display === 1) return { w: 0, h: 0 }
    const s = node.style
    let w = 0
    let h = 0
    if (isSet(s.width)) availW = resolve(s.width, availW)
    if (node.text) {
      const size = measure(node, availW - padH(s, availW))
      w = size.w + padH(s, availW)
      h = size.h + padV(s, availH)
    } else {
      const innerW = availW - padH(s, availW)
      const gap = resolve(s.gap, innerW)
      let count = 0
      const row = s.row && s.display === 3
      for (const c of node.children) {
        if (!inFlow(c)) continue
        const cs = intrinsic(c, innerW, availH)
        const mh = resolve(c.style.margin[1], innerW) + resolve(c.style.margin[3], innerW)
        const mv = resolve(c.style.margin[0], innerW) + resolve(c.style.margin[2], innerW)
        if (row) {
          w += cs.w + mh
          if (cs.h + mv > h) h = cs.h + mv
        } else {
          h += cs.h + mv
          if (cs.w + mh > w) w = cs.w + mh
        }
        ++count
      }
      if (count > 1) {
        if (row) w += gap * (count - 1)
        else h += gap * (count - 1)
      }
      w += padH(s, availW)
      h += padV(s, availH)
    }
    if (isSet(s.width)) w = resolve(s.width, availW)
    if (isSet(s.height)) h = resolve(s.height, availH)
    return { w: clampTo(w, s.minWidth, s.maxWidth, availW), h: clampTo(h, s.minHeight, s.maxHeight, availH) }
  }

  const place = (node, x, y, w, h) => {
    node.box = { x, y, w, h }
    // What the draw pass reads of a text box: padding and border resolved
    // against the box's own width (and height, for the vertical sum).
    const s = node.style
    if (node.text)
      node.inner = {
        x: x + resolve(s.padding[3], w) + s.borderWidth,
        y: y + resolve(s.padding[0], w) + s.borderWidth,
        w: w - padH(s, w),
        h: h - padV(s, h)
      }
    layoutChildren(node)
  }

  const layoutChildren = (node) => {
    if (node.text) return
    const s = node.style
    const { x, y, w, h } = node.box
    const left = x + resolve(s.padding[3], w) + s.borderWidth
    const top = y + resolve(s.padding[0], w) + s.borderWidth
    const innerW = w - padH(s, w)
    const innerH = h - padV(s, h)
    const row = s.row && s.display === 3
    const mainSize = row ? innerW : innerH
    const gap = resolve(s.gap, innerW)
    const items = []
    let used = 0
    let grow = 0
    for (const c of node.children) {
      if (c.style.display === 1) {
        c.box = { x: 0, y: 0, w: 0, h: 0 }
        continue
      }
      const cs = c.style
      if (cs.absolute) {
        const size = intrinsic(c, innerW, innerH)
        let cx = left
        let cy = top
        if (isSet(cs.inset[3])) cx = left + resolve(cs.inset[3], innerW)
        else if (isSet(cs.inset[1])) cx = left + innerW - resolve(cs.inset[1], innerW) - size.w
        if (isSet(cs.inset[0])) cy = top + resolve(cs.inset[0], innerH)
        else if (isSet(cs.inset[2])) cy = top + innerH - resolve(cs.inset[2], innerH) - size.h
        place(c, cx, cy, size.w, size.h)
        continue
      }
      const size = intrinsic(c, innerW, innerH)
      const mTop = resolve(cs.margin[0], innerW)
      const mRight = resolve(cs.margin[1], innerW)
      const mBottom = resolve(cs.margin[2], innerW)
      const mLeft = resolve(cs.margin[3], innerW)
      const item = {
        node: c,
        main: row ? size.w : size.h,
        cross: row ? size.h : size.w,
        marginBefore: row ? mLeft : mTop,
        marginAfter: row ? mRight : mBottom,
        crossBefore: row ? mTop : mLeft,
        crossAfter: row ? mBottom : mRight,
        grow: s.display === 3 ? cs.grow : 0
      }
      used += item.main + item.marginBefore + item.marginAfter
      grow += item.grow
      items.push(item)
    }
    if (items.length > 1) used += gap * (items.length - 1)
    let free = mainSize - used
    if (free > 0 && grow > 0) {
      for (const item of items) item.main += div(free * item.grow, grow)
      free = 0
    }
    const justify = s.display === 3 ? s.justify : Align.Start
    let cursor = 0
    let between = gap
    if (free > 0) {
      switch (justify) {
        case Align.Center:
          cursor = div(free, 2)
          break
        case Align.End:
          cursor = free
          break
        case Align.Between:
          if (items.length > 1) between += div(free, items.length - 1)
          break
        case Align.Around:
          if (items.length) {
            const share = div(free, items.length)
            cursor = div(share, 2)
            between += share
          }
          break
      }
    }
    const crossSize = row ? innerH : innerW
    for (const item of items) {
      const cs = item.node.style
      let align = cs.alignSelf >= 0 ? cs.alignSelf : s.alignItems
      if (s.display !== 3) align = Align.Stretch
      const crossFixed = row ? isSet(cs.height) : isSet(cs.width)
      let cross = item.cross
      const crossRoom = crossSize - item.crossBefore - item.crossAfter
      if (align === Align.Stretch && !crossFixed) cross = crossRoom
      let crossOffset = item.crossBefore
      if (align === Align.Center) crossOffset += div(crossRoom - cross, 2)
      else if (align === Align.End) crossOffset += crossRoom - cross
      cursor += item.marginBefore
      if (row) place(item.node, left + cursor, top + crossOffset, item.main, cross)
      else place(item.node, left + crossOffset, top + cursor, cross, item.main)
      cursor += item.main + item.marginAfter + between
    }
  }

  place(root, 0, 0, screen.width, screen.height)
}
