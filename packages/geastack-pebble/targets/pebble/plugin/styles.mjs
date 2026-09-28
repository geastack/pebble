// SPDX-License-Identifier: Apache-2.0
// The app's stylesheet as rules, read from the registration calls the gea
// build writes beside its style prelude (`gea-style-registration.calls.json`).
//
// These are the calls the Pebble engine (runtime/pebble_ui.cpp) would receive
// at startup, so interpreting them here the way its `StyleSheet` does gives
// the same cascade, only at build time. Values keep the engine's fixed-point
// representation (sixteenths of a pixel) and its truncating integer math, so
// every resolved length matches what the engine would compute on the watch.

export const kFixedOne = 16

// The engine's length units (pebble_ui.cpp `LengthUnit`).
export const Unit = { Unset: 0, Px: 1, Percent: 2, Vw: 3, Vh: 4, Auto: 5 }

const unitFrom = (name) => {
  switch (name) {
    case 'Raw':
    case 'Px':
      return Unit.Px
    case 'Percent':
      return Unit.Percent
    case 'Vw':
    case 'Dvw':
    case 'Vmin':
      return Unit.Vw
    case 'Vh':
    case 'Dvh':
    case 'Vmax':
      return Unit.Vh
    case 'Auto':
      return Unit.Auto
    default:
      return Unit.Unset
  }
}

// `fixedFromFloat`: the value in sixteenths, truncated toward zero, from a
// float32 -- the engine receives the literal as a C++ float.
export const fixedFromFloat = (value) => Math.trunc(Math.fround(value) * kFixedOne)

// GColor8: two bits each of alpha, red, green, blue.
export const pebbleColor = (r, g, b) => 0xc0 | ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6)

const splitArgs = (text) => {
  const args = []
  let depth = 0
  let quote = null
  let start = 0
  for (let i = 0; i < text.length; ++i) {
    const c = text[i]
    if (quote) {
      if (c === '\\') ++i
      else if (c === quote) quote = null
      continue
    }
    if (c === '"' || c === "'") quote = c
    else if (c === '(' || c === '{' || c === '[' || c === '<') ++depth
    else if (c === ')' || c === '}' || c === ']' || c === '>') --depth
    else if (c === ',' && depth === 0) {
      args.push(text.slice(start, i).trim())
      start = i + 1
    }
  }
  if (text.slice(start).trim()) args.push(text.slice(start).trim())
  return args
}

const enumName = (arg) => arg.slice(arg.lastIndexOf('::') + 2)
const stringArg = (arg) => (arg === 'nullptr' ? '' : JSON.parse(arg))
const numberArg = (arg) => Number(arg.replace(/f$/, ''))
const lengthSpec = (arg) => {
  const [unit, value] = splitArgs(arg.trim().slice(1, -1))
  return { unit: enumName(unit), value: numberArg(value) }
}

/**
 * The rules, in registration order, each `{ selectorKind, selector, apply }`
 * where `apply(style)` does what the engine's `applyRule` does. A call this
 * target has no meaning for (fonts, shadows, custom properties, line height)
 * is dropped exactly as the engine drops it; a call it does not recognise at
 * all is reported, because then the cascade would silently differ.
 */
export const parseRules = (calls) => {
  const rules = []
  const unknown = []
  for (const call of calls) {
    const match = call.match(/^__gea_stylesheet\.(\w+)\(([\s\S]*)\);$/)
    if (!match) {
      unknown.push(call)
      continue
    }
    const [, method, body] = match
    const args = splitArgs(body)
    const selectorKind = enumName(args[0])
    // A call not shaped (kind, "selector", ...) is one this target does not
    // know, like a length expression or a media plan: reported, not guessed.
    if (!/^"/.test(args[1] ?? '') && args[1] !== 'nullptr') {
      unknown.push(call)
      continue
    }
    const selector = stringArg(args[1])
    const add = (media, apply) => {
      // Media queries are for other screens; the watch takes the base rules.
      if (media && stringArg(media)) return
      if (selectorKind !== 'Class' && selectorKind !== 'Element') return
      rules.push({ selectorKind, selector, apply })
    }
    const lengthRule = (property, unitName, value, media) => {
      const unit = unitFrom(unitName)
      if (unit === Unit.Unset) return
      const fixed = fixedFromFloat(value)
      add(media, (style) => setLength(style, property, { value: fixed, unit }))
    }
    const colorRule = (property, r, g, b, a, media) => {
      const color = pebbleColor(r, g, b)
      const transparent = a < 128
      add(media, (style) => applyColor(style, property, color, transparent))
    }
    switch (method) {
      case 'registerStaticPropertyRule': {
        const property = enumName(args[2])
        const value = numberArg(args[3])
        add(args[4], (style) => setProperty(style, property, value))
        break
      }
      case 'registerStaticPropertyGroupRule':
        unknown.push(call)
        break
      case 'registerStaticLengthRule':
        lengthRule(enumName(args[2]), enumName(args[3]), numberArg(args[4]), args[5])
        break
      case 'registerStaticLengthSpecRule': {
        const spec = lengthSpec(args[3])
        lengthRule(enumName(args[2]), spec.unit, spec.value, args[4])
        break
      }
      case 'registerStaticColorRule':
        colorRule(enumName(args[2]), ...args.slice(3, 7).map(numberArg), args[7])
        break
      case 'registerStaticColorVarRule':
        // The declared fallback is what the rule means when the variable is unset.
        if (args[4] === 'true') colorRule(enumName(args[2]), ...args.slice(5, 9).map(numberArg), args[9])
        break
      case 'registerStaticBorderRule': {
        const spec = lengthSpec(args[2])
        lengthRule('BorderWidth', spec.unit, spec.value, args[7])
        colorRule('Border', ...args.slice(3, 7).map(numberArg), args[7])
        break
      }
      case 'registerStaticBorderRadiusRule': {
        const spec = lengthSpec(args[2])
        const value = spec.unit === 'Px' || spec.unit === 'Raw' ? fixedFromFloat(spec.value) : 0
        add(args[6], (style) => (style.radius = Math.trunc(value / kFixedOne)))
        break
      }
      case 'registerStaticBorderRadiusCornerRule': {
        if (enumName(args[2]) !== 'TopLeft') break
        const spec = lengthSpec(args[3])
        const value = spec.unit === 'Px' || spec.unit === 'Raw' ? fixedFromFloat(spec.value) : 0
        add(args[4], (style) => (style.radius = Math.trunc(value / kFixedOne)))
        break
      }
      case 'registerStaticFlexRule': {
        const grow = numberArg(args[2])
        add(args[5], (style) => setProperty(style, 'Flex', grow))
        break
      }
      case 'registerStaticLineHeightRule':
      case 'registerStaticFontFamilyRule':
      case 'registerStaticBoxShadowNoneRule':
      case 'registerStaticFilterBlurRule':
      case 'registerStaticCustomLengthRule':
      case 'registerStaticCustomColorRule':
      case 'registerStaticRule':
      case 'registerStaticElementRule':
      case 'registerStaticSelectorRule':
      case 'registerStaticKeyframes':
        break
      default:
        unknown.push(call)
    }
  }
  return { rules, unknown }
}

// gea's user-agent sheet (compiler src/plugins/gea/user-agent-styles.ts),
// registered ahead of the app's rules. Its flex-wrap rows mean nothing here.
export const userAgentRules = () =>
  [
    ['h1', 34],
    ['h2', 28],
    ['h3', 24],
    ['h4', 20],
    ['h5', 18],
    ['h6', 16]
  ].map(([tag, size]) => ({
    selectorKind: 'Element',
    selector: tag,
    apply: (style) => (style.fontSize = size)
  }))

// ---- The engine's ComputedStyle and its setters, verbatim in behaviour ----

const unset = () => ({ value: 0, unit: Unit.Unset })

export const defaultStyle = () => ({
  display: 0, // 0 block, 1 none, 2 grid, 3 flex
  row: false,
  justify: 6, // kAlignStart
  alignItems: 0, // kAlignStretch
  alignSelf: -1, // kAlignAutoSelf
  textAlign: 0,
  absolute: false,
  grow: 0,
  width: unset(),
  height: unset(),
  minWidth: unset(),
  minHeight: unset(),
  maxWidth: unset(),
  maxHeight: unset(),
  padding: [unset(), unset(), unset(), unset()],
  margin: [unset(), unset(), unset(), unset()],
  inset: [unset(), unset(), unset(), unset()],
  gap: unset(),
  borderWidth: 0,
  radius: 0,
  fontSize: 14,
  fontWeight: 400,
  background: 0,
  color: 0xff,
  borderColor: 0xc0,
  hasBackground: false
})

const lengthSlots = {
  Gap: (s, l) => (s.gap = l),
  Width: (s, l) => (s.width = l),
  Height: (s, l) => (s.height = l),
  MinWidth: (s, l) => (s.minWidth = l),
  MinHeight: (s, l) => (s.minHeight = l),
  MaxWidth: (s, l) => (s.maxWidth = l),
  MaxHeight: (s, l) => (s.maxHeight = l),
  PaddingTop: (s, l) => (s.padding[0] = l),
  PaddingRight: (s, l) => (s.padding[1] = l),
  PaddingBottom: (s, l) => (s.padding[2] = l),
  PaddingLeft: (s, l) => (s.padding[3] = l),
  MarginTop: (s, l) => (s.margin[0] = l),
  MarginRight: (s, l) => (s.margin[1] = l),
  MarginBottom: (s, l) => (s.margin[2] = l),
  MarginLeft: (s, l) => (s.margin[3] = l),
  Top: (s, l) => (s.inset[0] = l),
  Right: (s, l) => (s.inset[1] = l),
  Bottom: (s, l) => (s.inset[2] = l),
  Left: (s, l) => (s.inset[3] = l)
}

function setLength(style, property, length) {
  if (property === 'BorderWidth' || property === 'BorderTopWidth') style.borderWidth = Math.trunc(length.value / kFixedOne)
  else if (property === 'FontSize') style.fontSize = Math.trunc(length.value / kFixedOne)
  else lengthSlots[property]?.(style, { ...length })
}

function setProperty(style, property, value) {
  const px = () => ({ value: value * kFixedOne, unit: Unit.Px })
  switch (property) {
    case 'Display':
      style.display = value
      break
    case 'FlexDirection':
      style.row = value === 1
      break
    case 'JustifyContent':
      style.justify = value
      break
    case 'AlignItems':
      style.alignItems = value
      break
    case 'AlignSelf':
      style.alignSelf = value
      break
    case 'TextAlign':
      style.textAlign = value
      break
    case 'Position':
      style.absolute = value === 1
      break
    case 'Flex':
      style.grow = value
      break
    case 'FontWeight':
      style.fontWeight = value
      break
    case 'WidthPercent':
      style.width = { value: Math.trunc((value * kFixedOne) / 10), unit: Unit.Percent }
      break
    case 'HeightPercent':
      style.height = { value: Math.trunc((value * kFixedOne) / 10), unit: Unit.Percent }
      break
    case 'BorderWidth':
      style.borderWidth = value
      break
    default:
      lengthSlots[property]?.(style, px())
  }
}

function applyColor(style, property, color, transparent) {
  switch (property) {
    case 'Color':
      style.color = color
      break
    case 'Background':
    case 'BackgroundColor':
      style.background = color
      style.hasBackground = !transparent
      break
    case 'Border':
    case 'BorderTop':
      style.borderColor = color
      break
  }
}

/** `computeStyles` for one node: inherited values, then element rules, then class rules. */
export const computeStyle = (node, parent, rules) => {
  const style = defaultStyle()
  if (parent) {
    style.color = parent.color
    style.fontSize = parent.fontSize
    style.fontWeight = parent.fontWeight
    style.textAlign = parent.textAlign
  }
  for (const rule of rules) if (rule.selectorKind === 'Element' && rule.selector === node.tag) rule.apply(style)
  for (const rule of rules) if (rule.selectorKind === 'Class' && node.classes.includes(rule.selector)) rule.apply(style)
  return style
}
