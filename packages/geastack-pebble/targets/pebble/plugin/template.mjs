// SPDX-License-Identifier: Apache-2.0
// A component's template as a node tree, from the gea IR the vite plugin
// writes (`gea-ir.json`): the static markup of the template and the slots the
// program fills at run time, each addressed by a walk from the template root.
//
// The tree has the engine's shape: an element per tag, and a text node for
// each run of text, which is where the engine keeps text (`createText`,
// `setText` on an element replaces its children with one text node).

export class TemplateError extends Error {}

const voidTags = new Set(['area', 'br', 'col', 'embed', 'hr', 'img', 'input', 'link', 'meta', 'source', 'track', 'wbr'])
const entities = { amp: '&', lt: '<', gt: '>', quot: '"', apos: "'", nbsp: ' ' }
const decode = (text) =>
  text.replace(/&(#x[0-9a-f]+|#\d+|\w+);/gi, (whole, name) => {
    if (name[0] === '#') return String.fromCodePoint(name[1] === 'x' || name[1] === 'X' ? parseInt(name.slice(2), 16) : Number(name.slice(1)))
    return entities[name] ?? whole
  })

const element = (tag, classes) => ({ tag, classes, text: false, children: [], content: '', dynamic: null, click: null })
export const textNode = (content) => ({ tag: '', classes: [], text: true, children: [], content, dynamic: null, click: null })

/** Parses the IR's minified markup (unquoted attributes, end tags the HTML serializer may omit). */
export const parseMarkup = (html) => {
  const root = element('#template', [])
  const stack = [root]
  let at = 0
  while (at < html.length) {
    if (html.startsWith('</', at)) {
      const end = html.indexOf('>', at)
      const tag = html.slice(at + 2, end).trim().toLowerCase()
      while (stack.length > 1) {
        const open = stack.pop()
        if (open.tag === tag) break
      }
      at = end + 1
      continue
    }
    if (html[at] === '<') {
      const match = /^<([a-zA-Z][\w-]*)((?:\s+[^\s=>]+(?:=(?:"[^"]*"|'[^']*'|[^\s>]*))?)*)\s*(\/?)>/.exec(html.slice(at))
      if (!match) throw new TemplateError(`markup at ${at}: ${html.slice(at, at + 30)}`)
      const tag = match[1].toLowerCase()
      const attributes = {}
      for (const attribute of match[2].matchAll(/([^\s=>]+)(?:=("([^"]*)"|'([^']*)'|([^\s>]*)))?/g)) {
        attributes[attribute[1].toLowerCase()] = decode(attribute[3] ?? attribute[4] ?? attribute[5] ?? '')
      }
      for (const name of Object.keys(attributes)) {
        if (name !== 'class' && name !== 'id') throw new TemplateError(`attribute "${name}" on <${tag}>`)
      }
      const node = element(tag, (attributes.class ?? '').split(/\s+/).filter(Boolean))
      stack.at(-1).children.push(node)
      if (!match[3] && !voidTags.has(tag)) stack.push(node)
      at += match[0].length
      continue
    }
    const next = html.indexOf('<', at)
    const text = decode(html.slice(at, next < 0 ? html.length : next))
    if (text.length) stack.at(-1).children.push(textNode(text))
    at = next < 0 ? html.length : next
  }
  if (root.children.length !== 1 || root.children[0].text) throw new TemplateError('a template must have one root element')
  return root.children[0]
}

const walkTo = (root, slot) => {
  let node = root
  for (const step of slot.walkKinds ?? []) {
    if ('elem' in step) node = node.children.filter((child) => !child.text)[step.elem]
    else if ('child' in step) node = node.children[step.child]
    else throw new TemplateError(`slot ${slot.index}: walk step ${JSON.stringify(step)}`)
    if (!node) throw new TemplateError(`slot ${slot.index}: walk leaves the template`)
  }
  return node
}

// Handlers the watch can deliver: a button press or a tap on the control.
const clickEvents = new Set(['onClick', 'onclick', 'onPress', 'onTap'])

/**
 * The template tree with each slot attached to its node, or a TemplateError
 * naming the first thing the compiled UI does not express (a list, a
 * condition, a child component, a class or style binding): for those the
 * program keeps the engine.
 */
export const templateTree = (component) => {
  const template = component.template
  if (!template?.html) throw new TemplateError(`${component.exportName}: no template`)
  const root = parseMarkup(template.html)
  for (const slot of template.slots ?? []) {
    const node = walkTo(root, slot)
    if (slot.kind === 'text') {
      // The walk ends on the text node the slot replaces; a direct-text slot
      // may land on its element when the placeholder was elided.
      const target = node.text ? node : node.children.length === 1 && node.children[0].text ? node.children[0] : null
      if (!target) throw new TemplateError(`slot ${slot.index}: text slot on an element with mixed children`)
      target.dynamic = { expr: slot.expr }
      continue
    }
    if (slot.kind === 'event') {
      const name = slot.payload?.attrName
      if (!clickEvents.has(name)) throw new TemplateError(`slot ${slot.index}: ${name} handler`)
      if (node.text) throw new TemplateError(`slot ${slot.index}: handler on text`)
      node.click = { expr: slot.expr }
      continue
    }
    throw new TemplateError(`slot ${slot.index}: ${slot.kind} slot`)
  }
  return root
}
