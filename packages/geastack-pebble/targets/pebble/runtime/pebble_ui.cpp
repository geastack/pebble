// SPDX-License-Identifier: Apache-2.0
// The Gea UI engine surface a compiled app talks to (Document, NodeHandle,
// StyleSheet, Tree), implemented for a Pebble watch.
//
// A Pebble app gets 128 KB for its code, data and heap together, so the
// full engine (its retained display list, rasterizer and complete CSS
// cascade) cannot come along. This keeps the engine's headers -- the compiled
// program is emitted against them and links here unchanged -- and implements
// what a watch UI needs underneath: a node tree, class and element rules, a
// flexbox layout, and drawing through PebbleOS's own graphics and fonts, which
// live in firmware and cost the app nothing.
#include "gea/embedded.h"
#include "ui/tree_internal.h"

#include "pebble_host.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using gea::embedded::ui::NodeHandle;
using gea::embedded::ui::Property;
using gea::embedded::ui::StaticStyleColorProperty;
using gea::embedded::ui::StaticStyleLengthProperty;
using gea::embedded::ui::StaticStyleLengthSpec;
using gea::embedded::ui::StaticStyleLengthUnit;
using gea::embedded::ui::StaticStyleLineHeightKind;
using gea::embedded::ui::StaticStyleSelectorKind;
using gea::framework::events::EventListener;
using gea::framework::events::EventListenerId;
using gea::framework::events::PointerEvent;
using gea::framework::events::PointerEventType;

namespace gea::pebble {
namespace {

// ---- Lengths and colors ---------------------------------------------------

enum LengthUnit : std::uint8_t { kUnset, kPx, kPercent, kVw, kVh, kAuto };

// Lengths are fixed point, sixteenths of a pixel: the watch has no FPU, and
// float arithmetic here linked 2 KB of soft-float routines for what is only
// ever scaling a CSS number by a percentage.
constexpr int kFixedOne = 16;

// A declaration's float value in fixed point, truncated toward zero. Decoded
// from the IEEE bits so no float operation (and no soft-float call) is needed.
int fixedFromFloat(float value)
{
	std::uint32_t bits;
	std::memcpy(&bits, &value, sizeof(bits));
	const int exponent = static_cast<int>((bits >> 23) & 0xff) - 127;
	if (exponent < -4) return 0;
	const std::uint32_t mantissa = (bits & 0x7fffff) | 0x800000;
	const int shift = exponent + 4 - 23;
	const std::int32_t magnitude = shift >= 4 ? 0x07ffffff : static_cast<std::int32_t>(shift >= 0 ? mantissa << shift : mantissa >> -shift);
	return (bits >> 31) ? -magnitude : magnitude;
}

int pixelsOf(int fixed) { return fixed / kFixedOne; }

// Four bytes, not eight: a node keeps 25 of them in its computed style, and
// on a 64 KB heap that halving is 100 bytes a node. 28 bits of sixteenths is
// eight million pixels.
struct Length {
	std::int32_t value : 28 = 0;
	LengthUnit unit : 4 = kUnset;
	bool set() const { return unit != kUnset && unit != kAuto; }
};

// GColor8: two bits each of alpha, red, green, blue.
std::uint8_t pebbleColor(int r, int g, int b)
{
	return static_cast<std::uint8_t>(0xC0 | ((r >> 6) << 4) | ((g >> 6) << 2) | (b >> 6));
}

// ---- Style rules ----------------------------------------------------------

enum class RuleKind : std::uint8_t { Property, Length, Color, Radius, FontWeight };

// One registered declaration. Selectors and values are the compiled program's
// static strings, so a rule keeps pointers rather than copies.
struct Rule {
	const char *selector;
	StaticStyleSelectorKind selectorKind;
	RuleKind kind;
	std::uint8_t property;
	LengthUnit unit;
	int value;
	std::uint8_t color;
	bool transparent;
};

std::vector<Rule> &rules()
{
	static std::vector<Rule> list;
	return list;
}

LengthUnit unitFrom(StaticStyleLengthUnit unit)
{
	switch (unit) {
	case StaticStyleLengthUnit::Raw:
	case StaticStyleLengthUnit::Px: return kPx;
	case StaticStyleLengthUnit::Percent: return kPercent;
	case StaticStyleLengthUnit::Vw:
	case StaticStyleLengthUnit::Dvw:
	case StaticStyleLengthUnit::Vmin: return kVw;
	case StaticStyleLengthUnit::Vh:
	case StaticStyleLengthUnit::Dvh:
	case StaticStyleLengthUnit::Vmax: return kVh;
	case StaticStyleLengthUnit::Auto: return kAuto;
	default: return kUnset;
	}
}

// ---- Nodes ----------------------------------------------------------------

enum Align : std::int8_t {
	kAlignStretch = 0,
	kAlignCenter = 1,
	kAlignEnd = 2,
	kAlignBetween = 3,
	kAlignAround = 4,
	kAlignBaseline = 5,
	kAlignStart = 6,
	kAlignAutoSelf = -1
};

// What layout and drawing read, resolved from a node's rules and its parent.
struct ComputedStyle {
	std::uint8_t display = 0;  // 0 block, 1 none, 2 grid, 3 flex
	std::uint8_t row = 0;      // flex-direction: row
	std::int8_t justify = kAlignStart;
	std::int8_t alignItems = kAlignStretch;
	std::int8_t alignSelf = kAlignAutoSelf;
	std::uint8_t textAlign = GEA_PEBBLE_ALIGN_LEFT;
	std::uint8_t absolute = 0;
	std::uint8_t grow = 0;
	Length width, height, minWidth, minHeight, maxWidth, maxHeight;
	Length padding[4], margin[4], inset[4];
	Length gap;
	std::int16_t borderWidth = 0;
	std::int16_t radius = 0;
	std::int16_t fontSize = 14;
	std::int16_t fontWeight = 400;
	std::uint8_t background = 0;
	std::uint8_t color = 0xFF;  // GColorWhite
	std::uint8_t borderColor = 0xC0;
	bool hasBackground = false;
};

// One inline declaration. Its identity is the property it sets (kind and
// property), so it carries no name: `background-color` spelled as a string
// in every ball of a 64-ball scene was a heap block per ball.
using InlineRule = Rule;

bool sameProperty(const Rule &a, const Rule &b) { return a.kind == b.kind && a.property == b.property; }

struct Listener {
	EventListenerId id;
	std::uint8_t type;
	EventListener callback;
};

enum ListenerType : std::uint8_t { kListenClick, kListenTouchStart, kListenTouchEnd, kListenTouchMove, kListenKeyDown, kListenOther };

ListenerType listenerType(const char *name)
{
	if (!name) return kListenOther;
	if (!std::strcmp(name, "click") || !std::strcmp(name, "press")) return kListenClick;
	if (!std::strcmp(name, "touchstart") || !std::strcmp(name, "pointerdown")) return kListenTouchStart;
	if (!std::strcmp(name, "touchend") || !std::strcmp(name, "pointerup")) return kListenTouchEnd;
	if (!std::strcmp(name, "touchmove") || !std::strcmp(name, "pointermove")) return kListenTouchMove;
	if (!std::strcmp(name, "keydown")) return kListenKeyDown;
	return kListenOther;
}

struct PNode {
	bool alive = false;
	bool text = false;
	int parent = -1;
	std::vector<int> children;
	// Interned (see `sharedTagName`): a handful of tag names, shared by every node.
	const char *tag = "";
	std::string classes;
	std::string content;
	std::vector<Listener> listeners;
	// A node with a delegated click/press/touch handler (see
	// gea_pebble_note_listener): a control the buttons can focus.
	bool control = false;
	ComputedStyle style;
	// The element's own `style` declarations, applied after its class rules.
	std::vector<InlineRule> inlineRules;
	std::int16_t x = 0, y = 0, w = 0, h = 0;
	std::int8_t font = -1;
};

struct State {
	// Each node is its own allocation: growing one contiguous table needs the old
	// and new tables alive at once (21 KB for 64 nodes), which a Pebble heap
	// does not have, and would move every node under a held PNode pointer.
	std::vector<PNode *> nodes;
	std::vector<int> freeIds;
	int root = -1;
	int body = -1;
	int width = 200;
	int height = 228;
	// How far the page is scrolled down, so a focused control below the
	// fold can be brought on screen.
	int scrollY = 0;
	bool layoutDirty = true;
	bool styleDirty = true;
	EventListenerId nextListenerId = 1;
	int focus = -1;
	int pressed = -1;
	bool touching = false;
	// Set while a frame runs: what it invalidates, it lays out before it ends.
	bool inFrame = false;
};

State &state()
{
	static State s;
	return s;
}

PNode *nodeAt(int id)
{
	State &s = state();
	if (id < 0 || id >= static_cast<int>(s.nodes.size()) || !s.nodes[id]->alive) return nullptr;
	return s.nodes[id];
}

void invalidate(bool styles)
{
	State &s = state();
	s.layoutDirty = true;
	if (styles) s.styleDirty = true;
	if (!s.inFrame) gea_pebble_request_frame(1);
}

// The one copy of each tag name. A node's tag can outlive the string it was
// set from, and a program uses a handful of them.
const char *sharedTagName(const char *tag)
{
	if (!tag || !*tag) return "";
	static std::vector<const char *> names;
	for (const char *name : names)
		if (!std::strcmp(name, tag)) return name;
	const std::size_t length = std::strlen(tag);
	char *copy = new char[length + 1];
	std::memcpy(copy, tag, length + 1);
	names.push_back(copy);
	return copy;
}

// Element ids, off to the side: a page names one or two nodes, and a string
// in every node was 24 bytes each of 64 list rows.
std::vector<std::pair<int, std::string>> &elementIds()
{
	static std::vector<std::pair<int, std::string>> ids;
	return ids;
}

const char *elementIdOf(int node)
{
	for (const auto &entry : elementIds())
		if (entry.first == node) return entry.second.c_str();
	return "";
}

void setElementId(int node, const char *text)
{
	auto &ids = elementIds();
	for (auto entry = ids.begin(); entry != ids.end(); ++entry) {
		if (entry->first != node) continue;
		if (text && *text) entry->second = text;
		else ids.erase(entry);
		return;
	}
	if (text && *text) ids.emplace_back(node, text);
}

int allocNode(const char *tag, bool text)
{
	State &s = state();
	int id;
	if (!s.freeIds.empty()) {
		id = s.freeIds.back();
		s.freeIds.pop_back();
		*s.nodes[id] = PNode{};
		setElementId(id, nullptr);
	} else {
		id = static_cast<int>(s.nodes.size());
		s.nodes.push_back(new PNode{});
	}
	PNode &node = *s.nodes[id];
	node.alive = true;
	node.text = text;
	node.tag = sharedTagName(tag);
	return id;
}

void detach(int child)
{
	PNode *node = nodeAt(child);
	if (!node || node->parent < 0) return;
	PNode *parent = nodeAt(node->parent);
	if (parent) {
		auto &list = parent->children;
		for (std::size_t i = 0; i < list.size(); ++i) {
			if (list[i] == child) {
				list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
				break;
			}
		}
	}
	node->parent = -1;
}

void release(int id)
{
	PNode *node = nodeAt(id);
	if (!node) return;
	const std::vector<int> children = node->children;
	for (int child : children) release(child);
	State &s = state();
	if (s.focus == id) s.focus = -1;
	*s.nodes[id] = PNode{};
	s.freeIds.push_back(id);
}

bool hasClassToken(const std::string &classes, const char *token)
{
	const std::size_t length = std::strlen(token);
	if (!length) return false;
	std::size_t start = 0;
	while (start < classes.size()) {
		while (start < classes.size() && classes[start] == ' ') ++start;
		std::size_t end = start;
		while (end < classes.size() && classes[end] != ' ') ++end;
		if (end - start == length && !classes.compare(start, length, token)) return true;
		start = end;
	}
	return false;
}

// ---- Cascade --------------------------------------------------------------

void setLength(ComputedStyle &style, StaticStyleLengthProperty property, Length length)
{
	switch (property) {
	case StaticStyleLengthProperty::Gap: style.gap = length; break;
	case StaticStyleLengthProperty::Width: style.width = length; break;
	case StaticStyleLengthProperty::Height: style.height = length; break;
	case StaticStyleLengthProperty::MinWidth: style.minWidth = length; break;
	case StaticStyleLengthProperty::MinHeight: style.minHeight = length; break;
	case StaticStyleLengthProperty::MaxWidth: style.maxWidth = length; break;
	case StaticStyleLengthProperty::MaxHeight: style.maxHeight = length; break;
	case StaticStyleLengthProperty::PaddingTop: style.padding[0] = length; break;
	case StaticStyleLengthProperty::PaddingRight: style.padding[1] = length; break;
	case StaticStyleLengthProperty::PaddingBottom: style.padding[2] = length; break;
	case StaticStyleLengthProperty::PaddingLeft: style.padding[3] = length; break;
	case StaticStyleLengthProperty::MarginTop: style.margin[0] = length; break;
	case StaticStyleLengthProperty::MarginRight: style.margin[1] = length; break;
	case StaticStyleLengthProperty::MarginBottom: style.margin[2] = length; break;
	case StaticStyleLengthProperty::MarginLeft: style.margin[3] = length; break;
	case StaticStyleLengthProperty::Top: style.inset[0] = length; break;
	case StaticStyleLengthProperty::Right: style.inset[1] = length; break;
	case StaticStyleLengthProperty::Bottom: style.inset[2] = length; break;
	case StaticStyleLengthProperty::Left: style.inset[3] = length; break;
	case StaticStyleLengthProperty::BorderWidth:
	case StaticStyleLengthProperty::BorderTopWidth:
		style.borderWidth = static_cast<std::int16_t>(pixelsOf(length.value));
		break;
	case StaticStyleLengthProperty::FontSize: style.fontSize = static_cast<std::int16_t>(pixelsOf(length.value)); break;
	default: break;
	}
}

void setProperty(ComputedStyle &style, Property property, int value)
{
	const Length px{value * kFixedOne, kPx};
	switch (property) {
	case Property::Display: style.display = static_cast<std::uint8_t>(value); break;
	case Property::FlexDirection: style.row = value == 1; break;
	case Property::JustifyContent: style.justify = static_cast<std::int8_t>(value); break;
	case Property::AlignItems: style.alignItems = static_cast<std::int8_t>(value); break;
	case Property::AlignSelf: style.alignSelf = static_cast<std::int8_t>(value); break;
	case Property::TextAlign: style.textAlign = static_cast<std::uint8_t>(value); break;
	case Property::Position: style.absolute = value == 1; break;
	case Property::Flex: style.grow = static_cast<std::uint8_t>(value); break;
	case Property::FontWeight: style.fontWeight = static_cast<std::int16_t>(value); break;
	case Property::Gap: style.gap = px; break;
	case Property::Width: style.width = px; break;
	case Property::Height: style.height = px; break;
	case Property::WidthPercent: style.width = Length{value * kFixedOne / 10, kPercent}; break;
	case Property::HeightPercent: style.height = Length{value * kFixedOne / 10, kPercent}; break;
	case Property::MinWidth: style.minWidth = px; break;
	case Property::MinHeight: style.minHeight = px; break;
	case Property::MaxWidth: style.maxWidth = px; break;
	case Property::MaxHeight: style.maxHeight = px; break;
	case Property::PaddingTop: style.padding[0] = px; break;
	case Property::PaddingRight: style.padding[1] = px; break;
	case Property::PaddingBottom: style.padding[2] = px; break;
	case Property::PaddingLeft: style.padding[3] = px; break;
	case Property::MarginTop: style.margin[0] = px; break;
	case Property::MarginRight: style.margin[1] = px; break;
	case Property::MarginBottom: style.margin[2] = px; break;
	case Property::MarginLeft: style.margin[3] = px; break;
	case Property::Top: style.inset[0] = px; break;
	case Property::Right: style.inset[1] = px; break;
	case Property::Bottom: style.inset[2] = px; break;
	case Property::Left: style.inset[3] = px; break;
	case Property::BorderWidth: style.borderWidth = static_cast<std::int16_t>(value); break;
	default: break;
	}
}

void applyColor(ComputedStyle &style, StaticStyleColorProperty property, std::uint8_t color, bool transparent)
{
	switch (property) {
	case StaticStyleColorProperty::Color: style.color = color; break;
	case StaticStyleColorProperty::Background:
	case StaticStyleColorProperty::BackgroundColor:
		style.background = color;
		style.hasBackground = !transparent;
		break;
	case StaticStyleColorProperty::Border:
	case StaticStyleColorProperty::BorderTop: style.borderColor = color; break;
	default: break;
	}
}

void applyRule(ComputedStyle &style, const Rule &rule)
{
	switch (rule.kind) {
	case RuleKind::Property: setProperty(style, static_cast<Property>(rule.property), pixelsOf(rule.value)); break;
	case RuleKind::Length:
		setLength(style, static_cast<StaticStyleLengthProperty>(rule.property), Length{rule.value, rule.unit});
		break;
	case RuleKind::Color:
		applyColor(style, static_cast<StaticStyleColorProperty>(rule.property), rule.color, rule.transparent);
		break;
	case RuleKind::Radius: style.radius = static_cast<std::int16_t>(pixelsOf(rule.value)); break;
	case RuleKind::FontWeight: style.fontWeight = static_cast<std::int16_t>(pixelsOf(rule.value)); break;
	}
}

bool ruleMatches(const Rule &rule, const PNode &node)
{
	if (rule.selectorKind == StaticStyleSelectorKind::Element) return !std::strcmp(node.tag, rule.selector);
	if (rule.selectorKind == StaticStyleSelectorKind::Class) return hasClassToken(node.classes, rule.selector);
	return false;
}

void computeStyles(int id, const ComputedStyle *parent)
{
	PNode *node = nodeAt(id);
	if (!node) return;
	ComputedStyle style;
	if (parent) {
		style.color = parent->color;
		style.fontSize = parent->fontSize;
		style.fontWeight = parent->fontWeight;
		style.textAlign = parent->textAlign;
	}
	// Element rules first, class rules after: a class selector outranks a
	// type selector, and within one kind the later declaration wins.
	for (const Rule &rule : rules()) {
		if (rule.selectorKind == StaticStyleSelectorKind::Element && ruleMatches(rule, *node)) applyRule(style, rule);
	}
	for (const Rule &rule : rules()) {
		if (rule.selectorKind == StaticStyleSelectorKind::Class && ruleMatches(rule, *node)) applyRule(style, rule);
	}
	for (const InlineRule &declaration : node->inlineRules) applyRule(style, declaration);
	node->style = style;
	node->font = -1;
	const std::vector<int> children = node->children;
	for (int child : children) computeStyles(child, &nodeAt(id)->style);
}

// ---- Layout ---------------------------------------------------------------

int resolve(const Length &length, int basis)
{
	const State &s = state();
	switch (length.unit) {
	case kPx: return pixelsOf(length.value);
	case kPercent: return length.value * basis / (100 * kFixedOne);
	case kVw: return length.value * s.width / (100 * kFixedOne);
	case kVh: return length.value * s.height / (100 * kFixedOne);
	default: return 0;
	}
}

int fontFor(PNode &node)
{
	if (node.font < 0) node.font = static_cast<std::int8_t>(gea_pebble_font_for(node.style.fontSize, node.style.fontWeight));
	return node.font;
}

// Pebble's fonts carry a few pixels of built-in top padding; set text that
// much higher so it sits centered in the box layout gave it.
int textTopInset(int fontSize)
{
	return fontSize >= 28 ? fontSize / 5 : fontSize / 4;
}

bool isText(const PNode &node)
{
	return node.text;
}

struct Size {
	int w;
	int h;
};

int padH(const ComputedStyle &s, int basis)
{
	return resolve(s.padding[1], basis) + resolve(s.padding[3], basis) + 2 * s.borderWidth;
}

int padV(const ComputedStyle &s, int basis)
{
	return resolve(s.padding[0], basis) + resolve(s.padding[2], basis) + 2 * s.borderWidth;
}

int clampTo(int value, const Length &min, const Length &max, int basis)
{
	if (max.set()) {
		const int limit = resolve(max, basis);
		if (value > limit) value = limit;
	}
	if (min.set()) {
		const int limit = resolve(min, basis);
		if (value < limit) value = limit;
	}
	return value;
}

bool inFlow(const PNode &node)
{
	return node.style.display != 1 && !node.style.absolute;
}

// Content-derived size of `id` when laid out in at most `availW` pixels.
Size intrinsic(int id, int availW, int availH)
{
	PNode *node = nodeAt(id);
	if (!node || node->style.display == 1) return {0, 0};
	const ComputedStyle &s = node->style;
	int w = 0;
	int h = 0;
	if (s.width.set()) availW = resolve(s.width, availW);
	if (isText(*node)) {
		gea_pebble_measure_text(node->content.c_str(), fontFor(*node), availW - padH(s, availW), &w, &h);
		w += padH(s, availW);
		h += padV(s, availH);
	} else {
		const int innerW = availW - padH(s, availW);
		const int gap = resolve(s.gap, innerW);
		int count = 0;
		for (int child : node->children) {
			PNode *c = nodeAt(child);
			if (!c || !inFlow(*c)) continue;
			const Size cs = intrinsic(child, innerW, availH);
			const int mh = resolve(c->style.margin[1], innerW) + resolve(c->style.margin[3], innerW);
			const int mv = resolve(c->style.margin[0], innerW) + resolve(c->style.margin[2], innerW);
			if (s.row && s.display == 3) {
				w += cs.w + mh;
				if (cs.h + mv > h) h = cs.h + mv;
			} else {
				h += cs.h + mv;
				if (cs.w + mh > w) w = cs.w + mh;
			}
			++count;
		}
		if (count > 1) {
			if (s.row && s.display == 3) w += gap * (count - 1);
			else h += gap * (count - 1);
		}
		w += padH(s, availW);
		h += padV(s, availH);
	}
	if (s.width.set()) w = resolve(s.width, availW);
	if (s.height.set()) h = resolve(s.height, availH);
	return {clampTo(w, s.minWidth, s.maxWidth, availW), clampTo(h, s.minHeight, s.maxHeight, availH)};
}

void place(int id, int x, int y, int w, int h);

void layoutChildren(int id)
{
	PNode *node = nodeAt(id);
	if (!node || isText(*node)) return;
	const ComputedStyle s = node->style;
	const int left = node->x + resolve(s.padding[3], node->w) + s.borderWidth;
	const int top = node->y + resolve(s.padding[0], node->w) + s.borderWidth;
	const int innerW = node->w - padH(s, node->w);
	const int innerH = node->h - padV(s, node->h);
	const bool row = s.row && s.display == 3;
	const int mainSize = row ? innerW : innerH;
	const int gap = resolve(s.gap, innerW);

	struct Item {
		int id;
		int main;
		int cross;
		int marginBefore;
		int marginAfter;
		int crossBefore;
		int crossAfter;
		int grow;
	};
	std::vector<Item> items;
	items.reserve(node->children.size());
	int used = 0;
	int grow = 0;
	const std::vector<int> children = node->children;
	for (int child : children) {
		PNode *c = nodeAt(child);
		if (!c) continue;
		if (c->style.display == 1) {
			c->w = c->h = 0;
			continue;
		}
		if (c->style.absolute) {
			const ComputedStyle &cs = c->style;
			const Size size = intrinsic(child, innerW, innerH);
			int cx = left;
			int cy = top;
			if (cs.inset[3].set()) cx = left + resolve(cs.inset[3], innerW);
			else if (cs.inset[1].set()) cx = left + innerW - resolve(cs.inset[1], innerW) - size.w;
			if (cs.inset[0].set()) cy = top + resolve(cs.inset[0], innerH);
			else if (cs.inset[2].set()) cy = top + innerH - resolve(cs.inset[2], innerH) - size.h;
			place(child, cx, cy, size.w, size.h);
			continue;
		}
		const ComputedStyle &cs = c->style;
		const Size size = intrinsic(child, row ? innerW : innerW, innerH);
		Item item{};
		item.id = child;
		item.main = row ? size.w : size.h;
		item.cross = row ? size.h : size.w;
		const int mTop = resolve(cs.margin[0], innerW), mRight = resolve(cs.margin[1], innerW);
		const int mBottom = resolve(cs.margin[2], innerW), mLeft = resolve(cs.margin[3], innerW);
		item.marginBefore = row ? mLeft : mTop;
		item.marginAfter = row ? mRight : mBottom;
		item.crossBefore = row ? mTop : mLeft;
		item.crossAfter = row ? mBottom : mRight;
		item.grow = s.display == 3 ? cs.grow : 0;
		used += item.main + item.marginBefore + item.marginAfter;
		grow += item.grow;
		items.push_back(item);
	}
	if (items.size() > 1) used += gap * static_cast<int>(items.size() - 1);

	int free = mainSize - used;
	if (free > 0 && grow > 0) {
		for (Item &item : items) item.main += free * item.grow / grow;
		free = 0;
	}

	// Block and grid containers stack their children from the start; only a
	// flex container distributes free space.
	const std::int8_t justify = s.display == 3 ? s.justify : static_cast<std::int8_t>(kAlignStart);
	int cursor = 0;
	int between = gap;
	if (free > 0) {
		switch (justify) {
		case kAlignCenter: cursor = free / 2; break;
		case kAlignEnd: cursor = free; break;
		case kAlignBetween:
			if (items.size() > 1) between += free / static_cast<int>(items.size() - 1);
			break;
		case kAlignAround:
			if (!items.empty()) {
				const int share = free / static_cast<int>(items.size());
				cursor = share / 2;
				between += share;
			}
			break;
		default: break;
		}
	}

	const int crossSize = row ? innerH : innerW;
	for (const Item &item : items) {
		PNode *c = nodeAt(item.id);
		if (!c) continue;
		const ComputedStyle &cs = c->style;
		std::int8_t align = cs.alignSelf >= 0 ? cs.alignSelf : s.alignItems;
		if (s.display != 3) align = kAlignStretch;
		const bool crossFixed = row ? cs.height.set() : cs.width.set();
		int cross = item.cross;
		const int crossRoom = crossSize - item.crossBefore - item.crossAfter;
		if (align == kAlignStretch && !crossFixed) cross = crossRoom;
		int crossOffset = item.crossBefore;
		if (align == kAlignCenter) crossOffset += (crossRoom - cross) / 2;
		else if (align == kAlignEnd) crossOffset += crossRoom - cross;
		cursor += item.marginBefore;
		if (row) place(item.id, left + cursor, top + crossOffset, item.main, cross);
		else place(item.id, left + crossOffset, top + cursor, cross, item.main);
		cursor += item.main + item.marginAfter + between;
	}
}

void place(int id, int x, int y, int w, int h)
{
	PNode *node = nodeAt(id);
	if (!node) return;
	node->x = static_cast<std::int16_t>(x);
	node->y = static_cast<std::int16_t>(y);
	node->w = static_cast<std::int16_t>(w);
	node->h = static_cast<std::int16_t>(h);
	layoutChildren(id);
}

void layoutAll()
{
	State &s = state();
	if (s.styleDirty) {
		computeStyles(s.root, nullptr);
		s.styleDirty = false;
	}
	place(s.root, 0, 0, s.width, s.height);
	s.layoutDirty = false;
}

// ---- Drawing --------------------------------------------------------------

void draw(void *ctx, int id)
{
	PNode *node = nodeAt(id);
	if (!node || node->style.display == 1) return;
	const ComputedStyle &s = node->style;
	const State &st = state();
	const int x = node->x;
	const int y = node->y - st.scrollY;
	if (y > st.height || y + node->h < 0) {
		// Off screen; a child can still overflow into view.
		for (int child : node->children) draw(ctx, child);
		return;
	}
	if (s.hasBackground) gea_pebble_fill_rect(ctx, x, y, node->w, node->h, s.radius, s.background);
	if (s.borderWidth > 0) gea_pebble_stroke_rect(ctx, x, y, node->w, node->h, s.radius, s.borderWidth, s.borderColor);
	if (st.focus == id && !st.touching) {
		gea_pebble_stroke_rect(ctx, x - 2, y - 2, node->w + 4, node->h + 4, s.radius + 2, 2, 0xFF);
	}
	if (isText(*node) && !node->content.empty()) {
		const int padLeft = resolve(s.padding[3], node->w) + s.borderWidth;
		const int padTop = resolve(s.padding[0], node->w) + s.borderWidth;
		const int innerW = node->w - padH(s, node->w);
		const int innerH = node->h - padV(s, node->h);
		const int font = fontFor(*node);
		int textW = 0;
		int textH = 0;
		gea_pebble_measure_text(node->content.c_str(), font, innerW, &textW, &textH);
		const int offset = innerH > textH ? (innerH - textH) / 2 : 0;
		gea_pebble_draw_text(ctx, node->content.c_str(), font, x + padLeft, y + padTop + offset - textTopInset(s.fontSize),
		                     innerW, textH + textTopInset(s.fontSize) + 4, s.textAlign, s.color);
	}
	for (int child : node->children) draw(ctx, child);
}

// ---- Events ---------------------------------------------------------------

bool hasListener(const PNode &node, std::uint8_t type)
{
	for (const Listener &listener : node.listeners) {
		if (listener.type == type) return true;
	}
	return false;
}

// Deliver `type` to `target` and each ancestor in turn, the way a DOM event
// bubbles. Listeners are copied out first: a handler may rebuild the tree.
bool dispatch(int target, std::uint8_t type, PointerEventType eventType, int x, int y)
{
	PointerEvent event{};
	event.type = eventType;
	event.target = gea::framework::events::EventTarget(target);
	event.targetId = target;
	event.x = event.clientX = event.pageX = event.screenX = x;
	event.y = event.clientY = event.pageY = event.screenY = y;
	bool handled = false;
	for (int id = target; id >= 0;) {
		PNode *node = nodeAt(id);
		if (!node) break;
		const int parent = node->parent;
		std::vector<EventListener> callbacks;
		for (const Listener &listener : node->listeners) {
			if (listener.type == type) callbacks.push_back(listener.callback);
		}
		event.currentTarget = gea::framework::events::EventTarget(id);
		event.currentTargetId = id;
		event.eventPhase = id == target ? gea::framework::events::EventPhase::AtTarget : gea::framework::events::EventPhase::Bubbling;
		for (EventListener &callback : callbacks) {
			callback(event);
			handled = true;
		}
		if (event.propagationStopped) break;
		id = parent;
	}
	return handled;
}

int hitTest(int id, int x, int y)
{
	PNode *node = nodeAt(id);
	if (!node || node->style.display == 1) return -1;
	for (auto it = node->children.rbegin(); it != node->children.rend(); ++it) {
		const int hit = hitTest(*it, x, y);
		if (hit >= 0) return hit;
	}
	if (x >= node->x && x < node->x + node->w && y >= node->y && y < node->y + node->h) return id;
	return -1;
}

int contentBottom(int id)
{
	PNode *node = nodeAt(id);
	if (!node || node->style.display == 1) return 0;
	int bottom = node->y + node->h;
	for (int child : node->children) {
		const int childBottom = contentBottom(child);
		if (childBottom > bottom) bottom = childBottom;
	}
	return bottom;
}

void scrollIntoView(int id)
{
	State &s = state();
	PNode *node = nodeAt(id);
	if (!node) return;
	constexpr int kMargin = 6;
	if (node->y - kMargin < s.scrollY) s.scrollY = node->y - kMargin;
	else if (node->y + node->h + kMargin > s.scrollY + s.height) s.scrollY = node->y + node->h + kMargin - s.height;
	const int maxScroll = contentBottom(s.root) - s.height;
	if (s.scrollY > maxScroll) s.scrollY = maxScroll;
	if (s.scrollY < 0) s.scrollY = 0;
}

void collectFocusable(int id, std::vector<int> &out)
{
	PNode *node = nodeAt(id);
	if (!node || node->style.display == 1) return;
	// The root and body hold every delegated JSX listener; they are the page,
	// not controls on it.
	const State &s = state();
	if (id != s.root && id != s.body && (node->control || hasListener(*node, kListenClick))) out.push_back(id);
	for (int child : node->children) collectFocusable(child, out);
}

void moveFocus(int step)
{
	State &s = state();
	std::vector<int> focusable;
	collectFocusable(s.root, focusable);
	if (focusable.empty()) {
		s.focus = -1;
		return;
	}
	int index = -1;
	for (std::size_t i = 0; i < focusable.size(); ++i) {
		if (focusable[i] == s.focus) index = static_cast<int>(i);
	}
	const int count = static_cast<int>(focusable.size());
	index = index < 0 ? (step > 0 ? 0 : count - 1) : (index + step + count) % count;
	s.focus = focusable[static_cast<std::size_t>(index)];
	scrollIntoView(s.focus);
	gea_pebble_request_redraw();
}

// ---- Inline styles ---------------------------------------------------------

std::string trimmed(const std::string &text)
{
	std::size_t start = 0, end = text.size();
	while (start < end && (text[start] == ' ' || text[start] == '\t')) ++start;
	while (end > start && (text[end - 1] == ' ' || text[end - 1] == '\t')) --end;
	return text.substr(start, end - start);
}

// A CSS number with an optional unit, in fixed point, without floats.
bool parseLength(const std::string &text, Length &out)
{
	std::size_t i = 0;
	const bool negative = i < text.size() && text[i] == '-';
	if (negative || (i < text.size() && text[i] == '+')) ++i;
	int whole = 0, fraction = 0, scale = 1;
	bool digits = false;
	while (i < text.size() && text[i] >= '0' && text[i] <= '9') whole = whole * 10 + (text[i++] - '0'), digits = true;
	if (i < text.size() && text[i] == '.') {
		++i;
		while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
			if (scale < 10000) fraction = fraction * 10 + (text[i] - '0'), scale *= 10;
			++i, digits = true;
		}
	}
	if (!digits) return false;
	int fixed = whole * kFixedOne + fraction * kFixedOne / scale;
	if (negative) fixed = -fixed;
	const std::string unit = text.substr(i);
	if (unit.empty() || unit == "px") out = {fixed, kPx};
	else if (unit == "%") out = {fixed, kPercent};
	else if (unit == "vw" || unit == "vmin") out = {fixed, kVw};
	else if (unit == "vh" || unit == "vmax") out = {fixed, kVh};
	else return false;
	return true;
}

int hexDigit(char c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }

bool parseColor(const std::string &text, std::uint8_t &color, bool &transparent)
{
	int r = 0, g = 0, b = 0, a = 255;
	if (text == "transparent") {
		color = 0, transparent = true;
		return true;
	}
	if (!text.empty() && text[0] == '#') {
		const std::size_t n = text.size() - 1;
		int v[8];
		for (std::size_t k = 0; k < n && k < 8; ++k)
			if ((v[k] = hexDigit(text[k + 1])) < 0) return false;
		if (n == 3 || n == 4) {
			r = v[0] * 17, g = v[1] * 17, b = v[2] * 17;
			if (n == 4) a = v[3] * 17;
		} else if (n == 6 || n == 8) {
			r = v[0] * 16 + v[1], g = v[2] * 16 + v[3], b = v[4] * 16 + v[5];
			if (n == 8) a = v[6] * 16 + v[7];
		} else {
			return false;
		}
	} else if (!text.compare(0, 4, "rgb(") || !text.compare(0, 5, "rgba(")) {
		int parts[4] = {0, 0, 0, 255}, count = 0;
		std::size_t i = text.find('(') + 1;
		while (i < text.size() && count < 4) {
			while (i < text.size() && (text[i] == ' ' || text[i] == ',')) ++i;
			int value = 0;
			bool digits = false;
			while (i < text.size() && text[i] >= '0' && text[i] <= '9') value = value * 10 + (text[i++] - '0'), digits = true;
			if (!digits) break;
			if (i < text.size() && text[i] == '.') {  // an alpha like 0.5
				int tenths = i + 1 < text.size() && text[i + 1] >= '0' && text[i + 1] <= '9' ? text[i + 1] - '0' : 0;
				value = value * 255 + tenths * 255 / 10;
				while (i < text.size() && text[i] != ',' && text[i] != ')') ++i;
			} else if (count == 3) {
				value *= 255;
			}
			parts[count++] = value;
		}
		if (count < 3) return false;
		r = parts[0], g = parts[1], b = parts[2], a = parts[3];
	} else {
		static const struct {
			const char *name;
			int rgb;
		} named[] = {{"black", 0x000000}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff},
		             {"yellow", 0xffff00}, {"cyan", 0x00ffff}, {"magenta", 0xff00ff}, {"gray", 0x808080}, {"grey", 0x808080},
		             {"orange", 0xffa500}, {"purple", 0x800080}};
		bool found = false;
		for (const auto &entry : named) {
			if (text == entry.name) {
				r = entry.rgb >> 16, g = (entry.rgb >> 8) & 0xff, b = entry.rgb & 0xff, found = true;
				break;
			}
		}
		if (!found) return false;
	}
	color = pebbleColor(r, g, b);
	transparent = a < 128;
	return true;
}

// The inline properties this backend has, by CSS name: what kind of rule
// each makes and which property it sets. Keyword properties list the values
// they take.
struct InlineLength {
	const char *name;
	StaticStyleLengthProperty property;
};
constexpr InlineLength kInlineLengths[] = {
	{"left", StaticStyleLengthProperty::Left}, {"top", StaticStyleLengthProperty::Top},
	{"right", StaticStyleLengthProperty::Right}, {"bottom", StaticStyleLengthProperty::Bottom},
	{"width", StaticStyleLengthProperty::Width}, {"height", StaticStyleLengthProperty::Height},
	{"min-width", StaticStyleLengthProperty::MinWidth}, {"min-height", StaticStyleLengthProperty::MinHeight},
	{"max-width", StaticStyleLengthProperty::MaxWidth}, {"max-height", StaticStyleLengthProperty::MaxHeight},
	{"gap", StaticStyleLengthProperty::Gap}, {"font-size", StaticStyleLengthProperty::FontSize},
	{"border-width", StaticStyleLengthProperty::BorderWidth},
	{"padding-top", StaticStyleLengthProperty::PaddingTop}, {"padding-right", StaticStyleLengthProperty::PaddingRight},
	{"padding-bottom", StaticStyleLengthProperty::PaddingBottom}, {"padding-left", StaticStyleLengthProperty::PaddingLeft},
	{"margin-top", StaticStyleLengthProperty::MarginTop}, {"margin-right", StaticStyleLengthProperty::MarginRight},
	{"margin-bottom", StaticStyleLengthProperty::MarginBottom}, {"margin-left", StaticStyleLengthProperty::MarginLeft},
};

struct InlineColor {
	const char *name;
	StaticStyleColorProperty property;
};
constexpr InlineColor kInlineColors[] = {
	{"color", StaticStyleColorProperty::Color}, {"background-color", StaticStyleColorProperty::BackgroundColor},
	{"background", StaticStyleColorProperty::Background}, {"border-color", StaticStyleColorProperty::Border},
};

struct InlineKeyword {
	const char *name;
	const char *value;
	Property property;
	int number;
};
constexpr InlineKeyword kInlineKeywords[] = {
	{"display", "none", Property::Display, 1},   {"display", "block", Property::Display, 0},
	{"display", "grid", Property::Display, 2},   {"display", "flex", Property::Display, 3},
	{"position", "absolute", Property::Position, 1}, {"position", "fixed", Property::Position, 1},
	{"position", "relative", Property::Position, 0}, {"position", "static", Property::Position, 0},
	{"flex-direction", "row", Property::FlexDirection, 1}, {"flex-direction", "column", Property::FlexDirection, 0},
	{"text-align", "left", Property::TextAlign, GEA_PEBBLE_ALIGN_LEFT},
	{"text-align", "center", Property::TextAlign, GEA_PEBBLE_ALIGN_CENTER},
	{"text-align", "right", Property::TextAlign, GEA_PEBBLE_ALIGN_RIGHT},
};

// The property an inline declaration named `name` sets -- its kind and
// property, the identity setProperty and removeProperty address it by -- or
// false when this backend has no such property.
bool inlinePropertyFor(const std::string &name, Rule &rule)
{
	rule = Rule{};
	for (const auto &entry : kInlineLengths) {
		if (name != entry.name) continue;
		rule.kind = RuleKind::Length;
		rule.property = static_cast<std::uint8_t>(entry.property);
		return true;
	}
	for (const auto &entry : kInlineColors) {
		if (name != entry.name) continue;
		rule.kind = RuleKind::Color;
		rule.property = static_cast<std::uint8_t>(entry.property);
		return true;
	}
	if (name == "border-radius") {
		rule.kind = RuleKind::Radius;
		return true;
	}
	if (name == "font-weight") {
		rule.kind = RuleKind::FontWeight;
		return true;
	}
	for (const auto &entry : kInlineKeywords) {
		if (name != entry.name) continue;
		rule.kind = RuleKind::Property;
		rule.property = static_cast<std::uint8_t>(entry.property);
		return true;
	}
	return false;
}

// Completes `rule` (from inlinePropertyFor) with `value`, or false when the
// value cannot be read -- the declaration is then ignored, as a browser
// ignores one it cannot parse.
bool inlineValueFor(const std::string &name, const std::string &value, Rule &rule)
{
	switch (rule.kind) {
	case RuleKind::Length: {
		Length length;
		if (!parseLength(value, length)) return false;
		rule.unit = length.unit;
		rule.value = length.value;
		return true;
	}
	case RuleKind::Color: return parseColor(value, rule.color, rule.transparent);
	case RuleKind::Radius: {
		Length length;
		if (!parseLength(value, length) || length.unit != kPx) return false;
		rule.value = length.value;
		return true;
	}
	case RuleKind::FontWeight: {
		const int weight = value == "bold" ? 700 : value == "normal" ? 400 : std::atoi(value.c_str());
		if (weight <= 0) return false;
		rule.value = weight * kFixedOne;
		return true;
	}
	case RuleKind::Property:
		for (const auto &entry : kInlineKeywords) {
			if (name != entry.name || value != entry.value) continue;
			rule.value = entry.number * kFixedOne;
			return true;
		}
		return false;
	}
	return false;
}

bool sameRule(const Rule &a, const Rule &b)
{
	return a.kind == b.kind && a.property == b.property && a.unit == b.unit && a.value == b.value && a.color == b.color &&
	       a.transparent == b.transparent;
}

void setInlineStyle(int id, const std::string &rawName, const std::string &rawValue)
{
	PNode *node = nodeAt(id);
	if (!node) return;
	const std::string name = trimmed(rawName);
	Rule rule;
	if (!inlinePropertyFor(name, rule)) return;
	const std::string value = trimmed(rawValue);
	auto &list = node->inlineRules;
	auto existing = list.begin();
	while (existing != list.end() && !sameProperty(*existing, rule)) ++existing;
	if (value.empty()) {
		if (existing == list.end()) return;
		list.erase(existing);
		invalidate(true);
		return;
	}
	if (!inlineValueFor(name, value, rule)) return;
	if (existing != list.end()) {
		if (sameRule(*existing, rule)) return;
		*existing = rule;
	} else {
		// A style object is usually a few declarations set together; one
		// block sized for them beats three reallocations on a small heap.
		if (list.empty()) list.reserve(4);
		list.push_back(rule);
	}
	invalidate(true);
}

// A `style` attribute: `name: value; name: value`, replacing every inline
// declaration the element had.
void setStyleAttribute(int id, const char *text)
{
	PNode *node = nodeAt(id);
	if (!node) return;
	if (!node->inlineRules.empty()) {
		node->inlineRules.clear();
		invalidate(true);
	}
	const std::string declarations = text ? text : "";
	std::size_t start = 0;
	while (start < declarations.size()) {
		std::size_t end = declarations.find(';', start);
		if (end == std::string::npos) end = declarations.size();
		const std::string declaration = declarations.substr(start, end - start);
		const std::size_t colon = declaration.find(':');
		if (colon != std::string::npos) setInlineStyle(id, declaration.substr(0, colon), declaration.substr(colon + 1));
		start = end + 1;
	}
}

}  // namespace
}  // namespace gea::pebble

using namespace gea::pebble;

// ---- Engine surface: Document ---------------------------------------------

namespace gea::embedded::ui {

Document &Document::instance()
{
	static Document document;
	return document;
}

ViewElement Document::body() const
{
	return ViewElement(state().body);
}

ViewElement Document::createView() const
{
	return ViewElement(allocNode("div", false));
}

ViewElement Document::createButton() const
{
	return ViewElement(allocNode("button", false));
}

TextElement Document::createText(const char *text) const
{
	const int id = allocNode("", true);
	nodeAt(id)->content = text ? text : "";
	return TextElement(id);
}

NodeHandle Document::createElement(const char *tag) const
{
	return NodeHandle(allocNode(tag, false));
}

NodeHandle Document::createDocumentFragment() const
{
	return NodeHandle(allocNode("#fragment", false));
}

NodeHandle Document::getElementById(const char *id) const
{
	if (!id) return NodeHandle();
	State &s = state();
	for (std::size_t i = 0; i < s.nodes.size(); ++i) {
		if (s.nodes[i]->alive && !std::strcmp(elementIdOf(static_cast<int>(i)), id)) return NodeHandle(static_cast<int>(i));
	}
	return NodeHandle();
}

// A document-level listener hears what bubbles to the root, which is where
// every dispatch ends.
void Document::addEventListener(const char *type, gea::framework::events::EventListener listener) const
{
	NodeHandle(state().root).addEventListener(type, std::move(listener));
}

ViewElement Document::ensureAppRoot(const char *id) const
{
	const NodeHandle existing = getElementById(id);
	if (existing.valid()) return ViewElement(existing.id());
	const int node = allocNode("div", false);
	setElementId(node, id);
	body().appendChild(NodeHandle(node));
	return ViewElement(node);
}

void Document::setPreferredMountSize(int width, int height)
{
	state().width = width;
	state().height = height;
}

int Document::preferredMountWidth()
{
	return state().width;
}

int Document::preferredMountHeight()
{
	return state().height;
}

// ---- Engine surface: NodeHandle -------------------------------------------

NodeHandle::NodeTypeProperty::operator int() const
{
	const PNode *node = nodeAt(id);
	if (!node) return 11;
	return node->text && !*node->tag ? 3 : 1;
}

void NodeHandle::appendChild(NodeHandle child) const
{
	PNode *parent = nodeAt(id_);
	if (!parent || !nodeAt(child.id())) return;
	detach(child.id());
	nodeAt(id_)->children.push_back(child.id());
	nodeAt(child.id())->parent = id_;
	invalidate(true);
}

void NodeHandle::insertBefore(NodeHandle child, NodeHandle reference) const
{
	if (!nodeAt(id_) || !nodeAt(child.id())) return;
	detach(child.id());
	auto &list = nodeAt(id_)->children;
	std::size_t at = list.size();
	for (std::size_t i = 0; i < list.size(); ++i) {
		if (list[i] == reference.id()) at = i;
	}
	list.insert(list.begin() + static_cast<std::ptrdiff_t>(at), child.id());
	nodeAt(child.id())->parent = id_;
	invalidate(true);
}

void NodeHandle::replaceChildren(const std::vector<NodeHandle> &children) const
{
	PNode *node = nodeAt(id_);
	if (!node) return;
	const std::vector<int> old = node->children;
	for (int child : old) detach(child);
	for (const NodeHandle &child : children) appendChild(child);
	invalidate(true);
}

NodeHandle NodeHandle::childAt(int index) const
{
	const PNode *node = nodeAt(id_);
	if (!node || index < 0 || index >= static_cast<int>(node->children.size())) return NodeHandle();
	return NodeHandle(node->children[static_cast<std::size_t>(index)]);
}

NodeHandle NodeHandle::firstChildHandle() const
{
	return childAt(0);
}

NodeHandle NodeHandle::nextSiblingHandle() const
{
	const PNode *node = nodeAt(id_);
	if (!node) return NodeHandle();
	const PNode *parent = nodeAt(node->parent);
	if (!parent) return NodeHandle();
	for (std::size_t i = 0; i + 1 < parent->children.size(); ++i) {
		if (parent->children[i] == id_) return NodeHandle(parent->children[i + 1]);
	}
	return NodeHandle();
}

void NodeHandle::setParent(NodeHandle parent) const
{
	parent.appendChild(*this);
}

void NodeHandle::remove() const
{
	if (!nodeAt(id_)) return;
	detach(id_);
	release(id_);
	invalidate(false);
}

void NodeHandle::setText(const char *text) const
{
	PNode *node = nodeAt(id_);
	if (!node) return;
	const char *value = text ? text : "";
	if (node->text) {
		if (node->content == value) return;
		node->content = value;
	} else {
		// Setting text on an element replaces its children with that text.
		const std::vector<int> old = node->children;
		for (int child : old) {
			detach(child);
			release(child);
		}
		const int leaf = allocNode("", true);
		nodeAt(leaf)->content = value;
		NodeHandle(id_).appendChild(NodeHandle(leaf));
	}
	invalidate(false);
}

void NodeHandle::setAttribute(const char *name, const char *value) const
{
	PNode *node = nodeAt(id_);
	if (!node || !name) return;
	const char *text = value ? value : "";
	if (!std::strcmp(name, "class") || !std::strcmp(name, "className")) {
		if (node->classes == text) return;
		node->classes = text;
		invalidate(true);
	} else if (!std::strcmp(name, "id")) {
		setElementId(id_, text);
	} else if (!std::strcmp(name, "style")) {
		setStyleAttribute(id_, text);
	}
}

void NodeHandle::removeAttribute(const char *name) const
{
	setAttribute(name, "");
}

bool NodeHandle::toggleAttribute(const char *name, bool force) const
{
	setAttribute(name, force ? name : "");
	return force;
}

const char *NodeHandle::getAttribute(const char *name) const
{
	const PNode *node = nodeAt(id_);
	if (!node || !name) return nullptr;
	if (!std::strcmp(name, "class") || !std::strcmp(name, "className")) return node->classes.c_str();
	if (!std::strcmp(name, "id")) return elementIdOf(id_);
	return nullptr;
}

bool NodeHandle::hasAttribute(const char *name) const
{
	const char *value = getAttribute(name);
	return value && *value;
}

void NodeHandle::setTagName(const char *tagName) const
{
	PNode *node = nodeAt(id_);
	if (!node) return;
	node->tag = sharedTagName(tagName);
	invalidate(true);
}

const char *NodeHandle::tagName() const
{
	const PNode *node = nodeAt(id_);
	return node ? node->tag : "";
}

void NodeHandle::scrollIntoView() const {}
void NodeHandle::focus() const
{
	if (nodeAt(id_)) state().focus = id_;
}
void NodeHandle::blur() const
{
	if (state().focus == id_) state().focus = -1;
}
bool NodeHandle::play() const
{
	return false;
}
void NodeHandle::pause() const {}
void NodeHandle::setPressId(int) const {}
void NodeHandle::setPressValue(int) const {}

EventListenerId NodeHandle::addEventListener(const char *type, EventListener listener) const
{
	PNode *node = nodeAt(id_);
	if (!node) return gea::framework::events::kInvalidEventListenerId;
	const EventListenerId id = state().nextListenerId++;
	node->listeners.push_back(Listener{id, listenerType(type), std::move(listener)});
	return id;
}

bool NodeHandle::removeEventListener(const char *, EventListenerId listenerId) const
{
	PNode *node = nodeAt(id_);
	if (!node) return false;
	for (std::size_t i = 0; i < node->listeners.size(); ++i) {
		if (node->listeners[i].id == listenerId) {
			node->listeners.erase(node->listeners.begin() + static_cast<std::ptrdiff_t>(i));
			return true;
		}
	}
	return false;
}

// ---- Engine surface: ClassList --------------------------------------------

bool ClassList::contains(const std::string &token) const
{
	const PNode *node = nodeAt(nodeId_);
	return node && hasClassToken(node->classes, token.c_str());
}

bool ClassList::add(const std::string &token) const
{
	PNode *node = nodeAt(nodeId_);
	if (!node || token.empty() || contains(token)) return false;
	if (!node->classes.empty()) node->classes += ' ';
	node->classes += token;
	invalidate(true);
	return true;
}

bool ClassList::remove(const std::string &token) const
{
	PNode *node = nodeAt(nodeId_);
	if (!node || !contains(token)) return false;
	std::string next;
	std::size_t start = 0;
	const std::string &classes = node->classes;
	while (start < classes.size()) {
		while (start < classes.size() && classes[start] == ' ') ++start;
		std::size_t end = start;
		while (end < classes.size() && classes[end] != ' ') ++end;
		if (end > start && classes.compare(start, end - start, token)) {
			if (!next.empty()) next += ' ';
			next.append(classes, start, end - start);
		}
		start = end;
	}
	node->classes = next;
	invalidate(true);
	return true;
}

bool ClassList::toggle(const std::string &token) const
{
	if (contains(token)) {
		remove(token);
		return false;
	}
	add(token);
	return true;
}

bool ClassList::toggle(const std::string &token, bool force) const
{
	if (force) add(token);
	else remove(token);
	return force;
}

void ClassList::set(const char *className) const
{
	NodeHandle(nodeId_).setAttribute("class", className ? className : "");
}

void ClassList::set(const std::string &className) const
{
	set(className.c_str());
}

void ClassList::clear() const
{
	set("");
}

std::string ClassList::value() const
{
	const PNode *node = nodeAt(nodeId_);
	return node ? node->classes : std::string();
}

// ---- Engine surface: Style ------------------------------------------------

void Style::setProperty(const std::string &property, const std::string &value) const
{
	setInlineStyle(nodeId_, property, value);
}

bool Style::removeProperty(const std::string &property) const
{
	const PNode *node = nodeAt(nodeId_);
	if (!node) return false;
	Rule rule;
	if (!inlinePropertyFor(trimmed(property), rule)) return false;
	for (const InlineRule &declaration : node->inlineRules) {
		if (sameProperty(declaration, rule)) {
			setInlineStyle(nodeId_, property, "");
			return true;
		}
	}
	return false;
}

// ---- Engine surface: Tree -------------------------------------------------

Tree &Tree::instance()
{
	static Tree tree;
	return tree;
}

bool Tree::containsNode(int ancestor, int node) const
{
	for (int id = node; id >= 0;) {
		if (id == ancestor) return true;
		const PNode *current = nodeAt(id);
		if (!current) return false;
		id = current->parent;
	}
	return false;
}

// ---- Engine surface: StyleSheet -------------------------------------------

namespace {

void addRule(StaticStyleSelectorKind selectorKind, const char *selector, const char *media, gea::pebble::Rule rule)
{
	// Media queries are for other screens; a watch app's base rules are the
	// ones without one.
	if (media && *media) return;
	if (!selector) return;
	rule.selector = selector;
	rule.selectorKind = selectorKind;
	rules().push_back(rule);
	invalidate(true);
}

}  // namespace

StyleSheet &StyleSheet::instance()
{
	static StyleSheet sheet;
	return sheet;
}

void StyleSheet::beginRuleRegistrationBatch() {}
void StyleSheet::endRuleRegistrationBatch() {}

void StyleSheet::registerStaticPropertyRule(StaticStyleSelectorKind selectorKind, const char *selector, Property property,
                                            int value, const char *media)
{
	gea::pebble::Rule rule{};
	rule.kind = RuleKind::Property;
	rule.property = static_cast<std::uint8_t>(property);
	rule.value = value * gea::pebble::kFixedOne;
	addRule(selectorKind, selector, media, rule);
}

void StyleSheet::registerStaticPropertyGroupRule(StaticStyleSelectorKind selectorKind, const char *selector,
                                                 std::initializer_list<StaticStylePropertyValue> properties, const char *media)
{
	for (const StaticStylePropertyValue &entry : properties) registerStaticPropertyRule(selectorKind, selector, entry.property, entry.value, media);
}

void StyleSheet::registerStaticColorRule(StaticStyleSelectorKind selectorKind, const char *selector, StaticStyleColorProperty property,
                                         int r, int g, int b, int a, const char *media)
{
	gea::pebble::Rule rule{};
	rule.kind = RuleKind::Color;
	rule.property = static_cast<std::uint8_t>(property);
	rule.color = pebbleColor(r, g, b);
	rule.transparent = a < 128;
	addRule(selectorKind, selector, media, rule);
}

void StyleSheet::registerStaticColorVarRule(StaticStyleSelectorKind selectorKind, const char *selector, StaticStyleColorProperty property,
                                            const char *, bool hasFallback, int r, int g, int b, int a, const char *media)
{
	// Custom properties are not resolved on the watch; the declared fallback
	// is what the rule means when the variable is unset.
	if (hasFallback) registerStaticColorRule(selectorKind, selector, property, r, g, b, a, media);
}

void StyleSheet::registerStaticLengthRule(StaticStyleSelectorKind selectorKind, const char *selector, StaticStyleLengthProperty property,
                                          StaticStyleLengthUnit unit, float value, const char *media)
{
	registerStaticLengthSpecRule(selectorKind, selector, property, StaticStyleLengthSpec{unit, value}, media);
}

void StyleSheet::registerStaticLengthSpecRule(StaticStyleSelectorKind selectorKind, const char *selector, StaticStyleLengthProperty property,
                                              StaticStyleLengthSpec length, const char *media)
{
	gea::pebble::Rule rule{};
	rule.kind = RuleKind::Length;
	rule.property = static_cast<std::uint8_t>(property);
	rule.unit = unitFrom(length.unit);
	rule.value = gea::pebble::fixedFromFloat(length.value);
	if (rule.unit == gea::pebble::kUnset) return;
	addRule(selectorKind, selector, media, rule);
}

void StyleSheet::registerStaticBorderRule(StaticStyleSelectorKind selectorKind, const char *selector, StaticStyleLengthSpec width, int r,
                                          int g, int b, int a, const char *media)
{
	registerStaticLengthSpecRule(selectorKind, selector, StaticStyleLengthProperty::BorderWidth, width, media);
	registerStaticColorRule(selectorKind, selector, StaticStyleColorProperty::Border, r, g, b, a, media);
}

void StyleSheet::registerStaticBorderRadiusRule(StaticStyleSelectorKind selectorKind, const char *selector, StaticStyleLengthSpec topLeft,
                                                StaticStyleLengthSpec, StaticStyleLengthSpec, StaticStyleLengthSpec, const char *media)
{
	gea::pebble::Rule rule{};
	rule.kind = RuleKind::Radius;
	rule.value = topLeft.unit == StaticStyleLengthUnit::Px || topLeft.unit == StaticStyleLengthUnit::Raw ? gea::pebble::fixedFromFloat(topLeft.value) : 0;
	addRule(selectorKind, selector, media, rule);
}

void StyleSheet::registerStaticBorderRadiusCornerRule(StaticStyleSelectorKind selectorKind, const char *selector,
                                                      StaticStyleBorderRadiusCorner corner, StaticStyleLengthSpec radius, const char *media)
{
	if (corner == StaticStyleBorderRadiusCorner::TopLeft) registerStaticBorderRadiusRule(selectorKind, selector, radius, radius, radius, radius, media);
}

void StyleSheet::registerStaticFlexRule(StaticStyleSelectorKind selectorKind, const char *selector, int grow, StaticStyleLengthSpec,
                                        bool, const char *media)
{
	registerStaticPropertyRule(selectorKind, selector, Property::Flex, grow, media);
}

void StyleSheet::registerStaticLineHeightRule(StaticStyleSelectorKind, const char *, StaticStyleLineHeightKind, StaticStyleLengthSpec,
                                              const char *)
{
	// Pebble's text engine sets each system font at its own line height.
}

void StyleSheet::registerStaticFontFamilyRule(StaticStyleSelectorKind, const char *, const char *, const char *)
{
	// Only the watch's system fonts exist; size and weight choose among them.
}

// A media query's rules carry its condition as their `media` argument and
// are dropped (addRule): the watch takes the base rules. The plan itself is
// not needed.
std::uint16_t StyleSheet::registerStaticMediaConditionPlan(const char *, std::initializer_list<StaticStyleMediaQuerySpec>) { return 0; }

// calc()/clamp()/min()/max()/var() lengths are not evaluated on the watch; a
// rule using one registers with the Expression unit, which unitFrom() does
// not map, so the rule is dropped rather than applied with a wrong value.
std::uint16_t StyleSheet::registerStaticLengthExpression(StaticStyleLengthExpressionKind, StaticStyleLengthSpec, StaticStyleLengthSpec,
                                                         StaticStyleLengthSpec, float, const char *, bool)
{
	return 0;
}

void StyleSheet::registerStaticBoxShadowNoneRule(StaticStyleSelectorKind, const char *, const char *) {}
void StyleSheet::registerStaticFilterBlurRule(StaticStyleSelectorKind, const char *, StaticStyleLengthSpec, const char *) {}
void StyleSheet::registerStaticCustomLengthRule(StaticStyleSelectorKind, const char *, const char *, StaticStyleLengthSpec, const char *) {}
void StyleSheet::registerStaticCustomColorRule(StaticStyleSelectorKind, const char *, const char *, int, int, int, int, const char *) {}
void StyleSheet::registerStaticRule(const char *, const char *, const char *, const char *) {}
void StyleSheet::registerStaticElementRule(const char *, const char *, const char *, const char *) {}
void StyleSheet::registerStaticSelectorRule(const char *, const char *, const char *, const char *) {}

}  // namespace gea::embedded::ui

// ---- Shell entry points ----------------------------------------------------

extern void __gea_top_level();

namespace gea::framework::app::generated {
void drainMicrotasks();
}  // namespace gea::framework::app::generated

extern "C" {

void gea_pebble_ui_init(int width, int height)
{
	State &s = state();
	s.width = width;
	s.height = height;
	s.root = allocNode("html", false);
	s.body = allocNode("body", false);
	nodeAt(s.root)->children.push_back(s.body);
	nodeAt(s.body)->parent = s.root;
	gea::embedded::ui::Document::instance().ensureAppRoot("app");
	__gea_top_level();
	gea::framework::app::generated::drainMicrotasks();
}

void gea_pebble_ui_frame(uint32_t now_ms)
{
	State &s = state();
	s.inFrame = true;
	gea_pebble_run_animation_frames(now_ms);
	gea::framework::app::generated::drainMicrotasks();
	s.inFrame = false;
	if (s.layoutDirty || s.styleDirty) {
		layoutAll();
		gea_pebble_request_redraw();
	}
}

void gea_pebble_ui_draw(void *ctx)
{
	State &s = state();
	if (s.layoutDirty || s.styleDirty) layoutAll();
	gea_pebble_fill_rect(ctx, 0, 0, s.width, s.height, 0, 0xC0);
	draw(ctx, s.root);
}

void gea_pebble_ui_button(int button)
{
	State &s = state();
	s.touching = false;
	if (button == GEA_PEBBLE_BUTTON_UP) moveFocus(-1);
	else if (button == GEA_PEBBLE_BUTTON_DOWN) moveFocus(1);
	else if (button == GEA_PEBBLE_BUTTON_SELECT) {
		if (s.focus < 0) moveFocus(1);
		if (s.focus >= 0) dispatch(s.focus, kListenClick, PointerEventType::Click, 0, 0);
	}
	gea_pebble_ui_frame(gea_pebble_now_ms());
}

void gea_pebble_ui_touch(int phase, int x, int y)
{
	State &s = state();
	s.touching = true;
	const int target = hitTest(s.root, x, y + s.scrollY);
	if (phase == GEA_PEBBLE_TOUCH_DOWN) {
		s.pressed = target;
		if (target >= 0) dispatch(target, kListenTouchStart, PointerEventType::TouchStart, x, y);
	} else if (phase == GEA_PEBBLE_TOUCH_MOVE) {
		if (target >= 0) dispatch(target, kListenTouchMove, PointerEventType::TouchMove, x, y);
	} else {
		if (target >= 0) dispatch(target, kListenTouchEnd, PointerEventType::TouchEnd, x, y);
		// A tap is a press and release within the same subtree, as on the web.
		const auto &tree = gea::embedded::ui::Tree::instance();
		if (target >= 0 && s.pressed >= 0 && (tree.containsNode(s.pressed, target) || tree.containsNode(target, s.pressed))) {
			dispatch(s.pressed, kListenClick, PointerEventType::Click, x, y);
		}
		s.pressed = -1;
	}
	gea_pebble_ui_frame(gea_pebble_now_ms());
}

void gea_pebble_ui_deinit(void) {}

void gea_pebble_note_listener(int node, const char *type)
{
	PNode *target = nodeAt(node);
	if (!target) return;
	const ListenerType kind = listenerType(type);
	if (kind == kListenClick || kind == kListenTouchStart || kind == kListenTouchEnd) target->control = true;
}

}  // extern "C"
