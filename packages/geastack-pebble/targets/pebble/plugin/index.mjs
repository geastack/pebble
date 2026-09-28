// SPDX-License-Identifier: Apache-2.0
// The Pebble compiled UI, as a geatsc plugin.
//
// On a watch there is no DOM for a compiled template to drive, so the engine
// (runtime/pebble_ui.cpp) ships one inside every app: a node tree, the
// stylesheet matched against class strings, flexbox layout, all at run time.
// Everything that engine works out is already known when the app is built --
// the template's shape, its classes, the whole stylesheet, the screen -- so
// this plugin works it out then, and the program keeps only the drawing:
//
//   - the mounted component's template, cascade and layout are resolved here
//     (`plan.mjs`), with the firmware's own font metrics;
//   - the component gains a draw method made of constant host calls (fill,
//     stroke, text) and a mount method that registers its click targets;
//   - `mount(App)` becomes `new App().__geaPebbleMount()`.
//
// Reactivity is the compiler's, unchanged: a reactive field is still a
// `Signal`, and on this target a Signal write asks for a redraw
// (runtime/ui/signal.h), after which the draw method reads the new value.
//
// A program this cannot express -- a list, a condition, a child component, a
// binding the plan refuses -- is left untouched and keeps the engine. The
// decision is written to `pebble-ui.json` in the output directory, which the
// build reads to pick the runtime it links.
import { existsSync, readFileSync, writeFileSync } from 'node:fs'
import { createRequire } from 'node:module'
import path from 'node:path'
import { pathToFileURL } from 'node:url'
import { parseRules, userAgentRules } from './styles.mjs'
import { templateTree, TemplateError } from './template.mjs'
import { planPage, PlanError } from './plan.mjs'

const hostFunctions = {
  __geaPebbleOps: 'gea_pebble_ops',
  __geaPebbleTextAt: 'gea_pebble_text_at',
  __geaPebbleNumberAt: 'gea_pebble_number_at',
  __geaPebbleOn: 'gea_pebble_on',
  __geaPebbleScreen: 'gea_pebble_screen'
}

const hostDeclarations = `
declare function __geaPebbleOps(begin: number, end: number): void
declare function __geaPebbleTextAt(op: number, text: string): void
declare function __geaPebbleNumberAt(op: number, value: number): void
declare function __geaPebbleOn(control: number, onClick: () => void): void
declare function __geaPebbleScreen(draw: () => void): void
`

// TypeScript, from the compiler running this plugin: the same parser the
// program was checked with.
const typescript = () => {
  for (const base of [process.argv[1], import.meta.url]) {
    try {
      return createRequire(base.startsWith('file:') ? base : pathToFileURL(base))('typescript')
    } catch {}
  }
  throw new Error('pebble compiled UI: cannot load typescript')
}

const screenOf = (options) => ({
  width: Number(options.get('pebble.width') ?? 200),
  height: Number(options.get('pebble.height') ?? 228)
})

const readJson = (file) => JSON.parse(readFileSync(file, 'utf8'))

const handlerText = (expr) => {
  const trimmed = expr.trim()
  if (!/^\(\s*\)\s*=>/.test(trimmed)) throw new PlanError(`handler ${trimmed} takes the event`)
  return trimmed
}

const byte = (value, what) => {
  if (!Number.isInteger(value) || value < 0 || value > 255) throw new PlanError(`${what} ${value} does not fit the op table`)
  return value
}
const short = (value) => {
  if (!Number.isInteger(value) || value < -32768 || value > 32767) throw new PlanError(`coordinate ${value} does not fit the op table`)
  return value
}
const cppString = (text) => `"${[...new TextEncoder().encode(text)].map((b) => (b >= 32 && b < 127 && b !== 34 && b !== 92 && b !== 63 ? String.fromCharCode(b) : `\\${b.toString(8).padStart(3, '0')}`)).join('')}"`

// A run-time text slot: a number field formats as a number on the watch, a
// string field is drawn as it is, anything else is converted the way JS would.
const slotCall = (index, expr, fields) => {
  const field = /^this\.([A-Za-z_$][\w$]*)$/.exec(expr.trim())?.[1]
  const shape = field && fields.get(field)
  if (shape === 'number') return `__geaPebbleNumberAt(${index}, ${expr})`
  if (shape === 'string') return `__geaPebbleTextAt(${index}, ${expr})`
  return `__geaPebbleTextAt(${index}, '' + (${expr}))`
}

/**
 * The page as the program carries it: the op and control tables (C++, into
 * the unit through the host preamble), and the component's draw and mount
 * methods (TypeScript, into its class).
 */
const emitPage = (plan, component) => {
  const fields = new Map((component.reactiveState?.fields ?? []).map((field) => [field.name, field.shape?.valueType]))
  const rows = plan.draws.map((draw) => {
    const at = `${short(draw.x)}, ${short(draw.y)}, ${short(draw.w)}, ${short(draw.h)}`
    switch (draw.op) {
      case 'fill':
        return `{${at}, kFill, ${byte(draw.radius, 'radius')}, ${byte(draw.color, 'color')}, 0, 0, nullptr}`
      case 'stroke':
        return `{${at}, kStroke, ${byte(draw.radius, 'radius')}, ${byte(draw.width, 'border')}, ${byte(draw.color, 'color')}, 0, nullptr}`
      case 'focus':
        return `{${at}, kFocus, ${byte(draw.index, 'control')}, ${byte(draw.radius, 'radius')}, 0, 0, nullptr}`
      case 'text':
        return `{${at}, kText, ${byte(draw.font, 'font')}, ${byte(draw.align, 'align')}, ${byte(draw.color, 'color')}, ${byte(draw.inset, 'inset')}, ${draw.expr === undefined ? cppString(draw.content) : 'nullptr'}}`
    }
  })
  const controls = plan.controls.map((c) => `{${short(c.x)}, ${short(c.y)}, ${short(c.w)}, ${short(c.h)}}`)
  const count = plan.controls.length
  const cpp = [
    '#include "pebble_compiled_ui.h"',
    '#ifndef GEA_PEBBLE_COMPILED_PAGE',
    '#define GEA_PEBBLE_COMPILED_PAGE',
    '// The page, laid out when the app was built (the Pebble compiled UI).',
    'namespace gea::pebble::compiled::page {',
    `inline constexpr Op ops[] = {\n  ${rows.join(',\n  ')}\n};`,
    count ? `inline constexpr Rect controls[] = {${controls.join(', ')}};` : 'inline constexpr const Rect *controls = nullptr;',
    count ? `inline Callback handlers[${count}];` : 'inline Callback *const handlers = nullptr;',
    '}  // namespace gea::pebble::compiled::page',
    'inline void gea_pebble_ops(int begin, int end) { gea::pebble::compiled::drawOps(gea::pebble::compiled::page::ops + begin, end - begin); }',
    'template <typename Text> void gea_pebble_text_at(int op, const Text &text) { gea::pebble::compiled::textAt(gea::pebble::compiled::page::ops[op], text); }',
    'template <typename Number> void gea_pebble_number_at(int op, const Number &value) { gea::pebble::compiled::numberAt(gea::pebble::compiled::page::ops[op], value); }',
    'template <typename F> void gea_pebble_on(int control, F &&onClick) { gea::pebble::compiled::page::handlers[control] = gea::pebble::compiled::callback(std::forward<F>(onClick)); }',
    `template <typename F> void gea_pebble_screen(F &&draw) { gea::pebble::compiled::registerScreen(gea::pebble::compiled::page::controls, gea::pebble::compiled::page::handlers, ${count}, ${plan.contentBottom}, gea::pebble::compiled::callback(std::forward<F>(draw))); }`,
    '#endif'
  ].join('\n')

  const lines = ['  __geaPebbleDraw(): void {']
  let run = null
  const flush = (end) => {
    if (run !== null && end > run) lines.push(`    __geaPebbleOps(${run}, ${end})`)
    run = null
  }
  plan.draws.forEach((draw, index) => {
    if (draw.op === 'text' && draw.expr !== undefined) {
      flush(index)
      lines.push(`    ${slotCall(index, draw.expr, fields)}`)
    } else run ??= index
  })
  flush(plan.draws.length)
  lines.push('  }', '  __geaPebbleMount(): void {')
  plan.controls.forEach((control, index) => lines.push(`    __geaPebbleOn(${index}, ${handlerText(control.expr)})`))
  lines.push('    __geaPebbleScreen(() => this.__geaPebbleDraw())', '  }')
  return { cpp, methods: lines.join('\n') }
}

const sameFile = (a, b) => path.resolve(a) === path.resolve(b)

/**
 * The whole decision, made once per compilation from the build's own outputs:
 * the gea IR, the style registration calls, and the module graph's entry.
 */
const planProgram = (options) => {
  const irPath = options.get('gea.ir')
  const prelude = options.get('gea.cpp-prelude')
  if (!irPath || !prelude) return { mode: 'engine', reason: 'not a gea pipeline build' }
  const outDir = path.dirname(prelude)
  const callsPath = path.join(outDir, 'gea-style-registration.calls.json')
  const graphPath = path.join(outDir, 'module-graph', 'gea-module-graph.json')
  for (const file of [irPath, callsPath, graphPath]) if (!existsSync(file)) return { mode: 'engine', reason: `missing ${file}` }
  const ir = readJson(irPath)
  const components = ir.components ?? []
  if (components.length !== 1) return { mode: 'engine', reason: `${components.length} components (only a single root component compiles)` }
  const [component] = components
  const { rules, unknown } = parseRules(readJson(callsPath))
  if (unknown.length) return { mode: 'engine', reason: `unrecognised style call: ${unknown[0]}` }

  const graph = readJson(graphPath)
  const entry = graph.modules.find((module) => module.isEntry)
  if (!entry) return { mode: 'engine', reason: 'module graph has no entry' }
  const ts = typescript()
  const entryText = readFileSync(path.resolve(path.dirname(graphPath), entry.originalSource), 'utf8')
  const entryFile = ts.createSourceFile(entry.file, entryText, ts.ScriptTarget.Latest, true)
  const mounts = entryFile.statements.filter(
    (statement) =>
      ts.isExpressionStatement(statement) &&
      ts.isCallExpression(statement.expression) &&
      ts.isIdentifier(statement.expression.expression) &&
      statement.expression.expression.text === 'mount'
  )
  if (mounts.length !== 1) return { mode: 'engine', reason: `${mounts.length} mount() calls in the entry` }
  const [mountArg, ...rest] = mounts[0].expression.arguments
  if (rest.length || !mountArg || !ts.isIdentifier(mountArg)) return { mode: 'engine', reason: 'mount() is not mount(Component)' }

  try {
    const tree = templateTree(component)
    const plan = planPage(tree, [...userAgentRules(), ...rules], screenOf(options))
    const { cpp, methods } = emitPage(plan, component)
    return {
      mode: 'compiled',
      component,
      methods,
      cpp,
      entry: { file: entry.file, statement: mounts[0], text: entryText, component: mountArg.text },
      stats: { draws: plan.draws.length, controls: plan.controls.length, contentBottom: plan.contentBottom }
    }
  } catch (error) {
    if (error instanceof TemplateError || error instanceof PlanError) return { mode: 'engine', reason: error.message }
    throw error
  }
}

const rewriteComponent = (ts, input, plan) => {
  const source = ts.createSourceFile(input.fileName, input.text, ts.ScriptTarget.Latest, true)
  const edits = []
  let found = false
  for (const statement of source.statements) {
    // The vite transform's DOM template for the component: the engine's
    // construction path, which the compiled draw replaces.
    const names = ts.isFunctionDeclaration(statement)
      ? [statement.name?.text]
      : ts.isVariableStatement(statement)
        ? statement.declarationList.declarations.map((declaration) => declaration.name.getText(source))
        : []
    if (names.length && names.every((name) => /^_tpl\d+_(root|create)$/.test(name ?? ''))) {
      edits.push({ start: statement.getFullStart(), end: statement.end, text: '' })
      continue
    }
    if (ts.isClassDeclaration(statement) && statement.name?.text === plan.component.exportName) {
      const close = statement.end - 1
      edits.push({ start: close, end: close, text: `\n${plan.methods}\n` })
      found = true
    }
  }
  if (!found) throw new Error(`pebble compiled UI: class ${plan.component.exportName} not found in ${input.fileName}`)
  let text = input.text
  for (const edit of edits.sort((a, b) => b.start - a.start)) text = text.slice(0, edit.start) + edit.text + text.slice(edit.end)
  return `${text}\n${hostDeclarations}`
}

const rewriteEntry = (ts, input, plan) => {
  const source = ts.createSourceFile(input.fileName, input.text, ts.ScriptTarget.Latest, true)
  const mount = source.statements.find(
    (statement) =>
      ts.isExpressionStatement(statement) &&
      ts.isCallExpression(statement.expression) &&
      ts.isIdentifier(statement.expression.expression) &&
      statement.expression.expression.text === 'mount'
  )
  if (!mount) throw new Error(`pebble compiled UI: mount() not found in ${input.fileName}`)
  return `${input.text.slice(0, mount.getStart(source))}new ${plan.entry.component}().__geaPebbleMount()${input.text.slice(mount.end)}`
}

export default {
  name: 'pebble-compiled-ui',
  instantiate: (options) => {
    const plan = planProgram(options)
    const ts = plan.mode === 'compiled' ? typescript() : null
    const rewritten = new Set()
    return {
      producers: () => [],
      lower: () => false,
      transformSource:
        plan.mode === 'compiled'
          ? (input) => {
              if (sameFile(input.fileName, plan.component.module)) {
                rewritten.add('component')
                return rewriteComponent(ts, input, plan)
              }
              if (sameFile(input.fileName, plan.entry.file)) {
                rewritten.add('entry')
                return rewriteEntry(ts, input, plan)
              }
              return null
            }
          : undefined,
      writeArtifacts: (outDir) => {
        if (plan.mode === 'compiled' && rewritten.size !== 2) throw new Error('pebble compiled UI: the component or the entry was never compiled')
        const { mode, reason, stats } = plan
        writeFileSync(path.join(outDir, 'pebble-ui.json'), JSON.stringify({ mode, reason, ...stats }, null, 2) + '\n')
      },
      capabilities: {
        runtimeHelpers: new Set(),
        propertyRecipes: new Set(),
        nativeProtocols: new Set(),
        nativeTypes: new Map(),
        ambientTypeRealizations: new Map(),
        hostMembers: new Map(),
        hostMemberVoidResults: new Set(),
        hostConstructors: new Map(),
        hostFunctions: new Map(plan.mode === 'compiled' ? Object.entries(hostFunctions) : []),
        hostFunctionsByDeclaration: new Map(),
        hostNamespaces: { roots: new Set(), typeofs: new Map(), methods: new Map(), properties: new Map(), propertySetters: new Map() },
        hostNamespaceRootsByDeclaration: new Map(),
        hostNamespaceRootTypes: new Map(),
        hostSingletons: new Set(),
        hostSingletonsByDeclaration: new Map(),
        absentGlobals: new Set(),
        // One entry, not one per line: the unit keeps each distinct preamble
        // line once, and two identical rows of the op table are two ops.
        hostPreambles: new Map(plan.mode === 'compiled' ? Object.values(hostFunctions).map((spelling) => [spelling, [plan.cpp]]) : []),
        nativeBases: new Map(),
        nativeIncludes: new Map(),
        nativeConstants: new Map(),
        hostConstantsByDeclaration: new Map(),
        generatedSupportIncludes: [],
        runtimeDefinitions: [],
        elementFragment: null,
        elementTextTags: [],
        reachedMemberKeys: new Set(),
        reactiveClassFields: new Map(),
        nativeReactiveCell: null,
        nativeReactiveCellPreamble: []
      }
    }
  }
}
