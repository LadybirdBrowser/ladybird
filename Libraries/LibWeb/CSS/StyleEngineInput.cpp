/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/SetUnion.h>
#include <LibGC/ConservativeVector.h>
#include <LibGC/RootHashTable.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/CSSTransition.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/Invalidation/FormControlInvalidator.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
#include <LibWeb/CSS/Invalidation/PseudoClassInvalidator.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/SelectorMatching.h>
#include <LibWeb/CSS/Sizing.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleScope.h>
#include <LibWeb/CSS/StyleSheetImport.h>
#include <LibWeb/CSS/StyleSheetState.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Slottable.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/CustomElements/CustomStateSet.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLBodyElement.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLFrameSetElement.h>
#include <LibWeb/HTML/HTMLHeadingElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/HTML/HTMLLinkElement.h>
#include <LibWeb/HTML/HTMLObjectElement.h>
#include <LibWeb/HTML/HTMLSelectElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/HTMLTableCellElement.h>
#include <LibWeb/HTML/HTMLTableElement.h>
#include <LibWeb/HTML/HTMLTextAreaElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/ImageProvider.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/RenderDocument.h>
#include <LibWeb/MathML/TagNames.h>
#include <LibWeb/SVG/SVGClipPathElement.h>
#include <LibWeb/SVG/SVGElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGImageElement.h>
#include <LibWeb/SVG/SVGMaskElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>
#include <LibWeb/SVG/SVGSwitchElement.h>
#include <LibWeb/StyleValueRustFFI.h>

namespace Web::CSS {

static void record_element_heading_level(DOM::Element&);

class ElementPublication;

static void publish_element_arrival(StyleEngine&, DOM::Element&, ElementPublication&, Optional<StyleEngineFFI::FfiArrivingElement const&> = {});
static void finish_element_publication(StyleEngine&, ElementPublication&);
static void record_element_inline_style_properties(DOM::Element&);
static void record_heading_levels_in_subtree(DOM::Element&);
static void republish_assigned_slots_of(ReadonlySpan<GC::Ref<DOM::Node>>);
static Optional<StyleEngineFFI::FfiStateFact> state_fact_for(PseudoClass);
static StyleAtomID intern_id_or_class_atom(StyleEngine&, DOM::Element const&, Utf16FlyString const&);
static u32 element_construction_facts(DOM::Element const&);
static u32 element_style_adjustment_facts(DOM::Element const&);
static void publish_element_replaced_content_input(DOM::Element&, StyleEngine&);

static constexpr StyleNodeID no_style_node;
// Shadow trees get their own scopes with the shadow surface; today everything names the document.
static constexpr TreeScopeID document_tree_scope;

static StyleEngine* style_engine_for(DOM::Node& node)
{
    if (!node.is_connected() || !node.document().style_engine_tracks_tree())
        return nullptr;
    return &node.document().style_computer().style_engine();
}

// The engine a fact of the element is recorded into, once the element has taken its style node identity.
static StyleEngine* style_engine_for_identified(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    return style_engine && element.style_node_id() != no_style_node ? style_engine : nullptr;
}

// The same, once the element's initial features are published too. Until they are, they will say what it is.
static StyleEngine* style_engine_for_published(DOM::Element& element)
{
    auto* style_engine = style_engine_for_identified(element);
    return style_engine && !element.initial_style_features_pending() ? style_engine : nullptr;
}

// A relation is only nameable if the element on its other end already has an identity. Naming a
// node the engine has never seen would be worse than naming none: it would assert on a relation
// column that was never allocated.
static StyleNodeID identity_of(GC::Ptr<DOM::Element> element)
{
    if (!element)
        return no_style_node;
    return element->style_node_id();
}

// The identity a node holds in the DOM child sequence the style tree keeps beside its element-only relations. Elements
// and text nodes are its members; comments and processing instructions never reach style or layout, so they hold no
// place in it.
static StyleNodeID dom_order_identity_of(DOM::Node const& node)
{
    if (auto const* element = as_if<DOM::Element>(node))
        return element->style_node_id();
    if (auto const* text = as_if<DOM::Text>(node))
        return text->style_node_id();
    return no_style_node;
}

// An element, a shadow root or the document owns a child sequence. Every other node is only ever a member of one.
static StyleNodeID dom_order_parent_of(DOM::Node const* parent)
{
    if (auto const* element = as_if<DOM::Element>(parent))
        return element->style_node_id();
    if (auto const* shadow_root = as_if<DOM::ShadowRoot>(parent))
        return shadow_root->style_node_id();
    if (auto const* document = as_if<DOM::Document>(parent))
        return document->style_node_id();
    return no_style_node;
}

// Appends the (node, parent, previous sibling) triple that links the node into its parent's child sequence.
static void append_dom_order_link(Vector<u32, 192>& links, DOM::Node const& node)
{
    auto previous = no_style_node;
    for (auto const* sibling = node.previous_sibling(); sibling && previous == no_style_node; sibling = sibling->previous_sibling())
        previous = dom_order_identity_of(*sibling);
    links.append(dom_order_identity_of(node).value());
    links.append(dom_order_parent_of(node.parent()).value());
    links.append(previous.value());
}

static void link_in_dom_order(StyleEngine& style_engine, DOM::Node const& node)
{
    Vector<u32, 192> links;
    append_dom_order_link(links, node);
    StyleEngineFFI::style_engine_link_style_nodes_in_dom_order(style_engine.host(), links.span());
}

// A shadow root's identity, minted on first use.
//
// The root is not an element and gets no style, but it is the parent its children's relations name.
// Giving it a real identity is what keeps a shadow tree inside the same relation columns as the
// document tree: a child combinator still stops at the root, and the subtree a `:host()` rule
// reaches is a subtree the engine can enumerate rather than a boundary it has to widen past.
static TreeScopeID tree_scope_of(DOM::Node&);

static StyleNodeID identity_of_shadow_root(DOM::ShadowRoot& shadow_root, StyleEngine& style_engine)
{
    if (shadow_root.style_node_id() == no_style_node) {
        shadow_root.set_style_node_id(style_engine.mint_style_node());
        shadow_root.document().style_computer().register_style_node(shadow_root.style_node_id(), shadow_root);
        // A shadow root is a scope and a subtree at once. Naming the subtree is what lets a sheet
        // attached here be bounded by the tree it decides in, even when its rules dispatch on
        // nothing the engine can enumerate. It is named here rather than where a scope is numbered,
        // because numbering must not mint a place in the tree: a sheet detaching from a scope whose
        // root has already left would otherwise give that root a new identity on its way out.
        StyleEngineFFI::style_engine_set_tree_scope_root(style_engine.host(), tree_scope_of(shadow_root), shadow_root.style_node_id());
    }
    // A shadow root built from the document's styles rather than its own decides with the author
    // origin from there, which is otherwise bounded by the scope it is attached to.
    if (shadow_root.uses_document_style_sheets())
        StyleEngineFFI::style_engine_set_tree_scope_uses_document_sheets(style_engine.host(), tree_scope_of(shadow_root));
    // The host link is established every time rather than only when the identity is minted, because
    // the two can be asked for in either order: a root whose identity was taken while its host had
    // none would otherwise stay unlinked once the host arrived.
    if (auto host = shadow_root.host(); host && host->style_node_id() != no_style_node)
        StyleEngineFFI::style_engine_set_shadow_root(style_engine.host(), host->style_node_id(), shadow_root.style_node_id());
    return shadow_root.style_node_id();
}

// A child taking its place in a shadow root's DOM child sequence needs the root's identity, since the sequence is named
// by the root. A root that has none yet is named here rather than leaving the child linked under nothing.
static void ensure_dom_order_parent_identity(DOM::Node* parent, StyleEngine& style_engine)
{
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(parent); shadow_root && shadow_root->style_node_id() == no_style_node)
        (void)identity_of_shadow_root(*shadow_root, style_engine);
}

// The style scope a node belongs to.
//
// The document is scope zero. A shadow root's scope is numbered once and kept for as long as the
// root belongs to this document, which is what makes an attach and its later detach name the same
// thing - the root's place in the style tree is given up and retaken every time it disconnects, and
// a scope that moved with it would leave every sheet adopted into it attached forever.
static TreeScopeID tree_scope_of(DOM::Node& document_or_shadow_root)
{
    auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root);
    if (!shadow_root)
        return document_tree_scope;
    if (shadow_root->style_engine_tree_scope() == document_tree_scope)
        shadow_root->set_style_engine_tree_scope(shadow_root->document().style_computer().allocate_tree_scope(*shadow_root));
    return shadow_root->style_engine_tree_scope();
}

template<typename Callback>
static void for_each_shadow_including_inclusive_descendant_with_scope(DOM::Node& node, TreeScopeID tree_scope, Callback& callback)
{
    callback(node, tree_scope);

    if (auto* element = as_if<DOM::Element>(node); element && element->shadow_root()) {
        auto& shadow_root = *element->shadow_root();
        auto shadow_scope = tree_scope_of(shadow_root);
        for_each_shadow_including_inclusive_descendant_with_scope(shadow_root, shadow_scope, callback);
    }

    for (auto* child = node.first_child(); child; child = child->next_sibling())
        for_each_shadow_including_inclusive_descendant_with_scope(*child, tree_scope, callback);
}

TreeScopeID style_engine_tree_scope_for(DOM::Node& document_or_shadow_root)
{
    return tree_scope_of(document_or_shadow_root);
}

// The style-tree parent of an element: its parent element, or the shadow root it is a top-level
// child of.
static StyleNodeID style_tree_parent_of(DOM::Element& element, StyleEngine& style_engine)
{
    if (auto parent = element.parent_element())
        return parent->style_node_id();
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(element.parent()))
        return identity_of_shadow_root(*shadow_root, style_engine);
    return no_style_node;
}

// The engine's tree is the DOM without the subtrees still waiting to arrive, so a sibling relation
// names the nearest sibling that has arrived, starting at `element`. While arrivals are taken in,
// every waiting node already has its identity, and that is the sibling itself.
static StyleNodeID identity_of_arrived_previous_sibling(GC::Ptr<DOM::Element> element)
{
    for (; element; element = element->previous_element_sibling()) {
        if (element->style_node_id() != no_style_node)
            return element->style_node_id();
    }
    return no_style_node;
}

static StyleNodeID identity_of_arrived_next_sibling(GC::Ptr<DOM::Element> element)
{
    for (; element; element = element->next_element_sibling()) {
        if (element->style_node_id() != no_style_node)
            return element->style_node_id();
    }
    return no_style_node;
}

// A subtree leaves in tree order, so below its root every previous sibling has already departed and
// none is left to name. Scanning past them would make removing a wide subtree quadratic.
enum class PreviousSiblings {
    Arrived,
    Departed,
};

static StyleEngineFFI::FfiTreeRelations relations_of(DOM::Element& element, StyleEngine& style_engine, TreeScopeID tree_scope, PreviousSiblings previous_siblings = PreviousSiblings::Arrived)
{
    auto assigned_slot = no_style_node;
    if (auto slot = element.assigned_slot_internal())
        assigned_slot = slot->style_node_id();

    return StyleEngineFFI::FfiTreeRelations {
        .parent = style_tree_parent_of(element, style_engine).value(),
        .previous_element_sibling = previous_siblings == PreviousSiblings::Departed
            ? no_style_node.value()
            : identity_of_arrived_previous_sibling(element.previous_element_sibling()).value(),
        .next_element_sibling = identity_of_arrived_next_sibling(element.next_element_sibling()).value(),
        .tree_scope = tree_scope.value(),
        .assigned_slot = assigned_slot.value(),
    };
}

static StyleEngineFFI::FfiTreeRelations relations_of(DOM::Element& element, StyleEngine& style_engine)
{
    return relations_of(element, style_engine, tree_scope_of(element.root()));
}

static void record_feature(
    DOM::Element&,
    StyleEngineFFI::FfiFeatureKind,
    StyleAtomID name_atom,
    StyleEngineFFI::FfiFeatureValueKind new_kind,
    StyleAtomID new_atom);

static StyleEngineFFI::FfiTreeRelations detached_relations()
{
    return StyleEngineFFI::FfiTreeRelations {
        .parent = no_style_node.value(),
        .previous_element_sibling = no_style_node.value(),
        .next_element_sibling = no_style_node.value(),
        .tree_scope = 0,
        .assigned_slot = no_style_node.value(),
    };
}

// An element that stays in the tree moves from one set of relations to another. Its delta takes the relinking path
// rather than the neighbor one, which is the only one that updates the engine's relation columns.
static void record_element_relinked(StyleEngine& style_engine, StyleNodeID node, StyleEngineFFI::FfiTreeRelations const& old_relations, StyleEngineFFI::FfiTreeRelations const& new_relations)
{
    style_engine.record_tree_delta({
        .node = node.value(),
        .old_connected = true,
        .new_connected = true,
        .old_relations = old_relations,
        .new_relations = new_relations,
    });
}

// How many nodes ahead a loop over a big list of nodes asks for their memory.
static constexpr size_t nodes_ahead = 8;

// Asks for the memory of a node whose kind isn't known yet, which reading its kind would wait for.
static void prefetch_node(DOM::Node const& node)
{
    auto const* bytes = reinterpret_cast<u8 const*>(&node);
    for (size_t offset = 0; offset < sizeof(DOM::Element); offset += 64)
        __builtin_prefetch(bytes + offset);
}

// OPTIMIZATION: Most of an element's publication is first reading memory no one touched since the element was parsed.
//               Asking for it ahead turns waiting for one element at a time into overlapping waits. Each stage reads
//               what the one before it asked for, so the distances shrink along the chain.
template<typename NodeAt>
static void prefetch_publication_ahead(size_t index, size_t count, NodeAt const& node_at)
{
    if (index + 2 * nodes_ahead < count) {
        if (DOM::Node const* ahead = node_at(index + 2 * nodes_ahead))
            prefetch_node(*ahead);
    }
    if (index + nodes_ahead < count) {
        if (auto const* ahead = as_if<DOM::Element>(node_at(index + nodes_ahead))) {
            __builtin_prefetch(ahead->inline_style_declarations());
            __builtin_prefetch(ahead->attribute_list_memory());
        }
    }
    if (index + nodes_ahead / 2 < count) {
        if (auto const* ahead = as_if<DOM::Element>(node_at(index + nodes_ahead / 2)); ahead && ahead->inline_style_declarations())
            Parser::ValueParserFFI::rust_declaration_block_prefetch_header(ahead->inline_style_declarations());
    }
}

// A node that connects takes no identity at once. Script often inserts markup and replaces it again before anything
// reads style, and a node nothing observes then costs the engine nothing: its subtree is marked as waiting to arrive,
// and take_in_pending_style_arrivals() gives it its identity once something observes the engine.
//
// Every connected node without an identity has a shadow-including inclusive ancestor marked as waiting, and every
// ancestor above that one is marked as having a waiting descendant, which is the path the take-in walks down.
static void mark_style_arrival_pending(DOM::Node& node, StyleEngine& style_engine)
{
    style_engine.note_pending_arrivals(1);
    style_engine.list_arrival(node);
    node.set_style_arrival_pending(true);
    // NB: The path is marked all the way up: a take-in that finds the subtrees among the nodes listed clears only the
    //     paths to them, and may leave the mark of a subtree that left on an element below a path it cleared.
    for (auto* ancestor = node.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (ancestor->style_arrival_pending())
            break;
        ancestor->set_descendant_style_arrival_pending(true);
    }
}

// Whether a subtree already waiting to arrive holds the node, or the children of an element in a display:none subtree
// that wait do. An element without an identity is always in one or the other. Where `holder` is given, the walk goes on
// to the nearest element with an identity, and sets it there if that element's children wait.
static bool waits_to_arrive_with_an_ancestor(DOM::Node& node, DOM::Element** holder = nullptr)
{
    bool waits = false;
    for (auto* ancestor = node.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        auto* element = as_if<DOM::Element>(*ancestor);
        if (element && element->style_node_id() != no_style_node) {
            if (!element->child_style_arrivals_wait())
                return waits;
            if (holder)
                *holder = element;
            return true;
        }
        if (element || ancestor->style_arrival_pending()) {
            waits = true;
            if (!holder)
                return true;
            continue;
        }
        if (!is<DOM::ShadowRoot>(*ancestor))
            return waits;
    }
    return waits;
}

// The element that arrived alone in a display:none subtree whose waiting children the node is, or is inside of.
static DOM::Element* element_whose_waiting_children_hold(DOM::Node& node)
{
    DOM::Element* holder = nullptr;
    (void)waits_to_arrive_with_an_ancestor(node, &holder);
    return holder;
}

// Whether a name answers to one of the keys. Tag and attribute names are keyed folded, as are IDs and classes in quirks
// mode, and folding a name that is folded already allocates nothing.
static bool name_answers_to_keys(Utf16FlyString const& name, bool folds, StyleEngine::RelationalArrivalKeys const& keys)
{
    return keys.names.contains(name.raw_identity()) || (folds && keys.names.contains(name.to_ascii_lowercase().raw_identity()));
}

// Whether an element can wait to arrive below one in a display:none subtree. A `:has()` argument could reach its anchor
// through the element's keys, and an element that is an SVG element, a style sheet's owner, a slot, a shadow host or in
// the top layer has an effect beyond its subtree, or a place in the engine's tree other than its parent's child, even
// while it is hidden. An animated element keeps its style while hidden.
static bool element_can_wait_to_arrive(DOM::Element& element, StyleEngine::RelationalArrivalKeys const& keys)
{
    if (keys.reached_by_any_arrival || is<SVG::SVGElement>(element) || element.is_html_style_element() || is<HTML::HTMLLinkElement>(element)
        || is<HTML::HTMLSlotElement>(element) || element.shadow_root() || element.in_top_layer() || element.has_associated_animations())
        return false;
    if (!keys.names.is_empty()) {
        auto folds_ids_and_classes = element.document().in_quirks_mode();
        if (name_answers_to_keys(element.local_name(), true, keys))
            return false;
        if (auto const& id = element.id(); id.has_value() && name_answers_to_keys(*id, folds_ids_and_classes, keys))
            return false;
        if (any_of(element.class_names(), [&](auto const& class_name) { return name_answers_to_keys(class_name, folds_ids_and_classes, keys); }))
            return false;
        bool attribute_answers = false;
        element.for_each_attribute([&](DOM::QualifiedName const& name, Utf16String const&) {
            attribute_answers = attribute_answers || name_answers_to_keys(name.local_name(), true, keys);
        });
        if (attribute_answers)
            return false;
    }
    return !any_of(keys.state_pseudo_classes, [&](auto pseudo_class) { return SelectorMatching::element_matches_state(element, pseudo_class); });
}

// The children that waited become subtrees waiting to arrive like any other. Whoever releases them outside of a take-in
// notes that they arrive as the input is next submitted.
static void release_waiting_children(DOM::Element& element)
{
    element.document().style_computer().style_engine().note_unlisted_arrivals();
    element.set_child_style_arrivals_wait(false);
    element.set_descendant_style_arrival_pending(true);
    auto mark = [](DOM::Node& child) { child.set_style_arrival_pending(true); };
    if (auto shadow_root = element.shadow_root())
        mark(*shadow_root);
    for (auto* child = element.first_child(); child; child = child->next_sibling()) {
        if (is<DOM::Element>(*child) || is<DOM::Text>(*child))
            mark(*child);
    }
}

// Children that waited may have to arrive as the input is next submitted. A style update or a take-in only notes it.
// Script records it as it records an insertion, which the transaction a geometry read deferred must not be waiting for.
static void note_waiting_children_may_arrive(StyleEngine& style_engine, DOM::Document const& document)
{
    if (style_engine.is_taking_in_pending_arrivals() || document.is_in_style_stabilization_epoch())
        style_engine.note_waiting_arrivals_may_arrive();
    else
        style_engine.note_pending_arrivals(1);
}

// Releases the waiting children outside of a take-in, which they arrive at as the input is next submitted.
static void release_waiting_children_until_submitted(DOM::Element& element, StyleEngine& style_engine)
{
    release_waiting_children(element);
    note_waiting_children_may_arrive(style_engine, element.document());
}

// Releases the waiting children that hold the node, if any do.
static void release_waiting_children_holding(DOM::Node& node, StyleEngine& style_engine)
{
    if (auto* holder = element_whose_waiting_children_hold(node))
        release_waiting_children_until_submitted(*holder, style_engine);
}

// What joins children that wait in a display:none subtree is looked at as it connects, against the keys they were last
// looked at against: keys that moved since have them all looked at again anyway. Only a document where children may
// wait walks past the nearest node waiting to arrive to look for them.
static void record_node_connected(DOM::Node& node, StyleEngine& style_engine)
{
    DOM::Element* holder = nullptr;
    if (!waits_to_arrive_with_an_ancestor(node, style_engine.may_have_waiting_children() ? &holder : nullptr)) {
        mark_style_arrival_pending(node, style_engine);
        return;
    }
    if (!holder) {
        style_engine.note_pending_arrivals(1);
        return;
    }
    if (auto* element = as_if<DOM::Element>(node); element && !element_can_wait_to_arrive(*element, style_engine.last_relational_arrival_keys()))
        release_waiting_children_until_submitted(*holder, style_engine);
}

// A name or state an element waiting in a display:none subtree takes is not recorded, as the element arrives with what
// it is then. One that a `:has()` argument may reach it by could let it reach that argument's anchor, so the children
// waiting with it arrive. The keys the children were last looked at against are the ones to ask: keys that moved since
// have them all looked at again anyway.
enum class NamesFold {
    InQuirksMode,
    Always,
};

static void record_waiting_element_names_changed(DOM::Element& element, ReadonlySpan<Utf16FlyString> names, NamesFold names_fold = NamesFold::InQuirksMode)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() != no_style_node)
        return;
    auto const& keys = style_engine->last_relational_arrival_keys();
    auto folds = names_fold == NamesFold::Always || element.document().in_quirks_mode();
    if (keys.reached_by_any_arrival || (!keys.names.is_empty() && any_of(names, [&](auto const& name) { return name_answers_to_keys(name, folds, keys); })))
        release_waiting_children_holding(element, *style_engine);
}

static void record_waiting_element_state_changed(DOM::Element& element, PseudoClass pseudo_class)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() != no_style_node)
        return;
    auto const& keys = style_engine->last_relational_arrival_keys();
    if (keys.reached_by_any_arrival || keys.state_pseudo_classes.contains_slow(pseudo_class))
        release_waiting_children_holding(element, *style_engine);
}

void record_waiting_element_classes_changed(DOM::Element& element)
{
    record_waiting_element_names_changed(element, element.class_names());
}

void record_element_connected(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() != no_style_node)
        return;
    record_node_connected(element, *style_engine);
}

// A text node's row records one of the facts an element's row does, whether it sits in a user agent shadow tree. A
// text node has no element columns in the mirror, so the fact travels on its own row, published where its identity
// arrives.
static bool text_is_in_user_agent_shadow_tree(DOM::Text const& text)
{
    auto shadow_root = text.containing_shadow_root();
    return shadow_root && shadow_root->is_user_agent_internal();
}

void record_text_connected(DOM::Text& text)
{
    auto* style_engine = style_engine_for(text);
    if (!style_engine || text.style_node_id() != no_style_node)
        return;
    record_node_connected(text, *style_engine);
}

// Data only ever arrives with the node or is replaced wholesale, so those are the two places it is published from.
void record_text_data_changed(DOM::Text& text)
{
    auto* style_engine = style_engine_for(text);
    if (!style_engine || text.style_node_id() == no_style_node)
        return;
    StyleEngineFFI::style_engine_set_text_data(style_engine->host(), text.style_node_id().value(), text.data().to_raw_leaked());
    StyleEngineFFI::style_engine_set_text_is_ascii_whitespace(style_engine->host(), text.style_node_id(), text.data().is_ascii_whitespace());
}

// The document's identity, minted before anything connects under it.
//
// The document is not an element and gets no style, but it is the parent the document element's place in the DOM
// child sequence names, and so the root the sequence can be walked from. It is deliberately kept out of the
// element-only relation columns: a selector that reaches for the document element's parent must still find nothing.
void record_document_tree_tracked(DOM::Document& document)
{
    if (document.style_node_id() != no_style_node)
        return;
    auto& style_engine = document.style_computer().style_engine();
    document.set_style_node_id(style_engine.mint_style_node());
    StyleEngineFFI::style_engine_mark_relation_only_style_node(style_engine.host(), document.style_node_id());
    // The viewport's row answers by the document's name, which the document's identity carries.
    StyleEngineFFI::style_engine_set_element_unique_node_id(style_engine.host(), document.style_node_id(), static_cast<u64>(document.unique_id().value()));
}

void record_subtree_connecting(DOM::Node& root)
{
    if (!root.parent())
        return;
    if (auto* style_engine = style_engine_for(*root.parent()))
        mark_style_arrival_pending(root, *style_engine);
}

// A node that is no element hovers the element it is inside of.
static DOM::Element const* hovered_element_of(DOM::Node const* node)
{
    if (!node)
        return nullptr;
    if (auto const* element = as_if<DOM::Element>(*node))
        return element;
    return node->parent_element().ptr();
}

// The questions about an element that elements of its name answer differently.
enum class ElementOpenFacts : u16 {
    None = 0,
    Input = 1 << 0,
    Audio = 1 << 1,
    SvgRoot = 1 << 2,
    SvgSymbol = 1 << 3,
    SvgElement = 1 << 4,
    Body = 1 << 5,
    Heading = 1 << 6,
    ReplacedContent = 1 << 7,
    OwnDirectionality = 1 << 8,
    PresentationalHints = 1 << 9,
};
AK_ENUM_BITWISE_OPERATORS(ElementOpenFacts);

static ElementOpenFacts element_open_facts(DOM::Element const&);
static u32 kind_style_adjustment_facts(DOM::Element const&);
static u32 kind_construction_facts(DOM::Element const&);
static u32 open_style_adjustment_facts(DOM::Element const&, ElementOpenFacts);
static u32 open_construction_facts(DOM::Element const&, ElementOpenFacts, bool is_in_user_agent_shadow_tree);

// What an element's qualified name settles. The name picks the element's class, so this holds for every element of it,
// and only what the name leaves open is asked of each.
struct ElementKind {
    DOM::QualifiedName::Impl const* name { nullptr };
    StyleAtomID tag_atom;
    StyleAtomID folded_tag_atom;
    StyleAtomID namespace_atom;
    u32 adjustment_facts { 0 };
    u32 construction_facts { 0 };
    ElementOpenFacts open_facts { ElementOpenFacts::None };
    SelectorMatching::ElementStateQuestions state_questions { SelectorMatching::ElementStateQuestions::None };
    bool is_slot { false };
};

static ElementKind element_kind_of(StyleEngine& style_engine, DOM::Element const& element)
{
    auto tag_atom = style_engine.intern_atom(element.local_name());
    ElementKind kind {
        .name = element.qualified_name_identity(),
        .tag_atom = tag_atom,
        .folded_tag_atom = tag_atom,
        .namespace_atom = {},
        .adjustment_facts = kind_style_adjustment_facts(element),
        .construction_facts = kind_construction_facts(element),
        .open_facts = element_open_facts(element),
        .state_questions = SelectorMatching::element_state_questions(element),
        .is_slot = is<HTML::HTMLSlotElement>(element),
    };
    if (auto folded_name = element.local_name().to_ascii_lowercase(); folded_name != element.local_name())
        kind.folded_tag_atom = style_engine.intern_atom(folded_name);
    if (auto const& namespace_uri = element.namespace_uri(); namespace_uri.has_value() && !namespace_uri->is_empty())
        kind.namespace_atom = style_engine.intern_atom(*namespace_uri);
    return kind;
}

// What one attribute name settles, which is the same on every element that has it.
struct PublishedAttributeName {
    DOM::QualifiedName::Impl const* name { nullptr };
    StyleAtomID atom;
    bool is_known_unread { false };
    bool is_lang { false };
    bool is_dir { false };
};

// A table a batch keeps of what it found out by name. Names repeat, so a slot a name shares with another only costs
// asking again.
template<typename Entry, size_t slot_count>
class TableByName {
public:
    static_assert((slot_count & (slot_count - 1)) == 0);

    Entry& slot_of(DOM::QualifiedName::Impl const* name)
    {
        // Fibonacci hashing: the high bits of the product depend on all of an allocation's address.
        auto hash = (reinterpret_cast<FlatPtr>(name) * 0x9e3779b97f4a7c15ull) >> (64 - count_trailing_zeroes(slot_count));
        return m_slots[hash];
    }

private:
    Array<Entry, slot_count> m_slots {};
};

// One batch of elements publishing their initial features. Nothing but the batch changes the DOM or what the engine
// reads while it lives, so what one element answers holds for the next one asking. The elements cross to the engine in
// blocks: their rows are written in one call, and what is published of them beside their rows follows the call.
class ElementPublication {
    AK_MAKE_NONCOPYABLE(ElementPublication);
    AK_MAKE_NONMOVABLE(ElementPublication);

public:
    // The facts an element takes from its parent where it has none of its own.
    struct Inherited {
        DOM::Element::Directionality directionality { DOM::Element::Directionality::Ltr };
        StyleAtomID language_atom;
        bool is_in_user_agent_shadow_tree { false };
    };

    ElementPublication(StyleEngine& style_engine, DOM::Document const& document)
        : m_style_engine(style_engine)
        , m_focus_within_chain(document)
        , m_hovered_element(hovered_element_of(document.hovered_node()))
        , m_ltr_atom(style_engine.intern_atom("ltr"_utf16_fly_string))
        , m_rtl_atom(style_engine.intern_atom("rtl"_utf16_fly_string))
    {
    }

    ~ElementPublication()
    {
        VERIFY(m_rows.is_empty());
        // NB: A tag read now may not be final, as an XML parser sets attributes after insertion, so none is left cached.
        for (auto element : m_elements_asked_their_language)
            element->invalidate_lang_value_of_inclusive_ancestors();
    }

    ElementKind const& kind_of(DOM::Element const& element)
    {
        auto& kind = m_kinds.slot_of(element.qualified_name_identity());
        if (kind.name != element.qualified_name_identity())
            kind = element_kind_of(m_style_engine, element);
        return kind;
    }

    PublishedAttributeName const& attribute_name(DOM::QualifiedName const& name)
    {
        auto& slot = m_attribute_names.slot_of(name.identity());
        if (slot.name == name.identity())
            return slot;
        auto atom = m_style_engine.intern_attribute_name(name.local_name(), name.namespace_());
        slot = {
            .name = name.identity(),
            .atom = atom,
            .is_known_unread = m_style_engine.attribute_is_known_unread(atom),
            .is_lang = name.local_name() == HTML::AttributeNames::lang,
            .is_dir = name.local_name() == HTML::AttributeNames::dir,
        };
        if (slot.is_known_unread)
            m_style_engine.note_attribute_withheld(name.local_name(), name.namespace_());
        return slot;
    }

    // What the element's parent passes on, if it has one. Elements come mostly in tree order, so most parents were
    // just published, and most of the rest are asked once for all the children that follow.
    Optional<Inherited> inherited_by(DOM::Element& element)
    {
        auto* parent = element.parent_or_shadow_host_element();
        while (!m_elements_to_parent.is_empty() && m_elements_to_parent.last().element.ptr() != parent)
            m_elements_to_parent.take_last();
        if (!parent)
            return {};
        if (m_elements_to_parent.is_empty()) {
            auto const* shadow_root = parent->containing_shadow_root().ptr();
            published(*parent, {
                                   .directionality = parent->directionality(),
                                   .language_atom = language_atom_asked_of(*parent),
                                   .is_in_user_agent_shadow_tree = shadow_root && shadow_root->is_user_agent_internal(),
                               });
        }
        return m_elements_to_parent.last().inherited;
    }

    void published(DOM::Element& element, Inherited inherited)
    {
        m_elements_to_parent.append({ element, inherited });
    }

    // The tag is computed afresh. One cached where the element used to be says nothing about where it arrives, as a
    // subtree that moves while detached is never reached by the walk a `lang` change runs.
    StyleAtomID language_atom_asked_of(DOM::Element& element)
    {
        element.invalidate_lang_value();
        auto const language = element.lang_view();
        m_elements_asked_their_language.append(element);
        return language.has_value() ? m_style_engine.intern_language_atom(*language) : StyleAtomID {};
    }

    // Where the next row's custom states, classes and attributes go, in that order.
    Vector<u32, 512>& atoms() { return m_atoms; }

    void append_row(DOM::Element& element, StyleEngineFFI::FfiElementArrivalRow const& row)
    {
        m_rows.append(row);
        m_elements.append(element);
    }

    bool block_is_full() const { return m_rows.size() >= rows_per_block; }

    // Writes the rows of the block, and answers its elements for what follows the rows.
    ReadonlySpan<GC::Ref<DOM::Element>> write_block()
    {
        m_style_engine.record_element_arrival_rows(m_rows.span(), m_atoms.span());
        m_rows.clear_with_capacity();
        m_atoms.clear_with_capacity();
        return m_elements.span();
    }

    void forget_block() { m_elements.clear_with_capacity(); }

    SelectorMatching::FocusWithinChain const& focus_within_chain() const { return m_focus_within_chain; }
    DOM::Element const* hovered_element() const { return m_hovered_element.ptr(); }
    StyleAtomID directionality_atom(DOM::Element::Directionality directionality) const
    {
        return directionality == DOM::Element::Directionality::Rtl ? m_rtl_atom : m_ltr_atom;
    }

private:
    static constexpr size_t rows_per_block = 256;

    struct AncestorRow {
        GC::Ptr<DOM::Element const> element;
        Inherited inherited;
    };

    StyleEngine& m_style_engine;
    SelectorMatching::FocusWithinChain m_focus_within_chain;
    GC::Ptr<DOM::Element const> m_hovered_element;
    StyleAtomID m_ltr_atom;
    StyleAtomID m_rtl_atom;
    TableByName<ElementKind, 64> m_kinds;
    TableByName<PublishedAttributeName, 32> m_attribute_names;
    GC::ConservativeVector<AncestorRow, 32> m_elements_to_parent;
    GC::RootVector<GC::Ref<DOM::Element>> m_elements_asked_their_language;
    Vector<StyleEngineFFI::FfiElementArrivalRow> m_rows;
    Vector<u32, 512> m_atoms;
    GC::RootVector<GC::Ref<DOM::Element>> m_elements;
};

// What the engine reads decides what of the elements is withheld from it, which the host asks the engine where a rule
// it queued since its last job leaves it not knowing.
static void learn_what_the_engine_reads(StyleEngine& style_engine, Layout::BegunRead const& read)
{
    StyleEngineFFI::document_host_learn_selector_requirements(style_engine.host(), &read);
}

// A subtree waiting to arrive, and whether its root arrives alone, its children waiting on in a display:none subtree.
struct PendingArrivalRoot {
    GC::Ref<DOM::Node> node;
    bool children_wait { false };
};

// Every node in the subtrees waiting to arrive, in tree order, takes its identity, and every element records its
// arrival. The subtrees are taken in together, so each identity is assigned before the first arrival is recorded:
// an arrival names its parent and siblings, and a sibling that waited in another subtree has arrived by then.
//
// With a publication, an element's features arrive with it, in one write of its row. An element not connected yet
// stands in the way of that, as its insertion steps are still running, and waits as it would without a publication.
static void record_subtree_arrivals(DOM::Document& document, ReadonlySpan<PendingArrivalRoot> roots, ElementPublication* publication)
{
    auto& style_computer = document.style_computer();
    auto& style_engine = style_computer.style_engine();
    struct Arrival {
        GC::Ref<DOM::Node> node;
        TreeScopeID tree_scope;
    };
    GC::ConservativeVector<Arrival, 64> arrivals;
    GC::RootVector<GC::Ref<DOM::Text>, 64> text_arrivals;
    // Elements and text nodes in tree order, which is the order their places in the DOM child sequence can be taken
    // in: each node's previous sibling has taken its place first.
    GC::RootVector<GC::Ref<DOM::Node>, 64> dom_order_arrivals;
    // A slot can take its identity after the elements assigned to it took theirs - a shadow tree built from markup
    // assigns before the slot connects - and a slottable's assignment is published as the slot's identity, so it was
    // published as no slot at all. Publishing it again once the slot has arrived is what lets `::slotted()` be
    // answered. An element taken in with the slot names the slot in its own arrival instead, so only the elements
    // that had an identity before this take-in are collected.
    GC::RootVector<GC::Ref<DOM::Element>> slottables_identified_earlier;
    size_t element_count = 0;
    auto collect = [&](DOM::Node& node, TreeScopeID tree_scope) {
        node.set_style_arrival_pending(false);
        node.set_descendant_style_arrival_pending(false);
        if (auto* element = as_if<DOM::Element>(node); element && element->style_node_id() == no_style_node) {
            // NB: An arrival reads its language through its parent, which may arrive after it, and whose answer an
            //     earlier read left cached from where the subtree stood.
            element->invalidate_lang_value();
            arrivals.append({ *element, tree_scope });
            dom_order_arrivals.append(*element);
            ++element_count;
            if (auto* slot = as_if<HTML::HTMLSlotElement>(*element)) {
                slot->assign_slottables_if_needed();
                for (auto const& slottable : slot->assigned_nodes_internal()) {
                    if (auto const* assigned = slottable.get_pointer<GC::Ref<DOM::Element>>(); assigned && (*assigned)->style_node_id() != no_style_node)
                        slottables_identified_earlier.append(*assigned);
                }
            }
        } else if (auto* shadow_root = as_if<DOM::ShadowRoot>(node); shadow_root && shadow_root->style_node_id() == no_style_node) {
            arrivals.append({ *shadow_root, tree_scope });
        } else if (auto* text = as_if<DOM::Text>(node); text && text->style_node_id() == no_style_node) {
            text_arrivals.append(*text);
            dom_order_arrivals.append(*text);
        }
    };
    for (auto const& root : roots) {
        ensure_dom_order_parent_identity(root.node->parent(), style_engine);
        if (root.children_wait) {
            collect(root.node, tree_scope_of(root.node->root()));
            root.node->set_child_style_arrivals_wait(true);
        } else {
            for_each_shadow_including_inclusive_descendant_with_scope(root.node, tree_scope_of(root.node->root()), collect);
        }
    }

    if (!text_arrivals.is_empty()) {
        Vector<StyleNodeID, 64> identities;
        identities.resize(text_arrivals.size());
        style_engine.mint_text_style_nodes(identities.span());
        style_computer.ensure_style_node_slot(identities.last());
        Vector<StyleEngineFFI::FfiArrivingText, 64> texts;
        texts.ensure_capacity(text_arrivals.size());
        for (size_t i = 0; i < text_arrivals.size(); ++i) {
            auto& text = *text_arrivals[i];
            text.set_style_node_id(identities[i]);
            style_computer.register_style_node(identities[i], text);
            texts.unchecked_append({
                .data = text.data().to_raw_leaked(),
                .node = identities[i].value(),
                .is_ascii_whitespace = text.data().is_ascii_whitespace(),
                .is_in_user_agent_shadow_tree = text_is_in_user_agent_shadow_tree(text),
                .is_password_input = text.is_password_input(),
            });
        }
        // The text nodes of the subtrees take their places in the engine in one write.
        StyleEngineFFI::style_engine_stage_text_arrivals(style_engine.host(), texts.span());
    }

    if (!arrivals.is_empty()) {
        Vector<StyleNodeID, 64> identities;
        identities.resize(arrivals.size());
        style_engine.mint_style_nodes(identities.span());
        style_computer.ensure_style_node_slot(identities.last());
        size_t next_element_identity = 0;
        size_t next_shadow_root_identity = element_count;
        for (size_t index = 0; index < arrivals.size(); ++index) {
            auto const& arrival = arrivals[index];
            if (index + nodes_ahead < arrivals.size())
                prefetch_node(*arrivals[index + nodes_ahead].node);
            if (auto* element = as_if<DOM::Element>(*arrival.node)) {
                auto identity = identities[next_element_identity++];
                element->set_style_node_id(identity);
                style_computer.register_style_node(identity, *element);
                Layout::publish_table_spans(*element);
                if (element->is_size_query_container() || element->style_depends_on_size_container_query())
                    element->publish_size_container_query_facts();
            } else {
                auto identity = identities[next_shadow_root_identity++];
                auto& shadow_root = as<DOM::ShadowRoot>(*arrival.node);
                shadow_root.set_style_node_id(identity);
                style_computer.register_style_node(identity, shadow_root);
                StyleEngineFFI::style_engine_set_tree_scope_root(style_engine.host(), arrival.tree_scope, identity);
            }
        }

        // The elements of the subtrees arrive in the engine's tree in one write, each standing in no tree before.
        Vector<StyleEngineFFI::FfiArrivingElement, 64> arriving_elements;
        for (size_t index = 0; index < arrivals.size(); ++index) {
            auto const& arrival = arrivals[index];
            prefetch_publication_ahead(index, arrivals.size(), [&](size_t index) { return arrivals[index].node.ptr(); });
            auto* element = as_if<DOM::Element>(*arrival.node);
            if (!element)
                continue;
            // A shadow root that took its identity before its host had one is still waiting to be linked to it. A sheet
            // adopted into a shadow tree names that root, so the root can be identified first, and the link is what lets
            // a `:host` or `::slotted()` rule in that tree reach the host instead of the document.
            if (auto shadow_root = element->shadow_root(); shadow_root && shadow_root->style_node_id() != no_style_node)
                StyleEngineFFI::style_engine_set_shadow_root(style_engine.host(), element->style_node_id(), shadow_root->style_node_id());
            StyleEngineFFI::FfiArrivingElement arriving {
                .unique_node_id = static_cast<u64>(element->unique_id().value()),
                .node = element->style_node_id().value(),
                .relations = relations_of(*element, style_engine, arrival.tree_scope),
            };
            if (publication && element->is_connected()) {
                style_engine.note_element_awaiting_first_style_computation(*element);
                publish_element_arrival(style_engine, *element, *publication, arriving);
            } else {
                arriving_elements.append(arriving);
                style_engine.defer_element_initial_features(*element);
            }
        }
        if (publication)
            finish_element_publication(style_engine, *publication);
        style_engine.record_element_arrivals(arriving_elements.span());
        for (auto const& slottable : slottables_identified_earlier)
            record_element_assigned_slot_changed(slottable, nullptr);
    }

    if (dom_order_arrivals.is_empty())
        return;
    Vector<u32, 192> links;
    links.ensure_capacity(dom_order_arrivals.size() * 3);
    for (size_t index = 0; index < dom_order_arrivals.size(); ++index) {
        if (index + nodes_ahead < dom_order_arrivals.size())
            prefetch_node(*dom_order_arrivals[index + nodes_ahead]);
        append_dom_order_link(links, dom_order_arrivals[index]);
    }
    StyleEngineFFI::style_engine_link_style_nodes_in_dom_order(style_engine.host(), links.span());

    republish_assigned_slots_of(dom_order_arrivals);

    // What the insertion steps publish about a node under its identity waited for the identity (see Node::inserted()).
    auto focused_area = document.focused_area();
    auto const* focused_text_control = is<HTML::FormAssociatedTextControlElement>(focused_area.ptr()) ? focused_area.ptr() : nullptr;
    for (size_t index = 0; index < dom_order_arrivals.size(); ++index) {
        auto const& node = dom_order_arrivals[index];
        if (index + nodes_ahead < dom_order_arrivals.size())
            prefetch_node(*dom_order_arrivals[index + nodes_ahead]);
        // Inertness, editability and the wheel handler state are inherited from the place the node arrived in.
        node->publish_dom_paint_facts();
        // As is being in the shadow tree of the focused text control.
        if (focused_text_control) {
            if (auto* shadow_root = as_if<DOM::ShadowRoot>(node->root()); shadow_root && shadow_root->host() == focused_text_control)
                Layout::publish_is_in_focused_text_control(*node);
        }
    }

    // The top layer is published whole, naming only the members that have arrived.
    if (any_of(arrivals, [](auto const& arrival) { auto* element = as_if<DOM::Element>(*arrival.node); return element && element->in_top_layer(); }))
        record_top_layer_changed(document);

    // The insertion that connected a subtree marked it for the layout tree build under the identity it did not have
    // yet, so the mark is made here, as the insertion would have made it.
    for (auto const& root : roots) {
        auto const* parent = root.node->parent();
        if (parent && (parent->is_html_style_element() || parent->is_svg_style_element()) && !parent->may_have_layout_box())
            continue;
        root.node->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::NodeInsertBefore);
    }
}

// Whether the element's parent is in a display:none subtree an animation cannot be holding visible: it or an ancestor
// holds a display:none style and runs no animation. The element's own display does not hide its children from
// selectors its siblings match, such as `:empty + *`, so it is not the one asked about.
static bool parent_is_in_display_none_subtree_no_animation_reveals(DOM::Element const& element)
{
    for (auto const* ancestor = element.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        auto const* ancestor_element = as_if<DOM::Element>(*ancestor);
        if (!ancestor_element)
            continue;
        if (ancestor_element->installed_style().display_is_none())
            return !ancestor_element->has_associated_animations();
    }
    return false;
}

// Whether every descendant of the element can wait to arrive.
static bool descendants_can_wait_to_arrive(DOM::Element& element, StyleEngine::RelationalArrivalKeys const& keys)
{
    if (element.associated_animation_count_in_subtree() != 0)
        return false;
    bool can_wait = true;
    element.for_each_shadow_including_descendant([&](DOM::Node& node) {
        auto* descendant = as_if<DOM::Element>(node);
        if (!descendant || element_can_wait_to_arrive(*descendant, keys))
            return TraversalDecision::Continue;
        can_wait = false;
        return TraversalDecision::Break;
    });
    return can_wait;
}

// Decides, along one take-in, which elements arrive alone in a display:none subtree, their children waiting.
class HiddenChildArrivalDecision {
public:
    HiddenChildArrivalDecision(DOM::Document& document, HiddenChildArrivals hidden_child_arrivals)
        : m_document(document)
        , m_style_engine(document.style_computer().style_engine())
        , m_hidden_child_arrivals(hidden_child_arrivals)
    {
    }

    ~HiddenChildArrivalDecision()
    {
        if (m_keys)
            m_style_engine.set_waiting_children_checked_against_keys_version(m_keys->version);
    }

    // Whether the children of `node`, which arrives now, can wait.
    bool children_of_arrival_can_wait(DOM::Node& node)
    {
        auto* element = as_if<DOM::Element>(node);
        if (!element || !may_wait() || !element->has_children() || element->shadow_root() || is<HTML::HTMLSlotElement>(*element)
            || !parent_is_in_display_none_subtree_no_animation_reveals(*element))
            return false;
        return descendants_can_wait_to_arrive(*element, keys());
    }

    // Whether the children of `element`, which wait, can go on waiting.
    bool waiting_children_can_go_on_waiting(DOM::Element& element)
    {
        if (!may_wait() || element.shadow_root() || !parent_is_in_display_none_subtree_no_animation_reveals(element))
            return false;
        // The children were looked at against these keys, as was whatever joined them or changed a name or state of
        // theirs since.
        if (m_style_engine.waiting_children_checked_against_keys_version() == keys().version)
            return true;
        return descendants_can_wait_to_arrive(element, keys());
    }

private:
    bool may_wait() const { return m_hidden_child_arrivals == HiddenChildArrivals::MayWait; }

    StyleEngine::RelationalArrivalKeys const& keys()
    {
        if (!m_keys)
            m_keys = &m_style_engine.relational_arrival_keys(Layout::ForcedReadScope { *m_document });
        return *m_keys;
    }

    GC::Ref<DOM::Document> m_document;
    StyleEngine& m_style_engine;
    HiddenChildArrivals m_hidden_child_arrivals;
    StyleEngine::RelationalArrivalKeys const* m_keys { nullptr };
};

// The subtrees waiting to arrive, in tree order, found along the marks their ancestors carry, and whether anything
// still waits below the node once they arrive. The marks are cleared on the way, and kept on the path to children
// that go on waiting in a display:none subtree: a subtree that left the tree before this walk leaves marks behind that
// nothing waits under.
static bool collect_pending_style_arrival_roots(DOM::Node& node, Vector<PendingArrivalRoot, 16>& roots, HiddenChildArrivalDecision& decision)
{
    bool waits_below = false;
    auto visit = [&](DOM::Node& child) {
        if (auto* element = as_if<DOM::Element>(child); element && element->child_style_arrivals_wait()) {
            if (decision.waiting_children_can_go_on_waiting(*element)) {
                waits_below = true;
                return;
            }
            release_waiting_children(*element);
        }
        if (child.style_arrival_pending()) {
            auto children_wait = decision.children_of_arrival_can_wait(child);
            roots.append({ child, children_wait });
            waits_below |= children_wait;
        } else if (child.descendant_style_arrival_pending()) {
            waits_below |= collect_pending_style_arrival_roots(child, roots, decision);
        }
    };
    if (auto* element = as_if<DOM::Element>(node); element && element->shadow_root())
        visit(*element->shadow_root());
    for (auto* child = node.first_child(); child; child = child->next_sibling())
        visit(*child);
    node.set_descendant_style_arrival_pending(waits_below);
    return waits_below;
}

// OPTIMIZATION: The subtrees waiting to arrive, found among the nodes marked since the last take-in rather than along the
//               marks, which reach every child of each node on the path to one. Answers false, having found nothing,
//               where the marks have to be walked: a node is in a shadow tree, whose tree order the walk follows.
static bool take_listed_arrival_roots(DOM::Document& document, GC::RootVector<GC::Ref<DOM::Node>> const& listed, GC::ConservativeVector<PendingArrivalRoot, 16>& roots, HiddenChildArrivalDecision& decision)
{
    GC::RootVector<GC::Ref<DOM::Node>> found;
    GC::RootHashTable<GC::Ref<DOM::Node>> seen;
    for (auto node : listed) {
        if (!node->is_connected() || !node->style_arrival_pending() || &node->document() != &document || seen.set(node) != HashSetResult::InsertedNewEntry)
            continue;
        if (is<DOM::ShadowRoot>(node->root()))
            return false;
        // A node in a subtree waiting to arrive arrives with it.
        bool in_waiting_subtree = false;
        for (auto* ancestor = node->parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
            if (ancestor->style_arrival_pending()) {
                in_waiting_subtree = true;
                break;
            }
        }
        if (!in_waiting_subtree)
            found.append(node);
    }
    quick_sort(found, [](auto const& a, auto const& b) {
        return (a->compare_document_position(b) & DOM::Node::DOCUMENT_POSITION_FOLLOWING) != 0;
    });
    // The marks on the path to each subtree are cleared, and kept on the path to one whose children go on waiting.
    GC::RootVector<GC::Ref<DOM::Node>> waiting;
    for (auto node : found) {
        auto children_wait = decision.children_of_arrival_can_wait(node);
        roots.append({ node, children_wait });
        if (children_wait)
            waiting.append(node);
        for (auto* ancestor = node->parent_or_shadow_host(); ancestor && ancestor->descendant_style_arrival_pending(); ancestor = ancestor->parent_or_shadow_host())
            ancestor->set_descendant_style_arrival_pending(false);
    }
    for (auto node : waiting) {
        for (auto* ancestor = node->parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host())
            ancestor->set_descendant_style_arrival_pending(true);
    }
    document.style_computer().style_engine().set_may_have_waiting_children(!waiting.is_empty());
    return true;
}

static void take_in_pending_style_arrivals(DOM::Document& document, HiddenChildArrivals hidden_child_arrivals, Layout::BegunRead const* read)
{
    auto& style_engine = document.style_computer().style_engine();
    if (style_engine.is_taking_in_pending_arrivals())
        return;
    // Nodes that waited and left the tree again are counted too, and nothing is left of them to take in.
    auto arrivals_were_noted = style_engine.has_pending_arrivals();
    style_engine.forget_pending_arrivals();
    if (!document.descendant_style_arrival_pending()) {
        (void)style_engine.take_listed_arrivals();
        return;
    }
    // Children that wait in a display:none subtree go on waiting until a node connects, something notes they may have
    // to arrive, or the keys they were looked at against move. Until then, the marks on the path to them lead to nothing
    // that has to arrive.
    if (!arrivals_were_noted && hidden_child_arrivals == HiddenChildArrivals::MayWait
        && style_engine.waiting_children_checked_against_keys_version() == style_engine.relational_arrival_keys(Layout::ForcedReadScope { document }).version)
        return;
    // Only the arrivals of a tracked tree record which children wait.
    if (!document.style_engine_tracks_tree())
        hidden_child_arrivals = HiddenChildArrivals::TakeIn;
    // NB: Publication can bring another document up to date, as an SVG image's for its natural size, whose own take-in
    //     this says nothing about.
    auto taking_in = style_engine.begin_taking_in_pending_arrivals();
    GC::ConservativeVector<PendingArrivalRoot, 16> roots;
    {
        HiddenChildArrivalDecision decision { document, hidden_child_arrivals };
        auto listed = style_engine.take_listed_arrivals();
        if (!listed.has_value() || style_engine.may_have_waiting_children() || !take_listed_arrival_roots(document, *listed, roots, decision))
            style_engine.set_may_have_waiting_children(collect_pending_style_arrival_roots(document, roots, decision));
    }
    if (!roots.is_empty() && document.style_engine_tracks_tree()) {
        if (read) {
            learn_what_the_engine_reads(style_engine, *read);
            ElementPublication publication { style_engine, document };
            record_subtree_arrivals(document, roots, &publication);
        } else {
            record_subtree_arrivals(document, roots, nullptr);
        }
        // NB: An element shows a highlight only once it has an identity to be named by, which a hidden child can take
        //     without the tree changing.
        document.set_needs_highlight_style_update(PseudoElement::Selection);
        document.set_needs_highlight_style_update(PseudoElement::SearchText);
        document.set_needs_highlight_style_update(PseudoElement::SpellingError);
    }
}

void take_in_pending_style_arrivals(DOM::Document& document, HiddenChildArrivals hidden_child_arrivals)
{
    take_in_pending_style_arrivals(document, hidden_child_arrivals, nullptr);
}

void take_in_and_publish_pending_style_arrivals(DOM::Document& document, HiddenChildArrivals hidden_child_arrivals, Layout::BegunRead const& read)
{
    take_in_pending_style_arrivals(document, hidden_child_arrivals, &read);
}

void record_element_associated_with_animation(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    // An animated element keeps its style while hidden, so it cannot wait to arrive itself.
    if (element.style_node_id() == no_style_node)
        release_waiting_children_holding(element, *style_engine);
    // An animation can hold a display:none element visible, revealing whatever waited to arrive below it.
    if (element.child_style_arrivals_wait() || element.descendant_style_arrival_pending())
        note_waiting_children_may_arrive(*style_engine, element.document());
}

// The box the element asks layout for.
CSS::ElementBoxKind box_kind_for_layout(DOM::Element const& element)
{
    // https://svgwg.org/svg2-draft/struct.html#ConditionalProcessing
    // NB: An element whose conditional processing attributes evaluate to false, and its children, are skipped and thus
    //     will not be rendered.
    if (auto const* svg_graphics_element = as_if<SVG::SVGGraphicsElement>(element); svg_graphics_element && !svg_graphics_element->conditional_processing_attributes_evaluate_to_true())
        return CSS::ElementBoxKind::NoBox;
    return element.box_kind();
}

void record_waiting_children_may_be_revealed(DOM::Element& element)
{
    if (auto* style_engine = style_engine_for(element))
        note_waiting_children_may_arrive(*style_engine, element.document());
}

// The element-backed pseudo-element an element in its host's shadow tree stands for, as one plus
// its kind, or zero.
static u8 associated_pseudo_kind_plus_one(DOM::Element const& element)
{
    auto pseudo_element = element.associated_shadow_host_pseudo_element();
    return pseudo_element.has_value() ? static_cast<u8>(to_underlying(*pseudo_element) + 1) : 0;
}

// Publish every selector-visible fact intrinsic to one element.
//
// An element arrives with facts already true of it, and the engine has heard none of them. They are
// published as ordinary deltas from absent, so the projection learns them by exactly the same path
// later mutations take rather than by a second one.
static void publish_element_selector_features(StyleEngine& style_engine, DOM::Element& element, ElementPublication& publication, Optional<StyleEngineFFI::FfiArrivingElement const&> arriving)
{
    auto node = element.style_node_id();
    // Everything the element's selectors test arrives as one row, which the engine writes in one go.
    // Slot identity and namespace never change during an element's lifetime.
    auto const& kind = publication.kind_of(element);
    StyleAtomID id_atom;
    if (auto const& id = element.id(); id.has_value())
        id_atom = intern_id_or_class_atom(style_engine, element, *id);
    auto& atoms = publication.atoms();
    auto atoms_before = atoms.size();
    if (auto states = element.custom_state_set()) {
        for (auto const& state : states->states())
            atoms.append(style_engine.intern_atom(state).value());
    }
    auto custom_state_count = atoms.size() - atoms_before;
    for (auto const& class_name : element.class_names())
        atoms.append(intern_id_or_class_atom(style_engine, element, class_name).value());
    auto class_count = atoms.size() - atoms_before - custom_state_count;
    bool has_own_language = false;
    bool has_own_directionality = has_flag(kind.open_facts, ElementOpenFacts::OwnDirectionality);
    for (auto const& [name, value] : element.attribute_list()) {
        auto const& attribute_name = publication.attribute_name(name);
        has_own_language |= attribute_name.is_lang;
        has_own_directionality |= attribute_name.is_dir;
        if (attribute_name.is_known_unread)
            continue;
        atoms.append(attribute_name.atom.value());
        atoms.append(style_engine.intern_attribute_value(attribute_name.atom, value).value());
    }
    auto attribute_count = (atoms.size() - atoms_before - custom_state_count - class_count) / 2;

    bool has_nonempty_text_child = false;
    for (GC::Ptr<DOM::Node const> child = element.first_child(); child; child = child->next_sibling()) {
        if (GC::Ptr<DOM::Text const> text = as_if<DOM::Text>(*child); text && !text->data().is_empty()) {
            has_nonempty_text_child = true;
            break;
        }
    }

    auto ask_local_link = SelectorMatching::AskLocalLink::Yes;
    if (has_flag(kind.state_questions, SelectorMatching::ElementStateQuestions::Link) && element.matches_link_pseudo_class() && style_engine.local_link_is_known_untested()) {
        ask_local_link = SelectorMatching::AskLocalLink::No;
        style_engine.note_local_link_state_withheld();
    }
    auto ask_validity = SelectorMatching::AskValidity::Yes;
    // A custom element's definition can make it a control, whatever its name settles.
    if ((has_flag(kind.state_questions, SelectorMatching::ElementStateQuestions::Control) || element.has_custom_element_definition())
        && SelectorMatching::element_can_have_validity(element) && style_engine.validity_is_known_untested()) {
        ask_validity = SelectorMatching::AskValidity::No;
        style_engine.note_validity_state_withheld();
    }
    auto states = SelectorMatching::element_states(element, kind.state_questions, publication.focus_within_chain(), ask_local_link, ask_validity);
    u64 state_bits = 0;
    states.for_each_set([&](PseudoClass pseudo_class) {
        if (auto fact = state_fact_for(pseudo_class); fact.has_value())
            state_bits |= static_cast<u64>(1) << to_underlying(*fact);
    });

    auto inherited = publication.inherited_by(element);
    ElementPublication::Inherited own;
    own.language_atom = inherited.has_value() && !has_own_language ? inherited->language_atom : publication.language_atom_asked_of(element);
    auto directionality = has_own_directionality ? element.directionality_unless_from_parent() : Optional<DOM::Element::Directionality> {};
    own.directionality = directionality.value_or_lazy_evaluated([&] {
        return inherited.has_value() ? inherited->directionality : element.directionality();
    });
    if (auto const* shadow_root = as_if<DOM::ShadowRoot>(element.parent())) {
        own.is_in_user_agent_shadow_tree = shadow_root->is_user_agent_internal();
    } else if (inherited.has_value()) {
        own.is_in_user_agent_shadow_tree = inherited->is_in_user_agent_shadow_tree;
    } else {
        auto const* containing_shadow_root = element.containing_shadow_root().ptr();
        own.is_in_user_agent_shadow_tree = containing_shadow_root && containing_shadow_root->is_user_agent_internal();
    }
    publication.published(element, own);

    u8 heading_level = 0;
    if (has_flag(kind.open_facts, ElementOpenFacts::Heading))
        heading_level = static_cast<u8>(min(as<HTML::HTMLHeadingElement>(element).heading_level(), 255u));

    publication.append_row(element, {
                                        .arrival = {
                                            .states = state_bits,
                                            .node = node.value(),
                                            .tag_atom = kind.tag_atom.value(),
                                            .folded_tag_atom = kind.folded_tag_atom.value(),
                                            .id_atom = id_atom.value(),
                                            .namespace_atom = kind.namespace_atom.value(),
                                            .language_atom = own.language_atom.value(),
                                            .directionality_atom = publication.directionality_atom(own.directionality).value(),
                                            .custom_state_count = static_cast<u32>(custom_state_count),
                                            .class_count = static_cast<u32>(class_count),
                                            .attribute_count = static_cast<u32>(attribute_count),
                                            .heading_level = heading_level,
                                            .is_slot = kind.is_slot,
                                            .has_text_content = has_nonempty_text_child,
                                            .associated_pseudo_kind_plus_one = associated_pseudo_kind_plus_one(element),
                                            .box_kind = to_underlying(box_kind_for_layout(element)),
                                            .adjustment_facts = kind.adjustment_facts | open_style_adjustment_facts(element, kind.open_facts),
                                            .construction_facts = kind.construction_facts | open_construction_facts(element, kind.open_facts, own.is_in_user_agent_shadow_tree),
                                        },
                                        .unique_node_id = arriving.has_value() ? arriving->unique_node_id : 0,
                                        .relations = arriving.has_value() ? arriving->relations : detached_relations(),
                                        .has_relations = arriving.has_value(),
                                    });
}

// An element's hints move when something beside its own attributes that they are mapped from
// moves: the table a cell is under, the <source> an image takes its dimensions from, the body's
// link colours for a link, an input entering or leaving the Image Button state. Publish them again.
void republish_presentational_hints(DOM::Element& element)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;
    StyleComputer::collect_presentational_hint_properties({ element });
    // The hints moved, and so does the style they are cascaded into.
    record_element_declarations_changed(element, ElementDeclarationKind::SvgPresentationAttribute, true, true);
}

static bool element_has_presentational_hints_to_publish(DOM::Element const& element, ElementKind const& kind)
{
    if (has_flag(kind.open_facts, ElementOpenFacts::PresentationalHints)) {
        if (element.publishes_presentational_hints_on_arrival())
            return true;
        // A table cell can take hints from its table's border and cellpadding, and an image from its
        // <picture>'s <source>, without a presentational attribute of its own.
        if (element.namespace_uri() == Namespace::HTML && first_is_one_of(element.local_name(), HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::img))
            return true;
        // The cascade also reads the width and height attributes of an element that supports them.
        if (element.supports_dimension_attributes()
            && (element.has_attribute(HTML::AttributeNames::width) || element.has_attribute(HTML::AttributeNames::height)))
            return true;
        // A body takes its margins from its frame's marginwidth and marginheight.
        if (is<HTML::HTMLBodyElement>(element)) {
            if (auto navigable = element.document().navigable(); navigable && navigable->container()
                && (navigable->container()->has_attribute(HTML::AttributeNames::marginwidth) || navigable->container()->has_attribute(HTML::AttributeNames::marginheight)))
                return true;
        }
    }
    // A body's link, vlink and alink attributes are presentational hints on every link, by the
    // link's :link, :visited and :active state.
    if (has_flag(kind.state_questions, SelectorMatching::ElementStateQuestions::Link)
        && (element.matches_link_pseudo_class() || element.matches_visited_pseudo_class())
        && (element.document().normal_link_color().has_value() || element.document().visited_link_color().has_value() || element.document().active_link_color().has_value()))
        return true;
    return any_of(element.attribute_list(), [&](auto const& attribute) { return element.is_presentational_hint(attribute.name.as_string()); });
}

// The element facts the style computation's box-type transformation and element style adjustments
// read of the DOM. Mirrors Rust `element_adjustment_fact`.
enum ElementStyleAdjustmentFact : u32 {
    IsBr = 1 << 0,
    IsWbr = 1 << 1,
    DisallowDisplayContents = 1 << 2,
    RewriteInlineFlow = 1 << 3,
    IsButton = 1 << 4,
    ForceLineHeightNormal = 1 << 5,
    CheckInputLineHeight = 1 << 6,
    HideAudioWithoutControls = 1 << 7,
    IsTable = 1 << 8,
    ForcePositionStatic = 1 << 9,
    ForceSymbolDisplayInline = 1 << 10,
    IsMathML = 1 << 11,
    IsMathMLMtable = 1 << 12,
    IsMathMLMtr = 1 << 13,
    IsMathMLMtd = 1 << 14,
    IsTh = 1 << 15,
    IsDocumentElement = 1 << 16,
    HasAnimations = 1 << 17,
    // An SVG graphics element folds its own transform into its SVG container's layout, which the
    // style engine's damage for the element's record moves reads.
    IsSvgGraphicsElement = 1 << 18,
    // The element stands for an element-reference pseudo-element of its shadow host, whose style
    // it takes.
    IsShadowHostPseudoElement = 1 << 19,
    // An HTML <body>. The first one among an HTML <html> root's children propagates its style to the
    // viewport, which layout and the style engine's damage for the element's record moves read.
    IsHtmlBodyElement = 1 << 20,
    // The element types layout tree construction branches on. An element's type is fixed when it is
    // created, so the store holds these rather than the tree builder asking the DOM for them.
    IsSvgElement = 1 << 21,
    IsSvgSwitchElement = 1 << 22,
    IsSvgContainer = 1 << 23,
    RequiresSvgContainer = 1 << 24,
    IsSvgForeignObjectElement = 1 << 25,
    IsSvgMaskElement = 1 << 26,
    IsSvgClipPathElement = 1 << 27,
    IsSvgPatternElement = 1 << 28,
    // Whether the element is rendered in the top layer. Unlike the type facts above it moves during
    // the element's lifetime, and every move is recorded where the element's flag is set.
    RenderedInTopLayer = 1 << 29,
    // An HTML <html>, whose first <body> child propagates its overflow to the viewport when it is
    // the root.
    IsHtmlHtmlElement = 1 << 30,
    // An HTML <frameset>, which is the document's body in place of a <body>.
    IsHtmlFramesetElement = 1u << 31,
};
// What a layout row records about the element it is built for at the moment it is allocated, published so that the
// tree build can read it out of the mirror rather than off the DOM node. Mirrors Rust `element_construction_fact`.
enum ElementConstructionFact : u32 {
    IsHtmlInputElement = 1 << 0,
    // This and ConstructedAsDocumentElement are also ElementStyleAdjustmentFact bits. A row is built out of this word
    // alone, so they are published into both rather than read across two.
    ConstructedAsHtmlHtmlElement = 1 << 1,
    IsInUserAgentShadowTree = 1 << 2,
    UsesButtonLayout = 1 << 3,
    IsEditingHost = 1 << 4,
    IsBody = 1 << 5,
    ConstructedAsDocumentElement = 1 << 6,
    IsSvgResourceElement = 1 << 7,
    IsSvgGeometryElement = 1 << 8,
    IsHtmlSelectElement = 1 << 9,
    // The layout algorithm a MathML element's tag name selects, as a MathMLLayoutKind in these bits.
    MathMLLayoutKindShift = 16,
    MathMLLayoutKindMask = 0x1f << MathMLLayoutKindShift,
};

// The layout algorithm a MathML element's tag name selects when its display is block math or inline math.
// Mirrors Rust `MathLayoutKind`.
enum class MathMLLayoutKind : u32 {
    NotMathML,
    Mrow,
    Math,
    Token,
    Mo,
    Mspace,
    Mfrac,
    Msqrt,
    Mroot,
    Msub,
    Msup,
    Msubsup,
    Munder,
    Mover,
    Munderover,
    Mmultiscripts,
    Mprescripts,
    None,
    Mpadded,
    Mtext,
};

static MathMLLayoutKind mathml_layout_kind(DOM::Element const& element)
{
    if (element.namespace_uri() != Namespace::MathML)
        return MathMLLayoutKind::NotMathML;
    namespace TagNames = MathML::TagNames;
    auto const& name = element.local_name();
    if (name == TagNames::math)
        return MathMLLayoutKind::Math;
    if (name == TagNames::mtext)
        return MathMLLayoutKind::Mtext;
    if (first_is_one_of(name, TagNames::mi, TagNames::mn, TagNames::ms, TagNames::annotation, TagNames::annotation_xml))
        return MathMLLayoutKind::Token;
    if (name == TagNames::mo)
        return MathMLLayoutKind::Mo;
    if (name == TagNames::mspace)
        return MathMLLayoutKind::Mspace;
    if (name == TagNames::mfrac)
        return MathMLLayoutKind::Mfrac;
    if (name == TagNames::msqrt)
        return MathMLLayoutKind::Msqrt;
    if (name == TagNames::mroot)
        return MathMLLayoutKind::Mroot;
    if (name == TagNames::msub)
        return MathMLLayoutKind::Msub;
    if (name == TagNames::msup)
        return MathMLLayoutKind::Msup;
    if (name == TagNames::msubsup)
        return MathMLLayoutKind::Msubsup;
    if (name == TagNames::munder)
        return MathMLLayoutKind::Munder;
    if (name == TagNames::mover)
        return MathMLLayoutKind::Mover;
    if (name == TagNames::munderover)
        return MathMLLayoutKind::Munderover;
    if (name == TagNames::mmultiscripts)
        return MathMLLayoutKind::Mmultiscripts;
    if (name == TagNames::mprescripts)
        return MathMLLayoutKind::Mprescripts;
    if (name == "none"sv)
        return MathMLLayoutKind::None;
    if (name == TagNames::mpadded)
        return MathMLLayoutKind::Mpadded;
    // https://w3c.github.io/mathml-core/#new-display-math-value
    // Unknown MathML elements behave the same as the <mrow> element.
    return MathMLLayoutKind::Mrow;
}
static ElementOpenFacts element_open_facts(DOM::Element const& element)
{
    bool is_html_element = element.namespace_uri() == Namespace::HTML;
    bool is_svg_element = element.namespace_uri() == Namespace::SVG;
    auto const& local_name = element.local_name();
    auto facts = ElementOpenFacts::None;
    auto set = [&](bool condition, ElementOpenFacts fact) {
        if (condition)
            facts |= fact;
    };
    auto is_input = is<HTML::HTMLInputElement>(element);
    auto is_body = is<HTML::HTMLBodyElement>(element);
    set(is_input, ElementOpenFacts::Input);
    set(is_html_element && local_name == HTML::TagNames::audio, ElementOpenFacts::Audio);
    set(is_svg_element && local_name == "svg"sv, ElementOpenFacts::SvgRoot);
    set(is_svg_element && local_name == "symbol"sv, ElementOpenFacts::SvgSymbol);
    set(is<SVG::SVGElement>(element), ElementOpenFacts::SvgElement);
    set(is_body || is<HTML::HTMLFrameSetElement>(element), ElementOpenFacts::Body);
    set(is<HTML::HTMLHeadingElement>(element), ElementOpenFacts::Heading);
    set(is_input || is<HTML::HTMLTextAreaElement>(element) || is<HTML::HTMLImageElement>(element) || is<HTML::HTMLObjectElement>(element) || is<HTML::HTMLVideoElement>(element)
            || is<SVG::SVGImageElement>(element) || is<HTML::HTMLCanvasElement>(element),
        ElementOpenFacts::ReplacedContent);
    set(is_input || local_name == HTML::TagNames::bdi, ElementOpenFacts::OwnDirectionality);
    set(is_body || is_input || element.publishes_presentational_hints_on_arrival() || element.supports_dimension_attributes()
            || (is_html_element && first_is_one_of(local_name, HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::img)),
        ElementOpenFacts::PresentationalHints);
    return facts;
}

// The box-type adjustment facts every element of a name shares.
static u32 kind_box_type_adjustment_facts(DOM::Element const& element)
{
    bool is_html_element = element.namespace_uri() == Namespace::HTML;
    bool is_svg_element = element.namespace_uri() == Namespace::SVG;
    bool is_mathml_element = element.namespace_uri() == Namespace::MathML;
    auto local_name = element.local_name();

    bool display_contents_computes_to_none = false;

    // https://drafts.csswg.org/css-display-3/#unbox-svg
    // - All other SVG container elements that are also renderable elements
    // - SVG text content child elements
    // - <use>
    //     display: contents strips the element from the formatting tree, and hoists its contents up to display in its
    //     place. These contents include the shadow-DOM content for use.
    // - any other SVG elements
    //     display: contents computes to display: none.
    display_contents_computes_to_none |= is_svg_element
        && !first_is_one_of(local_name, "a"sv, "g"sv, "svg"sv, "switch"sv, "textPath"sv, "tspan"sv, "use"sv);

    // https://drafts.csswg.org/css-display-3/#unbox-mathml
    // For all MathML elements, display: contents computes to display: none.
    display_contents_computes_to_none |= is_mathml_element;

    u32 facts = 0;
    auto set = [&](bool condition, ElementStyleAdjustmentFact fact) {
        if (condition)
            facts |= fact;
    };
    set(is<HTML::HTMLBRElement>(element), ElementStyleAdjustmentFact::IsBr);
    set(is_html_element && local_name == HTML::TagNames::wbr, ElementStyleAdjustmentFact::IsWbr);
    set(display_contents_computes_to_none || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::canvas, HTML::TagNames::object, HTML::TagNames::iframe, HTML::TagNames::progress, HTML::TagNames::embed, HTML::TagNames::frame, HTML::TagNames::meter, HTML::TagNames::frameset, HTML::TagNames::img)), ElementStyleAdjustmentFact::DisallowDisplayContents);
    set(is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::select), ElementStyleAdjustmentFact::RewriteInlineFlow);
    set(is_html_element && local_name == HTML::TagNames::button, ElementStyleAdjustmentFact::IsButton);
    set(is_html_element && local_name == HTML::TagNames::select, ElementStyleAdjustmentFact::ForceLineHeightNormal);
    set(is_html_element && local_name == HTML::TagNames::table, ElementStyleAdjustmentFact::IsTable);
    set(is_svg_element && local_name != "svg"sv, ElementStyleAdjustmentFact::ForcePositionStatic);
    set(is_mathml_element, ElementStyleAdjustmentFact::IsMathML);
    set(local_name.equals_ignoring_ascii_case("mtable"sv), ElementStyleAdjustmentFact::IsMathMLMtable);
    set(local_name.equals_ignoring_ascii_case("mtr"sv), ElementStyleAdjustmentFact::IsMathMLMtr);
    set(local_name.equals_ignoring_ascii_case("mtd"sv), ElementStyleAdjustmentFact::IsMathMLMtd);
    set(is_html_element && local_name.equals_ignoring_ascii_case(HTML::TagNames::th), ElementStyleAdjustmentFact::IsTh);
    return facts;
}

// The box-type adjustment facts that hang on the element itself or on where it stands.
static u32 open_box_type_adjustment_facts(DOM::Element const& element, ElementOpenFacts open_facts)
{
    u32 facts = 0;
    auto set = [&](bool condition, ElementStyleAdjustmentFact fact) {
        if (condition)
            facts |= fact;
    };

    if (has_flag(open_facts, ElementOpenFacts::Input)) {
        auto const& input = static_cast<HTML::HTMLInputElement const&>(element);
        bool input_allows_adjustment = !first_is_one_of(
            input.type_state(),
            HTML::HTMLInputElement::TypeAttributeState::Hidden,
            HTML::HTMLInputElement::TypeAttributeState::SubmitButton,
            HTML::HTMLInputElement::TypeAttributeState::Button,
            HTML::HTMLInputElement::TypeAttributeState::ResetButton,
            HTML::HTMLInputElement::TypeAttributeState::ImageButton,
            HTML::HTMLInputElement::TypeAttributeState::Checkbox,
            HTML::HTMLInputElement::TypeAttributeState::RadioButton);
        set(input_allows_adjustment, ElementStyleAdjustmentFact::DisallowDisplayContents);
        set(input_allows_adjustment, ElementStyleAdjustmentFact::RewriteInlineFlow);
        set(input_allows_adjustment && input.is_single_line(), ElementStyleAdjustmentFact::CheckInputLineHeight);
    }

    if (has_flag(open_facts, ElementOpenFacts::SvgRoot)) {
        bool is_outermost_svg_element = true;
        for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
            if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "foreignObject"sv)
                break;
            if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "svg"sv) {
                is_outermost_svg_element = false;
                break;
            }
        }
        set(!is_outermost_svg_element, ElementStyleAdjustmentFact::ForcePositionStatic);

        // https://drafts.csswg.org/css-display-3/#unbox-svg
        // - An svg element that has CSS box layout (this includes all svg whose parent is an HTML element, as well as
        //   document root elements):
        //     display: contents computes to display: none.
        set(is_outermost_svg_element, ElementStyleAdjustmentFact::DisallowDisplayContents);
    }

    if (has_flag(open_facts, ElementOpenFacts::SvgSymbol)) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(element.parent())) {
            if (auto* host = shadow_root->host())
                set(host->namespace_uri() == Namespace::SVG && host->local_name() == "use"sv, ElementStyleAdjustmentFact::ForceSymbolDisplayInline);
        }
    }

    set(has_flag(open_facts, ElementOpenFacts::Audio) && !element.has_attribute(HTML::AttributeNames::controls), ElementStyleAdjustmentFact::HideAudioWithoutControls);
    set(element.is_document_element(), ElementStyleAdjustmentFact::IsDocumentElement);
    return facts;
}

u32 element_box_type_adjustment_facts(DOM::Element const& element)
{
    return kind_box_type_adjustment_facts(element) | open_box_type_adjustment_facts(element, element_open_facts(element));
}

// The construction facts every element of a name shares.
static u32 kind_construction_facts(DOM::Element const& element)
{
    u32 facts = 0;
    auto set = [&](bool condition, ElementConstructionFact fact) {
        if (condition)
            facts |= fact;
    };
    auto const* html_element = as_if<HTML::HTMLElement>(element);
    auto is_input = is<HTML::HTMLInputElement>(element);
    set(is_input, ElementConstructionFact::IsHtmlInputElement);
    set(element.is_html_html_element(), ElementConstructionFact::ConstructedAsHtmlHtmlElement);
    set(!is_input && html_element && html_element->uses_button_layout(), ElementConstructionFact::UsesButtonLayout);
    set(element.is_svg_geometry_element(), ElementConstructionFact::IsSvgGeometryElement);
    set(is<HTML::HTMLSelectElement>(element), ElementConstructionFact::IsHtmlSelectElement);
    facts |= to_underlying(mathml_layout_kind(element)) << ElementConstructionFact::MathMLLayoutKindShift;
    return facts;
}

// The construction facts that hang on the element itself or on where it stands.
static u32 open_construction_facts(DOM::Element const& element, ElementOpenFacts open_facts, bool is_in_user_agent_shadow_tree)
{
    u32 facts = 0;
    auto set = [&](bool condition, ElementConstructionFact fact) {
        if (condition)
            facts |= fact;
    };
    set(is_in_user_agent_shadow_tree, ElementConstructionFact::IsInUserAgentShadowTree);
    set(has_flag(open_facts, ElementOpenFacts::Input) && static_cast<HTML::HTMLInputElement const&>(element).uses_button_layout(), ElementConstructionFact::UsesButtonLayout);
    set(element.may_be_editable_or_editing_host() && element.is_editing_host(), ElementConstructionFact::IsEditingHost);
    // NB: The document's body is found by walking its html element's children, which only these elements need to do.
    set(has_flag(open_facts, ElementOpenFacts::Body) && &element == element.document().body(), ElementConstructionFact::IsBody);
    set(element.is_document_element(), ElementConstructionFact::ConstructedAsDocumentElement);
    set(has_flag(open_facts, ElementOpenFacts::SvgElement) && static_cast<SVG::SVGElement const&>(element).is_resource_element(), ElementConstructionFact::IsSvgResourceElement);
    return facts;
}

static u32 element_construction_facts(DOM::Element const& element)
{
    auto shadow_root = element.containing_shadow_root();
    return kind_construction_facts(element) | open_construction_facts(element, element_open_facts(element), shadow_root && shadow_root->is_user_agent_internal());
}

// The style adjustment facts every element of a name shares.
static u32 kind_style_adjustment_facts(DOM::Element const& element)
{
    auto facts = kind_box_type_adjustment_facts(element);
    auto set = [&](bool condition, ElementStyleAdjustmentFact fact) {
        if (condition)
            facts |= fact;
    };
    set(element.is_html_html_element(), ElementStyleAdjustmentFact::IsHtmlHtmlElement);
    set(is<HTML::HTMLBodyElement>(element), ElementStyleAdjustmentFact::IsHtmlBodyElement);
    if (is<SVG::SVGElement>(element)) {
        facts |= ElementStyleAdjustmentFact::IsSvgElement;
        set(is<SVG::SVGGraphicsElement>(element), ElementStyleAdjustmentFact::IsSvgGraphicsElement);
        set(is<SVG::SVGSwitchElement>(element), ElementStyleAdjustmentFact::IsSvgSwitchElement);
        set(element.is_svg_container(), ElementStyleAdjustmentFact::IsSvgContainer);
        set(element.requires_svg_container(), ElementStyleAdjustmentFact::RequiresSvgContainer);
        set(element.is_svg_foreign_object_element(), ElementStyleAdjustmentFact::IsSvgForeignObjectElement);
        set(is<SVG::SVGMaskElement>(element), ElementStyleAdjustmentFact::IsSvgMaskElement);
        set(is<SVG::SVGClipPathElement>(element), ElementStyleAdjustmentFact::IsSvgClipPathElement);
        set(is<SVG::SVGPatternElement>(element), ElementStyleAdjustmentFact::IsSvgPatternElement);
    }
    set(is<HTML::HTMLFrameSetElement>(element), ElementStyleAdjustmentFact::IsHtmlFramesetElement);
    return facts;
}

// The style adjustment facts that hang on the element itself or on where it stands.
static u32 open_style_adjustment_facts(DOM::Element const& element, ElementOpenFacts open_facts)
{
    auto facts = open_box_type_adjustment_facts(element, open_facts);
    auto set = [&](bool condition, ElementStyleAdjustmentFact fact) {
        if (condition)
            facts |= fact;
    };
    // Admission facts do not participate in box transformations.
    // An animation the element is associated with composes into its style once it is relevant,
    // which its timeline can make it after the element's arrival.
    set(element.has_relevant_animations() || element.has_associated_animations(), ElementStyleAdjustmentFact::HasAnimations);
    set(element.associated_shadow_host_pseudo_element().has_value(), ElementStyleAdjustmentFact::IsShadowHostPseudoElement);
    set(element.rendered_in_top_layer(), ElementStyleAdjustmentFact::RenderedInTopLayer);
    return facts;
}

static u32 element_style_adjustment_facts(DOM::Element const& element)
{
    return kind_style_adjustment_facts(element) | open_style_adjustment_facts(element, element_open_facts(element));
}

void record_element_adjustment_facts(DOM::Element& element)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;
    StyleEngineFFI::style_engine_set_element_adjustment_facts(style_engine->host(), element.style_node_id(), element_style_adjustment_facts(element));
    StyleEngineFFI::style_engine_set_element_construction_facts(style_engine->host(), element.style_node_id(), element_construction_facts(element));
    StyleEngineFFI::style_engine_set_element_box_kind(style_engine->host(), element.style_node_id(), to_underlying(box_kind_for_layout(element)));
    auto pseudo_kind_plus_one = associated_pseudo_kind_plus_one(element);
    StyleEngineFFI::style_engine_set_element_backing_pseudo_kind(style_engine->host(), element.style_node_id(), pseudo_kind_plus_one);
}

void record_element_animations_changed(DOM::Element& element)
{
    record_element_adjustment_facts(element);
}

void record_element_box_kind(DOM::Element& element)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;
    StyleEngineFFI::style_engine_set_element_box_kind(style_engine->host(), element.style_node_id(), to_underlying(box_kind_for_layout(element)));
}

static void set_replaced_content_input(StyleEngine& style_engine, StyleNodeID node, StyleEngineFFI::FfiReplacedContentInputKind kind, u8 present, u32 first, u32 second = 0, u32 third = 0, u32 fourth = 0)
{
    u32 const values[] { first, second, third, fourth };
    StyleEngineFFI::style_engine_set_element_replaced_content_input(style_engine.host(), node, to_underlying(kind), present, values);
}

static void set_natural_size_input(StyleEngine& style_engine, StyleNodeID node, SizeWithAspectRatio const& natural_size, StyleEngineFFI::FfiReplacedContentInputKind kind = StyleEngineFFI::FfiReplacedContentInputKind::NaturalSize)
{
    using Present = StyleEngineFFI::FfiReplacedContentInputPresent;
    u8 present = 0;
    u32 values[4] {};
    if (natural_size.width.has_value()) {
        present |= to_underlying(Present::First);
        values[0] = bit_cast<u32>(natural_size.width->raw_value());
    }
    if (natural_size.height.has_value()) {
        present |= to_underlying(Present::Second);
        values[1] = bit_cast<u32>(natural_size.height->raw_value());
    }
    if (natural_size.aspect_ratio.has_value()) {
        present |= to_underlying(Present::ThirdAndFourth);
        values[2] = bit_cast<u32>(natural_size.aspect_ratio->numerator().raw_value());
        values[3] = bit_cast<u32>(natural_size.aspect_ratio->denominator().raw_value());
    }
    set_replaced_content_input(style_engine, node, kind, present, values[0], values[1], values[2], values[3]);
}

// An image box's natural size: its image's, or zero while no image is available.
static void set_image_natural_size_input(StyleEngine& style_engine, StyleNodeID node, Layout::ImageProvider const& image_provider)
{
    if (!image_provider.is_image_available()) {
        set_natural_size_input(style_engine, node, { 0, 0, {} });
        return;
    }
    set_natural_size_input(style_engine, node, { image_provider.intrinsic_width(), image_provider.intrinsic_height(), image_provider.intrinsic_aspect_ratio() });
}

// What the element gives the natural size of its replaced content, which layout resolves against the style of the
// element's box: what its attributes say, or the size of what it has loaded.
void record_element_replaced_content_input(DOM::Element& element)
{
    if (auto* style_engine = style_engine_for_identified(element))
        publish_element_replaced_content_input(element, *style_engine);
}

static void publish_element_replaced_content_input(DOM::Element& element, StyleEngine& style_engine)
{
    using Kind = StyleEngineFFI::FfiReplacedContentInputKind;
    auto node = element.style_node_id();
    if (auto const* text_area = as_if<HTML::HTMLTextAreaElement>(element)) {
        set_replaced_content_input(style_engine, node, Kind::TextArea, 0, text_area->cols(), text_area->rows());
        return;
    }
    if (auto const* image = as_if<HTML::HTMLImageElement>(element)) {
        set_image_natural_size_input(style_engine, node, *image);
        return;
    }
    if (auto const* object = as_if<HTML::HTMLObjectElement>(element)) {
        // An object representing its content navigable is sized from the SVG document it shows.
        if (object->represents_its_content_navigable())
            set_natural_size_input(style_engine, node, object->natural_size_of_content_document());
        else
            set_image_natural_size_input(style_engine, node, *object);
        return;
    }
    if (auto const* input = as_if<HTML::HTMLInputElement>(element)) {
        // An image button's box is an image box, which no size attribute sizes.
        if (input->type_state() == HTML::HTMLInputElement::TypeAttributeState::ImageButton) {
            set_image_natural_size_input(style_engine, node, *input);
            return;
        }
        auto kind = Kind::Input;
        switch (input->type_state()) {
        case HTML::HTMLInputElement::TypeAttributeState::Text:
        case HTML::HTMLInputElement::TypeAttributeState::Search:
        case HTML::HTMLInputElement::TypeAttributeState::URL:
        case HTML::HTMLInputElement::TypeAttributeState::Telephone:
        case HTML::HTMLInputElement::TypeAttributeState::Email:
        case HTML::HTMLInputElement::TypeAttributeState::Password:
        case HTML::HTMLInputElement::TypeAttributeState::Number:
            kind = Kind::TextEntryInput;
            break;
        default:
            break;
        }
        set_replaced_content_input(style_engine, node, kind, 0, input->size());
        return;
    }
    if (auto const* video = as_if<HTML::HTMLVideoElement>(element)) {
        SizeWithAspectRatio natural_size;
        if (auto size = video->natural_element_size(); size.has_value()) {
            if (size->is_empty())
                natural_size = { 0, 0, {} };
            else
                natural_size = { size->width(), size->height(), size->width() / size->height() };
        }
        set_natural_size_input(style_engine, node, natural_size);
        return;
    }
    if (auto const* image = as_if<SVG::SVGImageElement>(element)) {
        // An SVG <image> takes its image's natural size as the image reports it, and the default object size once
        // something has decoded.
        set_natural_size_input(style_engine, node, { image->intrinsic_width(), image->intrinsic_height(), image->intrinsic_aspect_ratio() },
            image->decoded_image_data() ? Kind::DecodedSvgImage : Kind::NaturalSize);
        return;
    }
    if (auto const* canvas = as_if<HTML::HTMLCanvasElement>(element))
        set_replaced_content_input(style_engine, node, Kind::Canvas, 0, canvas->width(), canvas->height());
}

void record_element_construction_facts(DOM::Element& element)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;
    StyleEngineFFI::style_engine_set_element_construction_facts(style_engine->host(), element.style_node_id(), element_construction_facts(element));
}

void publish_withheld_element_features(StyleEngine& style_engine, StyleComputer& style_computer, Layout::BegunRead const& read)
{
    auto now_read = style_engine.take_withheld_features_now_read(read);
    if (!now_read.local_link_state && !now_read.validity_state && now_read.attributes.is_empty())
        return;
    auto local_link = state_fact_for(PseudoClass::LocalLink);
    VERIFY(local_link.has_value());
    struct NameNowRead {
        StyleEngine::WithheldAttributeName name;
        StyleAtomID atom;
    };
    Vector<NameNowRead, 2> names_now_read;
    names_now_read.ensure_capacity(now_read.attributes.size());
    for (auto const& name : now_read.attributes)
        names_now_read.unchecked_append({ name, style_engine.intern_attribute_name(name.local_name, name.namespace_uri) });
    // NB: A name comes to be read once in a document's life, so nothing is kept per element for it. An element whose
    //     initial features still wait reads the names as they are by then.
    style_computer.for_each_element_with_style_node([&](DOM::Element& element) {
        if (!element.is_connected() || element.initial_style_features_pending())
            return;
        auto node = element.style_node_id();
        if (now_read.local_link_state && element.matches_local_link_pseudo_class())
            style_engine.record_state_delta({ .node = node.value(), .fact = *local_link, .new_value = true });
        if (now_read.validity_state && SelectorMatching::element_can_have_validity(element))
            Invalidation::record_validity_states(element);
        for (auto const& [name, atom] : names_now_read) {
            if (auto value = element.get_attribute_ns(name.namespace_uri, name.local_name); value.has_value()) {
                style_engine.record_local_feature_delta({
                    .node = node.value(),
                    .feature_kind = StyleEngineFFI::FfiFeatureKind::Attribute,
                    .name_atom = atom.value(),
                    .new_kind = StyleEngineFFI::FfiFeatureValueKind::Atom,
                    .new_atom = style_engine.intern_attribute_value(atom, *value).value(),
                });
            } else if (StyleEngineFFI::style_engine_row_holds_attribute(style_engine.host(), node.value(), atom.value())) {
                // NB: The row holds a value published before the name was known to be unread, of an attribute removed since.
                style_engine.record_local_feature_delta({
                    .node = node.value(),
                    .feature_kind = StyleEngineFFI::FfiFeatureKind::Attribute,
                    .name_atom = atom.value(),
                    .new_kind = StyleEngineFFI::FfiFeatureValueKind::Absent,
                    .new_atom = 0,
                });
            }
        }
    });
}

// The atom an id or class name is published under. A quirks-mode document matches those selectors
// ASCII case-insensitively, and a selector there is compiled against the lowercase folding of its
// own name, so an element's name has to be folded the same way for the two to name one atom.
static StyleAtomID intern_id_or_class_atom(StyleEngine& style_engine, DOM::Element const& element, Utf16FlyString const& name)
{
    if (element.document().in_quirks_mode())
        return style_engine.intern_atom(name.to_ascii_lowercase());
    return style_engine.intern_atom(name);
}

// Writes the block of rows the publication gathered, and what is published of their elements beside their rows.
static void finish_element_publication(StyleEngine& style_engine, ElementPublication& publication)
{
    for (auto element : publication.write_block()) {
        // The hover follows the element the events hover to where it arrives, the place a move took it to.
        if (element.ptr() == publication.hovered_element())
            style_engine.record_hover(element->style_node_id());
        if (auto const& id = element->id(); id.has_value())
            StyleEngineFFI::style_engine_set_element_id_name(style_engine.host(), element->style_node_id(), style_engine.intern_atom(*id));
        if (!element->part_names().is_empty())
            record_element_parts_changed(element);
        if (element->has_inline_style_declarations())
            record_element_inline_style_properties(element);
        auto const& kind = publication.kind_of(element);
        if (element_has_presentational_hints_to_publish(element, kind))
            StyleComputer::collect_presentational_hint_properties({ element });
        if (has_flag(kind.open_facts, ElementOpenFacts::ReplacedContent))
            publish_element_replaced_content_input(element, style_engine);
    }
    publication.forget_block();
}

static void publish_element_arrival(StyleEngine& style_engine, DOM::Element& element, ElementPublication& publication, Optional<StyleEngineFFI::FfiArrivingElement const&> arriving)
{
    publish_element_selector_features(style_engine, element, publication, arriving);
    if (publication.block_is_full())
        finish_element_publication(style_engine, publication);
}

void publish_pending_element_features(StyleEngine& style_engine, StyleComputer& style_computer, Layout::BegunRead const& read)
{
    auto nodes = style_engine.take_deferred_element_initial_features();
    if (nodes.is_empty())
        return;
    learn_what_the_engine_reads(style_engine, read);
    ElementPublication publication { style_engine, style_computer.document() };
    for (size_t index = 0; index < nodes.size(); ++index) {
        prefetch_publication_ahead(index, nodes.size(), [&](size_t index) { return style_computer.node_for_style_node(nodes[index]).ptr(); });
        auto node = nodes[index];
        auto element = style_computer.element_for_style_node(node);
        // NB: An element that left the tree gave up its mark. Its identity may have gone to an element since, which is
        //     then listed twice and published at the first.
        if (!element || !element->initial_style_features_pending())
            continue;
        // An inserted subtree takes its identities before its insertion steps mark its nodes connected, in tree order.
        // A step that submits the recorded input on the way, as a <style> writing its sheet to the engine in place
        // while a style update flies, finds the elements after it unconnected: their arrivals are submitted, and
        // their features wait for the next submit, which folds them onto those arrivals.
        if (!element->is_connected()) {
            style_engine.defer_element_initial_features(*element);
            continue;
        }
        element->set_initial_style_features_pending(false);
        if (style_engine_for_identified(*element))
            publish_element_arrival(style_engine, *element, publication);
    }
    finish_element_publication(style_engine, publication);
}

void record_node_moved_in_dom_order(DOM::Node& node, DOM::Node const& old_parent)
{
    auto* style_engine = style_engine_for(node);
    auto identity = dom_order_identity_of(node);
    if (!style_engine || identity == no_style_node)
        return;
    StyleEngineFFI::style_engine_unlink_style_node_from_dom_order(style_engine->host(), identity, dom_order_parent_of(&old_parent));
    ensure_dom_order_parent_identity(node.parent(), *style_engine);
    link_in_dom_order(*style_engine, node);
}

void record_element_moved(DOM::Element& element, DOM::Node* old_parent, DOM::Element* old_previous_sibling, DOM::Element* old_next_sibling, DOM::Element* old_assigned_slot)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;

    auto relations = relations_of(element, *style_engine);
    auto previous = relations;
    // The parent the element left is a node of the style tree whether or not it is an element: a
    // shadow root is one, and reporting no parent at all would say the element crossed out of the
    // tree rather than moved inside it.
    previous.parent = no_style_node.value();
    if (auto* old_parent_element = as_if<DOM::Element>(old_parent))
        previous.parent = old_parent_element->style_node_id().value();
    else if (auto* old_shadow_root = as_if<DOM::ShadowRoot>(old_parent))
        previous.parent = old_shadow_root->style_node_id().value();
    previous.previous_element_sibling = identity_of_arrived_previous_sibling(old_previous_sibling).value();
    previous.next_element_sibling = identity_of_arrived_next_sibling(old_next_sibling).value();
    previous.assigned_slot = identity_of(old_assigned_slot).value();
    if (old_parent)
        previous.tree_scope = tree_scope_of(old_parent->root()).value();
    if (previous.parent == relations.parent
        && previous.previous_element_sibling == relations.previous_element_sibling
        && previous.next_element_sibling == relations.next_element_sibling
        && previous.assigned_slot == relations.assigned_slot) {
        return;
    }

    // A heading's level counts the heading offset its ancestors declare, so moving under a
    // different ancestor can change it without the element itself changing at all.
    record_heading_levels_in_subtree(element);

    if (previous.parent != relations.parent) {
        // Language and directionality resolve through the parent chain, but are published facts
        // rather than computed values. Republish them for the moved subtree from its new place.
        Invalidation::invalidate_style_after_language_change(element);

        // Whether an SVG element's position must become static depends on the SVG and
        // foreignObject elements above it. A preserved move changes that chain without giving
        // the moved subtree another arrival notification.
        element.for_each_shadow_including_inclusive_descendant([&](auto& node) {
            if (auto* descendant = as_if<DOM::Element>(node); descendant && descendant->namespace_uri() == Namespace::SVG && descendant->style_node_id() != no_style_node) {
                StyleEngineFFI::style_engine_set_element_adjustment_facts(style_engine->host(), descendant->style_node_id(), element_style_adjustment_facts(*descendant));
                style_engine->record_derived_element_style_input_change(descendant->style_node_id(), StyleEngine::RecomputeStyle);
            }
            // A table cell's hints come from the table it is now under.
            if (auto* descendant = as_if<DOM::Element>(node); descendant && descendant->namespace_uri() == Namespace::HTML && descendant->style_node_id() != no_style_node
                && first_is_one_of(descendant->local_name(), HTML::TagNames::td, HTML::TagNames::th))
                republish_presentational_hints(*descendant);
            return TraversalDecision::Continue;
        });

        // Moving to a different parent changes the inherited input even if the moved element
        // matches exactly the same rules. Recomputing its style lets ordinary inherited-style
        // propagation carry any change through its light and shadow subtrees.
        style_engine->record_derived_element_style_input_change(element.style_node_id(), StyleEngine::RecomputeStyle);

        // The tree the element's light descendants stand in moved with it, which they answer for themselves. Their own
        // shadow trees are scopes of their own.
        if (previous.tree_scope != relations.tree_scope) {
            element.for_each_in_subtree_of_type<DOM::Element>([&](DOM::Element& descendant) {
                if (descendant.style_node_id() == no_style_node)
                    return TraversalDecision::Continue;
                auto descendant_relations = relations_of(descendant, *style_engine, TreeScopeID { relations.tree_scope });
                auto descendant_previous = descendant_relations;
                descendant_previous.tree_scope = previous.tree_scope;
                record_element_relinked(*style_engine, descendant.style_node_id(), descendant_previous, descendant_relations);
                return TraversalDecision::Continue;
            });
            // The hosts it and those below it are exposed to as parts are those of the tree it stands in now.
            element.for_each_shadow_including_inclusive_descendant([](DOM::Node& node) {
                if (auto* moved = as_if<DOM::Element>(node))
                    record_element_parts_changed(*moved);
                return TraversalDecision::Continue;
            });
        }
    }

    // The relinking path rather than the neighbour one. A move rewrites the DOM's own links without
    // going through insertion or removal, so nothing has told the engine's relation columns that
    // anything happened: they are the engine's copy of the child sequence, and only a delta splices
    // them. Publishing this as a neighbour change leaves the columns holding the order the element
    // left, which is the order every positional question is then answered in.
    record_element_relinked(*style_engine, element.style_node_id(), previous, relations);
}

void record_element_assigned_slot_changed(DOM::Element& element, DOM::Element* old_slot)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;
    // An element without a parent is between the removal and insertion steps of a move, or has
    // left. The move or the departure publishes its relations: relinking it here would report it
    // as having had no parent before, which a transaction keeps as where the element came from.
    if (!element.parent())
        return;

    auto relations = relations_of(element, *style_engine);
    auto previous = relations;
    previous.assigned_slot = identity_of(old_slot).value();
    if (previous.assigned_slot == relations.assigned_slot)
        return;

    // The engine holds the slot a slottable is assigned to, and only relinking updates it. Every other relation is
    // identical on both sides, so unlinking and linking again leaves them where they were.
    record_element_relinked(*style_engine, element.style_node_id(), previous, relations);
}

void record_slot_assignment_changed(HTML::HTMLSlotElement& slot)
{
    // A slot is named for as long as it is in the engine's tree, and assignment runs inside the insertion that connects
    // it, before the connected flag is set. The identity is therefore the membership test here, rather than
    // style_engine_for().
    if (slot.style_node_id() == no_style_node || !slot.document().style_engine_tracks_tree())
        return;

    auto const& assigned = slot.assigned_nodes_internal();
    Vector<StyleNodeID, 8> identities;
    identities.ensure_capacity(assigned.size());
    for (auto const& slottable : assigned) {
        auto identity = slottable.visit([](auto const& node) { return node->style_node_id(); });
        if (identity != no_style_node)
            identities.unchecked_append(identity);
    }
    StyleEngineFFI::style_engine_set_slot_assigned_nodes(slot.document().style_computer().style_engine().host(), slot.style_node_id(), identities.span());
    Invalidation::invalidate_style_after_slot_assignment_changed(slot);
}

// Only a connected element in a fully active document enters the top layer. A member still waiting to arrive has no
// identity yet, and its arrival publishes the top layer again. One that has since disconnected, waiting in the pending
// removals, has given its identity up.
void record_top_layer_changed(DOM::Document& document)
{
    if (!document.style_engine_tracks_tree())
        return;
    auto const& members = document.top_layer_elements();
    Vector<StyleNodeID, 8> identities;
    identities.ensure_capacity(members.size());
    auto& style_engine = document.style_computer().style_engine();
    for (auto const& member : members) {
        if (member->style_node_id() != no_style_node) {
            identities.unchecked_append(member->style_node_id());
            continue;
        }
        // A member is rendered whatever display:none subtree it is in, so the children it waits with arrive.
        if (member->is_connected())
            release_waiting_children_holding(*member, style_engine);
    }
    StyleEngineFFI::style_engine_set_top_layer_elements(document.style_computer().style_engine().host(), identities.span());
}

// Assignment runs inside the insertion that connects a node, which happens before the subtree it arrived in is named,
// and the list published then names only the members that already had an identity. Both ends of the relation therefore
// republish on arrival: a slottable the list it has just become a member of, and a slot the list it arrived owning.
static void republish_assigned_slots_of(ReadonlySpan<GC::Ref<DOM::Node>> nodes)
{
    GC::RootVector<GC::Ref<HTML::HTMLSlotElement>> slots;
    GC::RootHashTable<HTML::HTMLSlotElement const*> seen_slots;
    auto add_slot = [&](HTML::HTMLSlotElement& slot) {
        if (seen_slots.set(&slot) == HashSetResult::InsertedNewEntry)
            slots.append(slot);
    };
    for (auto const& node : nodes) {
        if (auto slot = DOM::assigned_slot_for_node(node)) {
            slot->assign_slottables_if_needed();
            add_slot(*slot);
        }
        if (auto* slot = as_if<HTML::HTMLSlotElement>(*node))
            add_slot(*slot);
    }
    for (auto const& slot : slots)
        record_slot_assignment_changed(slot);
}

static void record_element_disconnecting(DOM::Element& element, TreeScopeID tree_scope, PreviousSiblings previous_siblings)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;
    auto node = element.style_node_id();

    // Removing a slot can leave its slottables with no replacement. Publish their departure while
    // the slot still has an identity; assignment runs after the disconnect walk and cannot name the
    // old slot by then. If another slot takes over, the ordinary assignment path publishes the
    // subsequent arrival.
    if (auto* slot = as_if<HTML::HTMLSlotElement>(element)) {
        slot->assign_slottables_if_needed();
        for (auto const& slottable : slot->assigned_nodes_internal()) {
            auto const* assigned_element = slottable.get_pointer<GC::Ref<DOM::Element>>();
            if (!assigned_element || (*assigned_element)->style_node_id() == no_style_node)
                continue;
            auto old_relations = relations_of(**assigned_element, *style_engine);
            auto new_relations = old_relations;
            new_relations.assigned_slot = no_style_node.value();
            record_element_relinked(*style_engine, (*assigned_element)->style_node_id(), old_relations, new_relations);
        }
    }

    style_engine->cancel_deferred_element_initial_features(element);

    style_engine->record_tree_delta({
        .node = node.value(),
        .old_connected = true,
        .new_connected = false,
        .old_relations = relations_of(element, *style_engine, tree_scope, previous_siblings),
        .new_relations = detached_relations(),
    });

    // Dropping the identity is what makes a second removal a no-op rather than a double retirement.
    element.document().style_computer().unregister_style_node(node);
    element.set_style_node_id(no_style_node);
}

// The names of the CSS animations the element owns, in one of its per-pseudo-element lists, and the
// definition each one last had applied.
//
// This one is an input: the computation of an element's animation definitions matches them
// against the animations the element already has, and this is what it matches them against.
void record_element_css_defined_animations(DOM::Element& element, u8 slot, ReadonlySpan<Utf16FlyString> names, ReadonlySpan<StyleEngineFFI::FfiAppliedAnimationDefinition> definitions)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;

    // The names cross as the fly strings they are, which the engine copies. A list is almost always a single name.
    Vector<FlatPtr, 1> raw_names;
    raw_names.ensure_capacity(names.size());
    for (auto const& name : names)
        raw_names.unchecked_append(name.raw_identity());
    StyleEngineFFI::style_engine_set_element_css_defined_animations(style_engine->host(), element.style_node_id(), slot, raw_names, definitions);
}

// A keyframe's composite operation as a published keyframe spells it: the keyframe's own, or its effect's where the
// keyframe's is auto.
static StyleValueFFI::FfiCompositeOperation published_composite_operation(Bindings::CompositeOperationOrAuto keyframe, Bindings::CompositeOperation effect)
{
    switch (keyframe) {
    case Bindings::CompositeOperationOrAuto::Replace:
        return StyleValueFFI::FfiCompositeOperation::Replace;
    case Bindings::CompositeOperationOrAuto::Add:
        return StyleValueFFI::FfiCompositeOperation::Add;
    case Bindings::CompositeOperationOrAuto::Accumulate:
        return StyleValueFFI::FfiCompositeOperation::Accumulate;
    case Bindings::CompositeOperationOrAuto::Auto:
        break;
    }
    switch (effect) {
    case Bindings::CompositeOperation::Replace:
        return StyleValueFFI::FfiCompositeOperation::Replace;
    case Bindings::CompositeOperation::Add:
        return StyleValueFFI::FfiCompositeOperation::Add;
    case Bindings::CompositeOperation::Accumulate:
        return StyleValueFFI::FfiCompositeOperation::Accumulate;
    }
    VERIFY_NOT_REACHED();
}

// One keyframe's easing, spelled out for publication. A `linear()` keeps its control points in the
// shared buffer the keyframe names by range.
static void describe_easing(EasingFunction const& easing, StyleEngineFFI::FfiPublishedAnimationKeyframe& keyframe, Vector<StyleEngineFFI::FfiPublishedLinearEasingPoint>& points)
{
    static_assert(to_underlying(StyleEngineFFI::FfiPublishedEasingKind::Steps) == to_underlying(Compositing::RustFFI::FfiEasingKind::Steps));
    auto descriptor = easing.descriptor();
    keyframe.easing_kind = static_cast<StyleEngineFFI::FfiPublishedEasingKind>(to_underlying(descriptor.kind));
    keyframe.x1 = descriptor.x1;
    keyframe.y1 = descriptor.y1;
    keyframe.x2 = descriptor.x2;
    keyframe.y2 = descriptor.y2;
    keyframe.interval_count = descriptor.interval_count;
    keyframe.step_position = descriptor.step_position;
    keyframe.first_linear_point = static_cast<u32>(points.size());
    for (size_t index = 0; index < descriptor.linear_point_count; ++index)
        points.append({ .input = descriptor.linear_points[index].input, .output = descriptor.linear_points[index].output });
    keyframe.linear_point_count = static_cast<u32>(points.size()) - keyframe.first_linear_point;
}

// The effects one of an element's animation lists holds, in composite order, described for the style
// engine to sample them from, unless it already describes exactly these versions of them: every change
// that moves what a description says moves its effect's generation.
//
// Everything a keyframe declares that does not depend on the element being sampled is settled here. What
// does, a value or an easing still to be substituted against the element, travels as written.
void record_attr_names_read_by_keyframes(DOM::Element& element, Animations::KeyframeEffect const& effect)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || !effect.key_frame_set())
        return;
    bool reads_more_attributes = false;
    for (auto const& keyframe : effect.key_frame_set()->keyframes_by_key) {
        for (auto const& [property, value] : keyframe.properties) {
            if (auto const* handle = value.get_pointer<RustStyleValueHandle>())
                reads_more_attributes |= StyleEngineFFI::style_engine_note_attr_names_read_by_value(style_engine->host(), handle->data());
        }
        if (auto const* easing = keyframe.easing.get_pointer<RustStyleValueHandle>())
            reads_more_attributes |= StyleEngineFFI::style_engine_note_attr_names_read_by_value(style_engine->host(), easing->data());
    }
    // NB: An effect is sampled against what the engine holds as a read begins, so the attributes go over
    //     now. Only a script's keyframes get here with a new name, between reads: the keyframes of a CSS
    //     animation were noted as their rule was published.
    if (reads_more_attributes)
        style_engine->flush();
}

void record_element_animation_effect_descriptions(DOM::Element& element, u8 slot, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;

    Vector<StyleEngineFFI::FfiAnimationEffectVersion, 4> versions;
    versions.ensure_capacity(effects.size());
    for (auto const& effect : effects)
        versions.unchecked_append({ .identity = effect->animation_preparation_identity(), .generation = effect->animation_preparation_generation() });
    if (StyleEngineFFI::style_engine_describes_animation_effects(style_engine->host(), element.style_node_id().value(), slot, versions.span()))
        return;

    Vector<StyleEngineFFI::FfiPublishedAnimationEffect> ffi_effects;
    Vector<StyleEngineFFI::FfiPublishedAnimationKeyframe> ffi_keyframes;
    Vector<StyleEngineFFI::FfiPublishedAnimationDeclaration> ffi_declarations;
    Vector<StyleEngineFFI::FfiPublishedAnimationCustomDeclaration> ffi_custom_declarations;
    Vector<StyleEngineFFI::FfiPublishedLinearEasingPoint> ffi_points;
    Vector<u8> base_url_bytes;
    ffi_effects.ensure_capacity(effects.size());
    for (auto const& effect : effects) {
        auto animation = effect->associated_animation();
        StyleEngineFFI::FfiPublishedAnimationEffect row {};
        row.identity = effect->animation_preparation_identity();
        row.generation = effect->animation_preparation_generation();
        row.first_keyframe = static_cast<u32>(ffi_keyframes.size());
        row.base_url_offset = static_cast<u32>(base_url_bytes.size());
        row.is_transition = animation && animation->is_css_transition();
        row.reversing_shortening_factor = 1;
        if (row.is_transition) {
            auto const& transition = static_cast<CSSTransition const&>(*animation);
            row.reversing_adjusted_start_value = transition.reversing_adjusted_start_value()->rust_style_value_data();
            row.reversing_shortening_factor = transition.reversing_shortening_factor();
        }
        // An effect with no animation, or whose animation runs no keyframes, is described with none, which is
        // all there is to sample of it.
        auto const* key_frame_set = animation ? effect->key_frame_set() : nullptr;
        if (!key_frame_set) {
            ffi_effects.unchecked_append(row);
            continue;
        }
        if (auto const& resource_context = key_frame_set->style_sheet_resource_context; resource_context.has_value()) {
            row.has_resource_context = true;
            row.resource_context_is_origin_clean = resource_context->origin_clean;
            auto bytes = resource_context->base_url.bytes();
            base_url_bytes.append(bytes.data(), bytes.size());
            row.base_url_length = static_cast<u32>(bytes.size());
        }
        // A keyframe that declares no easing of its own runs the animation's.
        auto default_easing = animation->is_css_animation()
            ? static_cast<CSSAnimation const&>(*animation).default_easing()
            : EasingFunction::linear();
        for (auto it = key_frame_set->keyframes_by_key.begin(); it != key_frame_set->keyframes_by_key.end(); ++it) {
            StyleEngineFFI::FfiPublishedAnimationKeyframe keyframe {};
            keyframe.key = static_cast<i64>(it.key());
            auto easing = it->easing.visit(
                [&](Empty) { return default_easing; },
                [](EasingFunction const& easing) { return easing; },
                [&](RustStyleValueHandle const& value) {
                    // The value can need substitution against the element, which the engine does when
                    // it samples it; the animation's easing is what the keyframe runs if it resolves to
                    // none.
                    keyframe.easing_value = value.data();
                    return default_easing;
                });
            describe_easing(easing, keyframe, ffi_points);
            keyframe.composite = published_composite_operation(it->composite, effect->composite());
            keyframe.first_declaration = static_cast<u32>(ffi_declarations.size());
            keyframe.first_custom_declaration = static_cast<u32>(ffi_custom_declarations.size());
            for (auto const& [property, value] : it->properties) {
                // A keyframe the host synthesized holds the element's own value, which is not known
                // until the element is sampled, and travels as no value. A shorthand's stands for its
                // longhands', which have keyframes of their own.
                bool const use_initial = value.has<Animations::KeyframeEffect::KeyFrameSet::UseInitial>();
                if (use_initial && !property.is_custom_property() && property_is_shorthand(property.id()))
                    continue;
                StyleValueFFI::StyleValueData const* data = use_initial ? nullptr : value.get<RustStyleValueHandle>().data();
                // A value that is invalid at computed-value time animates nothing. A longhand pending the
                // substitution of its shorthand's value is substituted with it when the engine samples it.
                // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                if (!use_initial && data->tag == StyleValueFFI::StyleValueData::Tag::GuaranteedInvalid && !property.is_custom_property())
                    continue;
                if (property.is_custom_property())
                    ffi_custom_declarations.append({ .name = property.name().raw_identity(), .value = data });
                else
                    ffi_declarations.append({ .property_id = to_underlying(property.id()), .value = data });
            }
            keyframe.declaration_count = static_cast<u32>(ffi_declarations.size()) - keyframe.first_declaration;
            keyframe.custom_declaration_count = static_cast<u32>(ffi_custom_declarations.size()) - keyframe.first_custom_declaration;
            ffi_keyframes.append(keyframe);
        }
        row.keyframe_count = static_cast<u32>(ffi_keyframes.size()) - row.first_keyframe;
        ffi_effects.unchecked_append(row);
    }

    StyleEngineFFI::style_engine_set_element_animation_effect_descriptions(style_engine->host(),
        element.style_node_id().value(), slot,
        ffi_effects.data(), ffi_effects.size(),
        ffi_keyframes.data(), ffi_keyframes.size(),
        ffi_declarations.data(), ffi_declarations.size(),
        ffi_custom_declarations.data(), ffi_custom_declarations.size(),
        ffi_points.data(), ffi_points.size(),
        base_url_bytes.data(), base_url_bytes.size());
}

// An element's heading level, which `:heading()` tests. It follows from what the element is plus
// the heading offset its ancestors declare, so it moves when the element does and when one of those
// attributes changes - not with anything the element itself publishes.
static void record_element_heading_level(DOM::Element& element)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;

    auto const* heading = as_if<HTML::HTMLHeadingElement>(element);
    // Attribute invalidation runs before the document tree version is bumped, so the DOM-facing
    // heading_level() cache can still hold the value from before a headingoffset mutation.
    auto level = heading ? heading->computed_heading_level() : 0;
    StyleEngineFFI::style_engine_set_element_heading_level(style_engine->host(), element.style_node_id(), static_cast<u8>(min(level, 255u)));
}

// Republish the heading level of every element under one whose heading offset just moved.
static void record_heading_levels_in_subtree(DOM::Element& element)
{
    element.for_each_shadow_including_inclusive_descendant([](auto& node) {
        if (auto* descendant = as_if<DOM::Element>(node))
            record_element_heading_level(*descendant);
        return TraversalDecision::Continue;
    });
}

void record_element_language_and_directionality(DOM::Element& element)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;

    auto const language = element.lang_view();
    // A language range is not a name, so `:lang()` compares against the tag itself rather than against the atom. The
    // engine is given the tag once per language, not once per element.
    StyleEngineFFI::style_engine_set_element_language(style_engine->host(), element.style_node_id().value(),
        language.has_value() ? style_engine->intern_text_atom(*language).value() : 0,
        StyleEngineFFI::ffi_utf16_view(language.value_or({})));

    record_element_directionality(element);

    // Reading the tag caches it, and this can run while the element is still being built - an XML
    // parser sets attributes after insertion, so the tag read here may not be the one it ends up
    // with. Drop the cache so the next read computes it from the finished element. The published
    // fact is not wrong for having been early: what a `:lang()` rule is routed by is that the tag
    // moved, and the move to its real value is published like any other.
    element.invalidate_lang_value();
}

void record_element_directionality(DOM::Element& element)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;

    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? u"rtl"sv : u"ltr"sv;
    StyleEngineFFI::style_engine_set_element_directionality(style_engine->host(), element.style_node_id(), style_engine->intern_text_atom(directionality));
}

void record_element_custom_states_changed(DOM::Element& element)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;

    Vector<StyleAtomID> atoms;
    if (auto states = element.custom_state_set()) {
        for (auto const& state : states->states())
            atoms.append(style_engine->intern_atom(state));
    }
    StyleEngineFFI::style_engine_set_element_custom_states(style_engine->host(), element.style_node_id(), atoms);
}

// Walk the chain of hosts outwards, carrying the names the element is addressable by at each level.
//
// A part name reaches the scope enclosing the element's own shadow root. Each host along the way
// forwards the names it chose to, under the names it chose, so the walk ends at the first host that
// forwards none of them. It reports both halves of what a `::part()` rule needs: every name the
// element answers to, and how far out the last of them reaches.
//
// The names and the hosts are also reported paired, one entry per name per level. A name reaches the
// tree the host forwarding it stands in and no other, so a rule writing one level's name while its
// outer compound describes another level's host names no element at all - which the union of the
// names and the outermost host on its own cannot express.
static StyleNodeID collect_part_exposure(DOM::Element const& element, Vector<Utf16FlyString>& pair_names, Vector<StyleNodeID>& pair_hosts)
{
    Vector<Utf16FlyString> names;
    for (auto const& part : element.part_names())
        names.append(part);

    StyleNodeID exposing_host;
    for (auto root = element.containing_shadow_root(); root && !names.is_empty();) {
        auto host = root->host();
        if (!host)
            break;
        exposing_host = host->style_node_id();

        for (auto const& name : names) {
            pair_names.append(name);
            pair_hosts.append(exposing_host);
        }

        Vector<Utf16FlyString> forwarded;
        host->for_each_exported_part([&](Utf16View inner, Utf16View outer) {
            auto inner_name = Utf16FlyString::from_utf16(inner);
            if (!names.contains_slow(inner_name))
                return;
            auto outer_name = Utf16FlyString::from_utf16(outer);
            if (!forwarded.contains_slow(outer_name))
                forwarded.append(outer_name);
        });
        names = move(forwarded);
        root = host->containing_shadow_root();
    }
    return exposing_host;
}

void record_element_parts_changed(DOM::Element& element)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;

    // A rule naming a forwarded part names the element under the forwarded name, so the names it
    // is exposed under are what it is published as - and the reach alongside them, because a host
    // usually forwards a name under the one it already had, which moves no name at all.
    Vector<Utf16FlyString> pair_names;
    Vector<StyleNodeID> pair_hosts;
    auto const exposing_host = collect_part_exposure(element, pair_names, pair_hosts);

    Vector<StyleAtomID> pair_atoms;
    pair_atoms.ensure_capacity(pair_names.size());
    for (auto const& name : pair_names)
        pair_atoms.unchecked_append(style_engine->intern_atom(name));
    style_engine->set_element_parts(element.style_node_id(), pair_atoms, pair_hosts);

    StyleEngineFFI::style_engine_set_element_part_exposure(style_engine->host(), element.style_node_id(), StyleAtomID { exposing_host.value() });
}

void record_element_emptiness_changed(DOM::Element& element, DOM::Node const& changing_child, bool counted_before, bool counts_after)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;

    // The engine reads an element's element children from its tree, so what it holds here is whether the element has
    // text: an element child that comes or goes beside the text leaves it as it is.
    auto has_other_text = false;
    for (GC::Ptr<DOM::Node const> child = element.first_child(); child; child = child->next_sibling()) {
        if (GC::Ptr<DOM::Text const> text = as_if<DOM::Text>(*child); text && child.ptr() != &changing_child && !text->data().is_empty()) {
            has_other_text = true;
            break;
        }
    }
    auto was_empty = !has_other_text && !counted_before;
    auto is_empty = !has_other_text && !counts_after;
    if (was_empty == is_empty)
        return;

    auto kind_of = [](bool empty) {
        return empty ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };
    record_feature(element, StyleEngineFFI::FfiFeatureKind::Emptiness, 0, kind_of(is_empty), 0);
}

// An element can arrive with a style attribute already written, so this is published on arrival as
// well as when the block is edited.
static void record_element_inline_style_properties(DOM::Element& element)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;
    style_engine->set_element_inline_style_properties(element.style_node_id(), element.inline_style_declarations());
}

// The hints an element's attributes map to, published at its arrival, wherever what they are mapped
// from moves, and from where the cascade collects them.
bool record_element_presentational_hint_properties(DOM::Element& element, ReadonlySpan<StyleProperty> hints)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return false;
    // NB: The SvgPresentationAttribute kind is the one whose declarations the engine takes as
    //     current: every element's hints are published where they move.
    style_engine->set_element_presentational_hint_properties(element.style_node_id(), StyleEngineFFI::FfiElementDeclarationKind::SvgPresentationAttribute, hints);
    return true;
}

void record_element_declarations_changed(DOM::Element& element, ElementDeclarationKind kind, bool had_declarations, bool has_declarations)
{
    if (element.style_node_id() != no_style_node)
        StyleEngineFFI::style_engine_note_element_declarations_changed(element.document().style_computer().style_engine().host(), element.style_node_id().value());
    element.document().flush_deferred_style_change_event();
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;
    if (!had_declarations && !has_declarations)
        return;

    // The block's contents moved even where the CSSOM object did not, so the identity that makes
    // this a change is a version rather than the object's address. The engine mints it as it
    // applies the delta.
    style_engine->record_element_declaration_delta({
        .node = element.style_node_id().value(),
        .kind = kind,
        .old_block = had_declarations ? 1u : 0u,
        .new_block = has_declarations ? 1u : 0u,
    });

    // The properties the block covers are published from wherever the block is built. Inline style
    // is built here, because the CSSOM object is the block; a hint is built by the cascade.
    if (kind == ElementDeclarationKind::InlineStyle)
        record_element_inline_style_properties(element);
}

void record_shadow_root_disconnecting(DOM::ShadowRoot& shadow_root)
{
    auto node = shadow_root.style_node_id();
    if (node == no_style_node)
        return;
    if (auto* style_engine = style_engine_for(shadow_root)) {
        auto relations = detached_relations();
        relations.tree_scope = tree_scope_of(shadow_root).value();
        style_engine->record_tree_delta({
            .node = node.value(),
            .old_connected = true,
            .new_connected = false,
            .old_relations = relations,
            .new_relations = detached_relations(),
        });
    }
    shadow_root.document().style_computer().unregister_style_node(node);
    shadow_root.set_style_node_id(no_style_node);
}

void record_subtree_disconnecting(DOM::Node& root)
{
    auto* style_engine = style_engine_for(root);
    // A hovered element that leaves the document takes the engine's hover off it, until it arrives again.
    if (auto const* hovered = style_engine ? hovered_element_of(root.document().hovered_node()) : nullptr; hovered && root.is_shadow_including_inclusive_ancestor_of(*hovered))
        style_engine->record_hover({});
    // Only the root leaves a child sequence that stays in the tree. Every node below it leaves with the sequence it
    // belongs to.
    if (auto identity = dom_order_identity_of(root); style_engine && identity != no_style_node)
        StyleEngineFFI::style_engine_unlink_style_node_from_dom_order(style_engine->host(), identity, dom_order_parent_of(root.parent()));
    auto root_tree_scope = tree_scope_of(root.root());
    GC::RootVector<GC::Ref<DOM::ShadowRoot>> shadow_roots;
    Vector<StyleNodeID, 64> departing_texts;
    auto disconnect_element = [&](DOM::Node& node, TreeScopeID tree_scope) {
        if (auto* element = as_if<DOM::Element>(node)) {
            // Children that waited leave with the element, which takes them in again as a whole wherever it arrives.
            element->set_child_style_arrivals_wait(false);
            record_element_disconnecting(*element, tree_scope, &node == &root ? PreviousSiblings::Arrived : PreviousSiblings::Departed);
        } else if (auto* shadow_root = as_if<DOM::ShadowRoot>(node)) {
            shadow_roots.append(*shadow_root);
        } else if (auto* text = as_if<DOM::Text>(node); text && style_engine && text->style_node_id() != no_style_node) {
            departing_texts.append(text->style_node_id());
            text->document().style_computer().unregister_style_node(text->style_node_id());
            text->set_style_node_id(no_style_node);
        }
    };
    for_each_shadow_including_inclusive_descendant_with_scope(root, root_tree_scope, disconnect_element);
    if (!departing_texts.is_empty())
        StyleEngineFFI::style_engine_retire_text_style_nodes(style_engine->host(), departing_texts.span());

    // Only once no element still names a shadow root as its parent can the root give up its own
    // identity.
    for (auto const& shadow_root : shadow_roots)
        record_shadow_root_disconnecting(shadow_root);
}

void record_shadow_root_connected(DOM::ShadowRoot& shadow_root)
{
    auto* style_engine = style_engine_for(shadow_root);
    if (!style_engine)
        return;
    // A host's shadow tree takes the place of its children in the flat tree, so children that waited below it arrive,
    // as do those a host waits with: a shadow host cannot wait to arrive.
    if (auto* host = shadow_root.host(); host && host->child_style_arrivals_wait())
        release_waiting_children_until_submitted(*host, *style_engine);
    else if (host && host->style_node_id() == no_style_node)
        release_waiting_children_holding(*host, *style_engine);
    // The root retakes its place here rather than when whatever next needs it happens to ask. A
    // scope's root is how a sheet attached to that scope is bounded, and an empty shadow tree has no
    // child whose relations would ask for it, so the scope would be left naming a node that has been
    // given up.
    (void)identity_of_shadow_root(shadow_root, *style_engine);
}

// https://html.spec.whatwg.org/multipage/semantics-other.html#case-sensitivity-of-selectors
// A handful of attribute names compare their values ASCII case-insensitively, but only on an HTML
// element in an HTML document. The element half is the namespace each element already publishes;
// this is the document half, and a selector is compiled against it, so it has to be said before any
// rule is.
static void publish_document_kind(DOM::Document& document)
{
    auto& style_engine = document.style_computer().style_engine();
    style_engine.publish_html_element_namespace(
        document.document_type() == DOM::Document::Type::HTML
            ? style_engine.intern_atom(Namespace::HTML)
            : 0);
}

// https://drafts.csswg.org/css-cascade-6/#scope-atrule
// An `@scope` with no `<scope-start>` roots at an element rather than at a selector, so nothing the
// compiler reads says where it is. Resolving it here is the same walk the matcher makes, and the
// engine names the answer by identity.
static constexpr StyleNodeID implicit_scope_root_of_the_containing_tree { 0xffffffff };

static StyleNodeID implicit_scope_root_of(StyleSheetState const* owner_style_sheet)
{
    if (!owner_style_sheet)
        return no_style_node;

    // If no <scope-start> is specified, the scoping root is the parent element of the owner node of
    // the stylesheet where the @scope rule is defined.
    if (auto* owner_node = const_cast<StyleSheetState&>(*owner_style_sheet).owner_node()) {
        if (auto parent = owner_node->parent_element())
            return parent->style_node_id();
    }

    // If no such element exists and the containing node tree is a shadow tree, then the scoping root
    // is the shadow host; otherwise it is the root of the containing node tree. Which of those holds
    // is a property of the scope a sheet is being asked in rather than of the sheet - one
    // constructed sheet can be adopted into several - so the engine resolves it while matching.
    return implicit_scope_root_of_the_containing_tree;
}

using CompilationVisitor = Function<bool(RustRule::Type, StyleSheetState const&, Parser::ValueParserFFI::NativeCompilationContext const&, Parser::ValueParserFFI::NativeCompilationResult const&)>;

static void visit_compilation(StyleSheetState const& sheet, u64 rule_identity, DOM::Document const& document, Parser::ValueParserFFI::NativeCompilationPurpose purpose, CompilationVisitor const& visit, Parser::ValueParserFFI::NativeStylePublication const& publication)
{
    // An implicit scope is named by its root's identity, which a root still waiting to arrive takes first.
    if (sheet.native_rules().has_implicit_scope())
        take_in_pending_style_arrivals(const_cast<DOM::Document&>(document));
    MediaEnvironmentSnapshot environment { document };
    Parser::ValueParserFFI::NativeCompilationCallbacks callbacks {
        .context = &visit,
        .import_source = [](void const* source, u64 identity, Parser::ValueParserFFI::NativeStyleSheet const* native_sheet) -> void const* {
            auto const& sheet = *static_cast<StyleSheetState const*>(source);
            auto* import = sheet.import_for_rule(identity);
            VERIFY(import);
            auto* imported = import->loaded_style_sheet();
            VERIFY(imported && imported->native_sheet().handle() == native_sheet);
            return imported;
        },
        .implicit_scope_root = [](void const* source) { return implicit_scope_root_of(static_cast<StyleSheetState const*>(source)).value(); },
        .visit_rule = [](void const* context, void const* source, u64, RustRule::Type rule_type, Parser::ValueParserFFI::NativeCompilationContext const* compilation, Parser::ValueParserFFI::NativeCompilationResult result) { return (*static_cast<CompilationVisitor const*>(context))(rule_type, *static_cast<StyleSheetState const*>(source), *compilation, result); },
    };
    if (purpose == Parser::ValueParserFFI::NativeCompilationPurpose::Rules)
        Parser::ValueParserFFI::rust_style_sheet_compile(sheet.native_sheet().handle(), rule_identity, &sheet, environment.ffi_environment(), &callbacks, &publication);
    else
        Parser::ValueParserFFI::rust_style_sheet_replace_selectors(sheet.native_sheet().handle(), rule_identity, &sheet, environment.ffi_environment(), &callbacks, &publication);
}

struct RuleCompilationContext {
    RuleCompilationContext(StyleEngine& style_engine, SheetID sheet_handle, u64 before_rule_identity, DOM::Document const& document, StyleComputer& style_computer)
        : style_engine(style_engine)
        , sheet_handle(sheet_handle)
        , before_rule_identity(before_rule_identity)
        , document(document)
        , style_computer(style_computer)
    {
    }

    StyleEngine& style_engine;
    SheetID sheet_handle;
    // The native identity of the compiled rule the rules go before, or 0 for the end of the sheet.
    u64 before_rule_identity;
    GC::Ref<DOM::Document const> document;
    GC::Ref<StyleComputer> style_computer;
};

static void publish_layer_order_for_sheet(StyleSheetState const& sheet, DOM::Document const& document)
{
    sheet.for_each_owning_style_scope([&](StyleScope& scope) {
        if (&scope.document() == &document)
            scope.publish_cascade_layer_order();
    });
}

static void compile_rules_into(RuleCompilationContext const& context, StyleSheetState const& sheet, u64 rule_identity = 0, Parser::ValueParserFFI::NativeCompilationPurpose purpose = Parser::ValueParserFFI::NativeCompilationPurpose::Rules)
{
    Parser::ValueParserFFI::NativeStylePublication publication {
        .host = context.style_engine.host(),
        .sheet = context.sheet_handle.value(),
        .before = context.before_rule_identity,
    };
    CompilationVisitor visit = [&](RustRule::Type rule_type, StyleSheetState const& source, auto const&, auto const& result) {
        if (purpose == Parser::ValueParserFFI::NativeCompilationPurpose::Selectors && result.published)
            context.style_computer->document().bump_style_environment_version();
        if (result.declares_transitions)
            context.style_engine.note_css_transitions_may_observe_style_changes();
        if (rule_type == RustRule::Type::CounterStyle) {
            source.for_each_owning_style_scope([](StyleScope& scope) {
                scope.invalidate_counter_style_cache();
            });
        }
        if (result.published)
            context.style_computer->register_style_engine_sheet_source(source);
        return true;
    };
    visit_compilation(sheet, rule_identity, *context.document, purpose, visit, publication);
    if (purpose == Parser::ValueParserFFI::NativeCompilationPurpose::Rules)
        publish_layer_order_for_sheet(sheet, *context.document);
}

// Has the process keep the rules of `sheet`, a user agent sheet, where it does not yet. No document records them: a
// user agent sheet has no import, no scope and no owning style scope, and declares no transition.
void publish_user_agent_sheet(StyleSheetState const& sheet, DOM::Document const& document)
{
    MediaEnvironmentSnapshot environment { document };
    Parser::ValueParserFFI::NativeCompilationCallbacks callbacks {
        .context = nullptr,
        .import_source = [](void const*, u64, Parser::ValueParserFFI::NativeStyleSheet const*) -> void const* { VERIFY_NOT_REACHED(); },
        .implicit_scope_root = [](void const*) -> u32 { VERIFY_NOT_REACHED(); },
        .visit_rule = [](void const*, void const*, u64, RustRule::Type, Parser::ValueParserFFI::NativeCompilationContext const*, Parser::ValueParserFFI::NativeCompilationResult result) {
            VERIFY(!result.declares_transitions);
            return true; },
    };
    Parser::ValueParserFFI::rust_user_agent_style_sheet_publish(sheet.native_sheet().handle(), &sheet, environment.ffi_environment(), &callbacks);
}

// The sheet a rule's compiled rules belong to. An imported sheet's rules cascade in the importing
// sheet's program, so the handle to compile into is the outermost sheet's. A constructed sheet is
// always its own engine sheet: its ids are held per adopting document, so its raw id member being 0
// does not mean its rules live in another sheet's program.
static StyleSheetState* owning_compiled_sheet(StyleSheetState* sheet)
{
    while (sheet && !sheet->constructed() && sheet->style_engine_sheet_id() == 0) {
        auto* owner = sheet->owner_import();
        if (!owner)
            return nullptr;
        sheet = owner->parent_style_sheet();
    }
    return sheet;
}

static StyleSheetState* owning_compiled_sheet(CSSRule& rule)
{
    return owning_compiled_sheet(rule.parent_style_sheet());
}

// A constructed sheet compiles once per adopting document, so a mutation to it has to be replayed
// into every document engine holding a copy; every other sheet has exactly one owning document.
static void for_each_document_with_engine_copy(StyleSheetState& sheet, auto const& callback)
{
    if (sheet.constructed()) {
        GC::RootHashTable<DOM::Document*> documents;
        for (auto owner : sheet.owning_documents_or_shadow_roots())
            documents.set(&owner->document());
        for (auto* document : documents)
            callback(*document);
        // A constructed sheet remains bound to its constructor document while it is not adopted.
        // Keep that document's compiled copy synchronized so reattachment can reuse it.
        if (documents.is_empty()) {
            if (auto* document = const_cast<DOM::Document*>(sheet.constructor_document().ptr()))
                callback(*document);
        }
        return;
    }
    if (auto document = sheet.owning_document())
        callback(*document);
}

static void flush_deferred_style_change_events_for_sheet(StyleSheetState& sheet)
{
    for_each_document_with_engine_copy(sheet, [](DOM::Document& document) {
        document.flush_deferred_style_change_event();
    });
}

void flush_deferred_style_change_events_for_rule(CSSRule& rule)
{
    if (auto* sheet = owning_compiled_sheet(rule))
        flush_deferred_style_change_events_for_sheet(*sheet);
}

static bool rule_change_needs_style_environment_bump(RustRule const& rule)
{
    return Parser::ValueParserFFI::rust_rule_change_needs_style_environment_bump(rule.handle());
}

static bool sheet_can_share_compiled_style_sheet(StyleSheetState const& sheet)
{
    if (sheet.constructed() || sheet.compiled_style_sheet_is_unshareable())
        return false;
    if (sheet.owner_import() || sheet.parent_style_sheet() || !sheet.import_rules().is_empty())
        return false;
    if (!sheet.title().is_empty() || !sheet.native_rules().shared_contents_identity())
        return false;
    // NB: A link loads a sheet of its own, but the parse cache answers the same text with the same contents, as it
    //     does for style elements: components commonly link each shadow tree to one sheet.
    auto const* owner_node = sheet.owner_node();
    if (!owner_node || !(owner_node->is_html_style_element() || owner_node->is_svg_style_element() || owner_node->is_html_link_element()))
        return false;
    return !sheet.native_rules().has_implicit_scope();
}

static RefPtr<SharedCompiledStyleSheet> shared_compiled_style_sheet_for(StyleSheetState& sheet, TreeScopeID tree_scope, DOM::Document& document)
{
    if (!sheet_can_share_compiled_style_sheet(sheet))
        return nullptr;
    auto& style_computer = document.style_computer();
    auto& style_engine = style_computer.style_engine();
    SharedCompiledStyleSheetKey key { sheet.native_rules().shared_contents_identity(), sheet.style_resource_base_url().value_or(document.base_url()).serialize() };
    auto& shared_compiled_style_sheets = style_computer.shared_compiled_style_sheets();
    if (auto existing = shared_compiled_style_sheets.get(key); existing.has_value()) {
        // Each anonymous layer occurrence creates a distinct layer, even for identical contents.
        // Keep a separate program when another occurrence already occupies this scope.
        if ((*existing)->is_attached_to(tree_scope) && sheet.native_rules().has_anonymous_layer())
            return nullptr;
        if (!(*existing)->has_attachments())
            style_computer.detached_shared_compiled_style_sheets().remove_first_matching([&](auto const& detached) { return detached == key; });
        return *existing;
    }
    auto contents = StyleSheetState::create(sheet.native_rules().clone_shared_contents(), &document, RustMediaList {}, {});
    contents->set_base_url(sheet.style_resource_base_url().value_or(document.base_url()));
    auto sheet_id = style_engine.add_sheet(StyleEngineFFI::FfiCascadeOrigin::Author);
    contents->set_style_engine_sheet_id(sheet_id);
    RuleCompilationContext context { style_engine, sheet_id, 0, document, style_computer };
    compile_rules_into(context, *contents);
    contents->evaluate_media_queries(document);
    contents->load_pending_image_resources(document);
    auto shared_compiled_style_sheet = make_ref_counted<SharedCompiledStyleSheet>(move(key), move(contents), sheet_id);
    shared_compiled_style_sheets.set(shared_compiled_style_sheet->key(), shared_compiled_style_sheet);
    return shared_compiled_style_sheet;
}

static void release_shared_compiled_style_sheet(SharedCompiledStyleSheetKey const& key, StyleComputer& style_computer)
{
    auto& shared_compiled_style_sheets = style_computer.shared_compiled_style_sheets();
    auto sheet_id = shared_compiled_style_sheets.get(key).value()->sheet_id();
    auto& style_engine = style_computer.style_engine();
    StyleEngineFFI::style_engine_begin_sheet_rules_replacement(style_engine.host(), sheet_id.value());
    style_engine.finish_sheet_rules_replacement(sheet_id);
    shared_compiled_style_sheets.remove(key);
    if (shared_compiled_style_sheets.is_empty())
        shared_compiled_style_sheets.clear();
}

static void detach_shared_compiled_style_sheet(SharedCompiledStyleSheet& sheet, u64 occurrence, TreeScopeID tree_scope, StyleComputer& style_computer)
{
    auto& style_engine = style_computer.style_engine();
    StyleEngineFFI::style_engine_detach_sheet_occurrence(style_engine.host(), tree_scope, occurrence);
    sheet.remove_attachment(tree_scope);
    if (sheet.has_attachments())
        return;

    // Pages commonly insert a sheet they removed a moment ago (or a framework re-creates one with the same text).
    // Keeping the compiled sheet for a while lets that arrival attach it again instead of compiling every rule anew,
    // and since it holds the parsed contents, the parse cache answers the arriving text without parsing it again.
    auto& detached = style_computer.detached_shared_compiled_style_sheets();
    detached.append(sheet.key());
    if (detached.size() > StyleComputer::max_detached_shared_compiled_style_sheets)
        release_shared_compiled_style_sheet(detached.take_first(), style_computer);
}

bool stop_sharing_compiled_style_sheet(StyleSheetState& sheet)
{
    auto* shared_compiled_style_sheet = sheet.shared_compiled_style_sheet();
    if (!shared_compiled_style_sheet)
        return false;
    flush_deferred_style_change_events_for_sheet(sheet);
    sheet.mark_compiled_style_sheet_unshareable();
    GC::RootVector<GC::Ref<DOM::Node>> owners;
    for (auto const& owner : sheet.owning_documents_or_shadow_roots())
        owners.append(*owner);
    for (auto const& owner : owners) {
        auto tree_scope = tree_scope_of(owner);
        detach_shared_compiled_style_sheet(*shared_compiled_style_sheet, sheet.style_engine_occurrence_id(), tree_scope, owner->document().style_computer());
    }
    sheet.set_shared_compiled_style_sheet(nullptr);
    sheet.set_style_engine_sheet_id(0);
    for (auto const& owner : owners) {
        auto& style_scope = owner->is_shadow_root() ? as<DOM::ShadowRoot>(*owner).style_scope() : owner->document().style_scope();
        style_scope.attach_sheet_to_style_engine(sheet);
    }
    return true;
}

// A rule arrived in one document's engine. Compile it, and everything it brings with it, into the
// position it holds there.
static void record_style_rule_inserted_in(u64 identity, bool changes_environment, StyleSheetState& sheet, DOM::Document& document)
{
    document.flush_deferred_style_change_event();
    auto& style_computer = document.style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
    if (sheet_id == 0)
        return;

    if (changes_environment)
        document.bump_style_environment_version();

    RuleCompilationContext context {
        style_computer.style_engine(),
        sheet_id,
        StyleEngineFFI::style_engine_native_rule_successor(style_computer.style_engine().host(), sheet_id.value(), sheet.native_sheet().handle(), identity),
        document,
        style_computer
    };
    compile_rules_into(context, sheet, identity);
}

// A rule arrived. Compile it, and everything it brings with it, into the position it holds.
void record_style_rule_inserted(CSSRule& rule)
{
    if (auto* source_sheet = rule.parent_style_sheet())
        record_style_rule_inserted(rule.native_rule(), *source_sheet);
}

static void record_style_rule_inserted(u64 identity, bool changes_environment, StyleSheetState& source_sheet)
{
    auto* sheet = owning_compiled_sheet(&source_sheet);
    if (!sheet || stop_sharing_compiled_style_sheet(*sheet))
        return;
    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        record_style_rule_inserted_in(identity, changes_environment, *sheet, document);
    });
}

void record_style_rule_inserted(RustRule const& rule, StyleSheetState& source_sheet)
{
    record_style_rule_inserted(rule.identity(), rule_change_needs_style_environment_bump(rule), source_sheet);
}

void record_imported_style_sheet_loaded(u64 import_rule_identity, StyleSheetState& source_sheet)
{
    record_style_rule_inserted(import_rule_identity, true, source_sheet);
}

// A rule left. Retire the identities it compiled into, so nothing it decided keeps deciding.
//
// The sheet is named rather than asked for, because removing a rule from its list detaches it: by
// the time this runs the rule no longer knows where it was.
void record_style_rule_removed(CSSRule& rule)
{
    auto* sheet = owning_compiled_sheet(rule);
    if (!sheet)
        return;
    record_style_rule_removed(*sheet, rule.native_rule());
}

void record_style_rule_removed(StyleSheetState& sheet_it_left, RustRule const& rule, StyleSheetState const* detached_import)
{
    if (stop_sharing_compiled_style_sheet(sheet_it_left))
        return;
    // An imported sheet's rules were compiled into the sheet that imports it.
    auto* compiled_sheet = owning_compiled_sheet(&sheet_it_left);
    for_each_document_with_engine_copy(sheet_it_left, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        auto& style_computer = document.style_computer();
        struct RemovalContext {
            GC::Ref<DOM::Document> document;
            StyleSheetState& sheet;
        } context { document, sheet_it_left };
        StyleEngineFFI::style_engine_remove_native_rule(
            style_computer.style_engine().host(),
            sheet_it_left.native_sheet().handle(),
            rule.handle(),
            detached_import ? detached_import->native_sheet().handle() : nullptr,
            compiled_sheet ? style_computer.style_engine_sheet_id_for(*compiled_sheet).value() : 0,
            &context,
            [](void* opaque, bool changes_environment, bool has_counter_style) {
                auto& context = *static_cast<RemovalContext*>(opaque);
                if (changes_environment)
                    context.document->bump_style_environment_version();
                if (has_counter_style) {
                    context.sheet.for_each_owning_style_scope([&](StyleScope& scope) {
                        if (&scope.node().document() != context.document.ptr())
                            return;
                        scope.invalidate_counter_style_cache();
                    });
                }
            },
            [](void* opaque, bool declares_layer) {
                auto& context = *static_cast<RemovalContext*>(opaque);
                if (declares_layer)
                    publish_layer_order_for_sheet(context.sheet, context.document);
            });
    });
}

// A rule kept its place and its declarations, and changed what it selects.
void record_style_rule_selector_changed(CSSStyleRule& rule)
{
    auto* sheet = owning_compiled_sheet(rule);
    if (!sheet || stop_sharing_compiled_style_sheet(*sheet))
        return;

    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        auto& style_computer = document.style_computer();
        auto sheet_id = style_computer.style_engine_sheet_id_for(*sheet);
        if (sheet_id == 0)
            return;
        RuleCompilationContext context { style_computer.style_engine(), sheet_id, 0, document, style_computer };
        compile_rules_into(context, *sheet, rule.native_rule().identity(), Parser::ValueParserFFI::NativeCompilationPurpose::Selectors);
    });
}

// A rule kept its place and its selector, and changed what it declares.
void record_style_rule_declarations_changed(CSSRule& rule)
{
    if (auto* sheet = rule.parent_style_sheet())
        record_style_rule_declarations_changed(rule.native_rule(), *sheet);
}

void record_style_rule_declarations_changed(RustRule const& rule, StyleSheetState& source_sheet)
{
    auto* sheet = owning_compiled_sheet(&source_sheet);
    if (!sheet || stop_sharing_compiled_style_sheet(*sheet))
        return;

    for_each_document_with_engine_copy(*sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        struct ChangeContext {
            GC::Ref<DOM::Document> document;
            bool changes_environment;
        } context { document, rule.type() != RustRule::Type::Keyframe && rule_change_needs_style_environment_bump(rule) };
        auto& style_computer = document.style_computer();
        auto& style_engine = style_computer.style_engine();
        if (StyleEngineFFI::style_engine_native_rule_declarations_changed(
                style_engine.host(), style_computer.style_engine_sheet_id_for(*sheet).value(), rule.handle(), &context,
                [](void* opaque) {
                    auto& context = *static_cast<ChangeContext*>(opaque);
                    if (context.changes_environment)
                        context.document->bump_style_environment_version();
                }))
            style_engine.note_css_transitions_may_observe_style_changes();
    });
}

// `replace()` swaps a sheet's whole rule list, so there is nothing of the old one to keep.
void record_stylesheet_rules_replaced(StyleSheetState& sheet)
{
    if (stop_sharing_compiled_style_sheet(sheet))
        return;
    for_each_document_with_engine_copy(sheet, [&](DOM::Document& document) {
        document.flush_deferred_style_change_event();
        auto& style_computer = document.style_computer();
        auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
        if (sheet_id == 0)
            return;
        auto& style_engine = style_computer.style_engine();
        StyleEngineFFI::style_engine_begin_sheet_rules_replacement(style_engine.host(), sheet_id.value());
        RuleCompilationContext context { style_engine, sheet_id, 0, document, style_computer };
        compile_rules_into(context, sheet);
        style_engine.finish_sheet_rules_replacement(sheet_id);
    });
}

void record_stylesheet_attached(StyleSheetState& sheet, DOM::Node& document_or_shadow_root, StyleSheetState* before)
{
    document_or_shadow_root.document().flush_deferred_style_change_event();
    // The attachment may compile the sheet's rules into a shared snapshot, whose native sheet they then name.
    document_or_shadow_root.document().note_style_sheet_set_change();
    publish_document_kind(document_or_shadow_root.document());
    auto& style_computer = document_or_shadow_root.document().style_computer();
    auto& style_engine = style_computer.style_engine();
    auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
    auto first_attachment = sheet_id == 0;
    auto tree_scope = tree_scope_of(document_or_shadow_root);
    if (first_attachment) {
        if (auto shared_compiled_style_sheet = shared_compiled_style_sheet_for(sheet, tree_scope, document_or_shadow_root.document())) {
            sheet_id = shared_compiled_style_sheet->sheet_id();
            sheet.set_shared_compiled_style_sheet(move(shared_compiled_style_sheet));
        } else {
            sheet_id = style_engine.add_sheet(StyleEngineFFI::FfiCascadeOrigin::Author);
        }
        style_computer.set_style_engine_sheet_id_for(sheet, sheet_id);
    }
    // A sheet attached to a shadow root is bounded by the tree it decides in, and what bounds it is
    // that tree's root. Attaching the root to its host numbers the scope but puts nothing in it, so
    // a component that attaches and then adopts - which is the ordinary order - would name a scope
    // the engine cannot place and be answered for with the whole document.
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root))
        (void)identity_of_shadow_root(*shadow_root, style_engine);

    // Naming the successor rather than a position is what lets the engine keep order as tokens: an
    // insertion writes one label and renumbers nothing.
    StyleEngineFFI::style_engine_attach_sheet_occurrence(style_engine.host(),
        sheet_id, tree_scope, sheet.style_engine_occurrence_id(), before ? before->style_engine_occurrence_id() : 0,
        !sheet.disabled() && sheet.native_media_list().matches());
    if (first_attachment) {
        if (auto* shared_compiled_style_sheet = sheet.shared_compiled_style_sheet())
            shared_compiled_style_sheet->add_attachment(tree_scope);
    }

    // A constructed sheet can be configured before anything adopts it, so attachment is its first
    // opportunity to publish the condition state.
    if (sheet.constructed()) {
        sheet.evaluate_media_queries(document_or_shadow_root.document());
        StyleEngineFFI::style_engine_set_sheet_occurrence_conditions(style_engine.host(), tree_scope, sheet.style_engine_occurrence_id(), !sheet.disabled() && sheet.native_media_list().matches());
    }

    // Layer ranks belong to the attachment's tree scope. Publishing them while attaching keeps the
    // first transaction for a new shadow tree from matching with the document scope's order. The
    // rule-cache build is too late: it can happen while consuming that transaction's answers.
    if (auto* shadow_root = as_if<DOM::ShadowRoot>(document_or_shadow_root))
        shadow_root->style_scope().publish_cascade_layer_order(&sheet);
    else
        document_or_shadow_root.document().style_scope().publish_cascade_layer_order(&sheet);

    // A sheet arrives with a condition state, and that state is otherwise only published when it
    // moves. A constructed sheet is built disabled or given media before anything adopts it, with no
    // scope to publish either to, so an attachment is where the engine hears them - and its media
    // has never been evaluated against a document before this either. A sheet in a style sheet list
    // is announced by the list, which does this for itself.
    // A sheet's rules belong to the sheet, not to an attachment. Compiling them again when the same
    // sheet is adopted into a second scope, or moved from one to another, would give it two copies
    // of every rule.
    if (!first_attachment || sheet.shared_compiled_style_sheet())
        return;
    RuleCompilationContext context { style_engine, sheet_id, 0, document_or_shadow_root.document(), style_computer };
    compile_rules_into(context, sheet);

    // OPTIMIZATION: Until a document's first style update, the render owner has nothing else to do for it, so it takes
    //               each sheet's rules in as the sheet arrives, beside the parser, rather than the first style update
    //               waiting while it takes in every sheet the document parsed.
    if (auto& document = document_or_shadow_root.document(); document.navigable() && !document.has_completed_style_update())
        StyleEngineFFI::style_engine_stream_rule_writes(style_engine.host());
}

// The user-agent and user origins have no style sheet list to attach from, so nothing announces
// them the way an author sheet announces itself. They still carry the rules a great deal of state
// invalidation depends on: `:focus-visible` outlines come from the user-agent sheet, and a content
// blocker's `span:hover` comes from the user one. An engine that owns state invalidation without
// knowing those rules silently stops invalidating for them.
//
// The user-agent sheets are shared between documents, so their StyleEngine identities cannot live on
// the sheet object the way an author sheet's does; they are held here, per document, alongside the
// sheets they name. The user sheet is rebuilt rather than edited when content blockers change, so
// the set is compared by identity. A set that keeps the order of the sheets it had attaches only the
// sheets new to it; any other difference re-attaches the set whole.
// The key the document's user-agent and user sheets are collected under: its mode, whether it holds MathML or SVG, its
// kind, and its user sheet, which is rebuilt rather than edited. The sheets the key names are shared, or held by the
// recorded set, so an equal key names the sheets that were recorded.
static StyleComputer::NonAuthorStyleSheetsKey non_author_style_sheets_key(DOM::Document& document)
{
    auto& style_scope = document.style_scope();
    style_scope.build_user_style_sheet_if_needed();
    return {
        .in_quirks_mode = document.in_quirks_mode(),
        .needs_mathml_and_svg_user_agent_style_sheets = document.needs_mathml_and_svg_user_agent_style_sheets(),
        .is_html_document = document.document_type() == DOM::Document::Type::HTML,
        .user_style_sheet = style_scope.m_user_style_sheet.ptr(),
    };
}

// Whether the user-agent and user sheets the document's scope holds are the ones recorded with the style engine, in
// order.
bool non_author_stylesheets_are_recorded(DOM::Document const& document)
{
    auto& mutable_document = const_cast<DOM::Document&>(document);
    return mutable_document.style_computer().non_author_style_sheets_key() == non_author_style_sheets_key(mutable_document);
}

void record_non_author_stylesheets(DOM::Document& document)
{
    auto& style_computer = document.style_computer();
    auto& style_scope = document.style_scope();

    // OPTIMIZATION: The sheets follow from the key, so an unchanged key leaves nothing to collect or compare.
    auto key = non_author_style_sheets_key(document);
    if (style_computer.non_author_style_sheets_key() == key)
        return;
    ScopeGuard record_key = [&] {
        style_computer.non_author_style_sheets_key() = key;
    };

    publish_document_kind(document);

    Vector<NonnullRefPtr<StyleSheetState>> sheets;
    Vector<StyleEngineFFI::FfiCascadeOrigin> origins;
    for (auto origin : { CascadeOrigin::UserAgent, CascadeOrigin::User }) {
        style_scope.for_each_stylesheet(origin, [&](StyleSheetState& sheet) {
            sheets.append(sheet);
            origins.append(origin == CascadeOrigin::UserAgent ? StyleEngineFFI::FfiCascadeOrigin::UserAgent : StyleEngineFFI::FfiCascadeOrigin::User);
        });
    }

    auto& recorded = style_computer.non_author_style_sheets();
    auto recorded_sheets_match = [&] {
        if (recorded.size() != sheets.size())
            return false;
        for (size_t index = 0; index < sheets.size(); ++index) {
            if (recorded[index].sheet != sheets[index])
                return false;
        }
        return true;
    };
    if (recorded_sheets_match())
        return;

    document.flush_deferred_style_change_event();
    if (recorded_sheets_match())
        return;

    auto& style_engine = style_computer.style_engine();

    // These origins cascade before every author sheet, so each is inserted ahead of the first one
    // rather than appended. Inserting each new sheet before that same successor keeps them in the
    // order they were collected.
    SheetID first_author_sheet;
    for (auto const& sheet : document.style_scope().style_sheets()) {
        if (sheet->style_engine_sheet_id() != 0) {
            first_author_sheet = sheet->style_engine_sheet_id();
            break;
        }
    }

    auto attach_sheet = [&](size_t index, SheetID successor) {
        // A user agent sheet's rules are the process's: the first document needing the sheet has them compiled, and
        // each document only notes what its media leave out of them.
        if (origins[index] == StyleEngineFFI::FfiCascadeOrigin::UserAgent) {
            publish_user_agent_sheet(*sheets[index], document);
            SheetID sheet_id { StyleEngineFFI::style_engine_add_user_agent_sheet(style_engine.host(), sheets[index]->native_sheet().identity()) };
            StyleEngineFFI::style_engine_attach_sheet(style_engine.host(), sheet_id, document_tree_scope, successor);
            style_computer.register_style_engine_sheet_source(*sheets[index]);
            MediaEnvironmentSnapshot environment { document };
            Parser::ValueParserFFI::rust_style_sheet_publish_failing_conditions(sheets[index]->native_sheet().handle(), style_engine.host(), environment.ffi_environment());
            return StyleComputer::NonAuthorStyleSheet { sheets[index], sheet_id };
        }
        auto sheet_id = style_engine.add_sheet(origins[index]);
        StyleEngineFFI::style_engine_attach_sheet(style_engine.host(), sheet_id, document_tree_scope, successor);
        RuleCompilationContext context { style_engine, sheet_id, 0, document, style_computer };
        compile_rules_into(context, *sheets[index]);
        return StyleComputer::NonAuthorStyleSheet { sheets[index], sheet_id };
    };

    // OPTIMIZATION: A document takes in the MathML and SVG user-agent sheets once it first holds such an element, and
    //               a new user sheet replaces the old one. The sheets recorded before that stay keep their places
    //               among them, so only the sheets that left are detached and the new ones attached, each before the
    //               sheet that follows it, rather than detaching every sheet and routing all of their rules again.
    auto recorded_index_of = [&](StyleSheetState const& sheet) -> Optional<size_t> {
        for (size_t index = 0; index < recorded.size(); ++index) {
            if (recorded[index].sheet.ptr() == &sheet)
                return index;
        }
        return {};
    };
    auto recorded_sheets_keep_their_order = [&] {
        Optional<size_t> previous;
        for (auto const& sheet : sheets) {
            auto index = recorded_index_of(*sheet);
            if (!index.has_value())
                continue;
            if (previous.has_value() && *index < *previous)
                return false;
            previous = index;
        }
        return true;
    };
    if (recorded_sheets_keep_their_order()) {
        for (auto const& entry : recorded) {
            auto stays = false;
            for (auto const& sheet : sheets)
                stays |= sheet.ptr() == entry.sheet.ptr();
            if (!stays)
                StyleEngineFFI::style_engine_detach_sheet(style_engine.host(), entry.sheet_id, document_tree_scope);
        }
        Vector<StyleComputer::NonAuthorStyleSheet> updated;
        for (size_t index = 0; index < sheets.size(); ++index) {
            if (auto recorded_index = recorded_index_of(*sheets[index]); recorded_index.has_value()) {
                updated.append(recorded[*recorded_index]);
                continue;
            }
            auto successor = first_author_sheet;
            for (size_t next = index + 1; next < sheets.size(); ++next) {
                if (auto recorded_index = recorded_index_of(*sheets[next]); recorded_index.has_value()) {
                    successor = recorded[*recorded_index].sheet_id;
                    break;
                }
            }
            updated.append(attach_sheet(index, successor));
        }
        recorded = move(updated);
        return;
    }

    for (auto const& entry : recorded)
        StyleEngineFFI::style_engine_detach_sheet(style_engine.host(), entry.sheet_id, document_tree_scope);
    recorded.clear();
    for (size_t index = 0; index < sheets.size(); ++index)
        recorded.append(attach_sheet(index, first_author_sheet));
}

void record_stylesheet_rule_conditions(StyleSheetState& sheet)
{
    auto* engine_sheet = owning_compiled_sheet(&sheet);
    if (!engine_sheet)
        return;
    for_each_document_with_engine_copy(*engine_sheet, [&](DOM::Document& document) {
        record_stylesheet_rule_conditions(sheet, document);
    });
}

void record_stylesheet_rule_conditions(StyleSheetState& sheet, DOM::Document& document)
{
    auto* engine_sheet = sheet.owner_import() ? owning_compiled_sheet(&sheet) : &sheet;
    if (!engine_sheet)
        return;
    document.flush_deferred_style_change_event();
    auto& style_computer = document.style_computer();
    // Imported rules inherit the conditions of every enclosing import. Starting at an imported
    // sheet would lose those gates and could re-enable rules beneath a non-matching import.
    MediaEnvironmentSnapshot environment { document };
    Parser::ValueParserFFI::rust_style_sheet_publish_conditions(
        engine_sheet->native_sheet().handle(), style_computer.style_engine().host(), environment.ffi_environment());
}

void record_stylesheet_conditions(StyleSheetState& sheet, DOM::Node& document_or_shadow_root, bool conditions_hold)
{
    document_or_shadow_root.document().flush_deferred_style_change_event();
    auto* engine_sheet = owning_compiled_sheet(&sheet);
    if (!engine_sheet)
        return;
    // An imported sheet gates only its own rules, not the entire enclosing engine sheet.
    if (engine_sheet != &sheet) {
        record_stylesheet_rule_conditions(*engine_sheet, document_or_shadow_root.document());
        return;
    }
    auto& style_computer = document_or_shadow_root.document().style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(*engine_sheet);
    if (sheet_id == 0)
        return;
    StyleEngineFFI::style_engine_set_sheet_occurrence_conditions(style_computer.style_engine().host(), tree_scope_of(document_or_shadow_root), sheet.style_engine_occurrence_id(), conditions_hold);
}

void record_stylesheet_detached(StyleSheetState& sheet, DOM::Node& document_or_shadow_root)
{
    document_or_shadow_root.document().flush_deferred_style_change_event();
    document_or_shadow_root.document().note_style_sheet_set_change();
    auto& style_computer = document_or_shadow_root.document().style_computer();
    auto sheet_id = style_computer.style_engine_sheet_id_for(sheet);
    if (sheet_id == 0)
        return;
    auto tree_scope = tree_scope_of(document_or_shadow_root);
    auto* shared_compiled_style_sheet = sheet.shared_compiled_style_sheet();
    if (!shared_compiled_style_sheet) {
        StyleEngineFFI::style_engine_detach_sheet_occurrence(style_computer.style_engine().host(), tree_scope, sheet.style_engine_occurrence_id());
        return;
    }
    detach_shared_compiled_style_sheet(*shared_compiled_style_sheet, sheet.style_engine_occurrence_id(), tree_scope, style_computer);
    if (sheet.owning_documents_or_shadow_roots().is_empty()) {
        sheet.set_shared_compiled_style_sheet(nullptr);
        sheet.set_style_engine_sheet_id(0);
    }
}

// Every boolean pseudo-class the parser can produce has a fact, so the switch is exhaustive over
// them. The pseudo-classes with no entry are the ones StyleEngine models as operators rather than
// facts -- positional, logical, structural, and the parameterized ones -- and those change with
// inputs the engine already routes, not with a state transition published here.
#include <LibWeb/StyleEngineStateFactsGenerated.inc>

bool element_validity_is_withheld(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || !style_engine->validity_is_known_untested())
        return false;
    style_engine->note_validity_state_withheld();
    return true;
}

bool can_record_element_state_change(DOM::Element& element)
{
    // A state an element waiting in a display:none subtree takes matters where a `:has()` argument may reach the
    // element by a state, for record_element_state_changed() to release the children waiting with it.
    if (element.style_node_id() == no_style_node) {
        auto* style_engine = style_engine_for(element);
        if (!style_engine)
            return false;
        auto const& keys = style_engine->last_relational_arrival_keys();
        return (keys.reached_by_any_arrival || !keys.state_pseudo_classes.is_empty()) && element_whose_waiting_children_hold(element);
    }
    return style_engine_for_published(element);
}

void move_style_hover(DOM::Document& document, GC::Ptr<DOM::Node> node)
{
    if (!document.layout_node_arena_if_created())
        return;
    auto const* element = hovered_element_of(node.ptr());
    Optional<StyleNodeID> target;
    if (element && element->style_node_id() != no_style_node)
        target = element->style_node_id();
    document.style_computer().style_engine().record_hover(target);
}

void record_element_state_changed(DOM::Element& element, PseudoClass pseudo_class, bool new_value)
{
    auto fact = state_fact_for(pseudo_class);
    if (!fact.has_value())
        return;
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine) {
        record_waiting_element_state_changed(element, pseudo_class);
        return;
    }

    style_engine->record_state_delta({
        .node = element.style_node_id().value(),
        .fact = *fact,
        .new_value = new_value,
    });
}

static void record_feature(
    DOM::Element& element,
    StyleEngineFFI::FfiFeatureKind kind,
    StyleAtomID name_atom,
    StyleEngineFFI::FfiFeatureValueKind new_kind,
    StyleAtomID new_atom)
{
    auto* style_engine = style_engine_for_published(element);
    if (!style_engine)
        return;

    style_engine->record_local_feature_delta(
        {
            .node = element.style_node_id().value(),
            .feature_kind = kind,
            .name_atom = name_atom.value(),
            .new_kind = new_kind,
            .new_atom = new_atom.value(),
        });
}

void record_element_id_changed(DOM::Element& element, Optional<Utf16FlyString> const& new_value)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine) {
        if (new_value.has_value())
            record_waiting_element_names_changed(element, { &*new_value, 1 });
        return;
    }

    auto atom_of = [&](Optional<Utf16FlyString> const& value) -> StyleAtomID {
        return value.has_value() ? intern_id_or_class_atom(*style_engine, element, *value) : 0;
    };
    auto kind_of = [](Optional<Utf16FlyString> const& value) {
        return value.has_value() ? StyleEngineFFI::FfiFeatureValueKind::Atom : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };

    record_feature(element, StyleEngineFFI::FfiFeatureKind::Id, 0, kind_of(new_value), atom_of(new_value));

    // `getElementById` is case-sensitive in every mode, so the name the inverse index is keyed by
    // is the one written rather than the one a quirks-mode selector folds it to.
    StyleEngineFFI::style_engine_set_element_id_name(style_engine->host(), element.style_node_id(), new_value.has_value() ? style_engine->intern_atom(*new_value) : StyleAtomID {});

    if (element.is_svg_geometry_element())
        style_engine->record_derived_element_style_input_change(element.style_node_id(), StyleEngine::PublishedStyle | StyleEngine::RecomputeStyle);
}

void record_element_class_list_changed(DOM::Element& element, ReadonlySpan<Utf16FlyString> old_classes, ReadonlySpan<Utf16FlyString> new_classes)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine)
        return;

    // One class delta per class that actually gained or lost membership. A class present on both
    // sides is not a change, and journalling it would be exactly the amplification the engine
    // exists to avoid.
    // Two class names that differ only in case are one class where the document folds them, so
    // membership is decided between the folded names rather than the written ones.
    Vector<Utf16FlyString> folded_old_classes;
    Vector<Utf16FlyString> folded_new_classes;
    if (element.document().in_quirks_mode()) {
        auto fold = [](ReadonlySpan<Utf16FlyString> names, Vector<Utf16FlyString>& folded_names) {
            folded_names.ensure_capacity(names.size());
            for (auto const& name : names)
                folded_names.unchecked_append(name.to_ascii_lowercase());
        };
        fold(old_classes, folded_old_classes);
        fold(new_classes, folded_new_classes);
        old_classes = folded_old_classes;
        new_classes = folded_new_classes;
    }
    auto contains = [](ReadonlySpan<Utf16FlyString> names, Utf16FlyString const& name) {
        return any_of(names, [&](auto const& other) { return other == name; });
    };

    auto record_membership = [&](Utf16FlyString const& folded_name, bool was_present, bool is_present) {
        if (was_present == is_present)
            return;
        auto atom = style_engine->intern_atom(folded_name);
        record_feature(
            element,
            StyleEngineFFI::FfiFeatureKind::Class,
            atom,
            is_present ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent,
            0);
    };

    for (auto const& name : old_classes)
        record_membership(name, true, contains(new_classes, name));
    for (auto const& name : new_classes) {
        if (!contains(old_classes, name))
            record_membership(name, false, true);
    }
}

void record_element_attribute_changed(DOM::Element& element, Utf16FlyString const& name, Optional<Utf16FlyString> const& namespace_uri, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value)
{
    auto* style_engine = style_engine_for_identified(element);
    if (!style_engine) {
        if (!old_value.has_value() && new_value.has_value())
            record_waiting_element_names_changed(element, { &name, 1 }, NamesFold::Always);
        return;
    }
    if (!old_value.has_value() && !new_value.has_value())
        return;

    // These two are what a heading level counts, and they answer for every heading beneath them.
    if (name == HTML::AttributeNames::headingoffset || name == HTML::AttributeNames::headingreset)
        record_heading_levels_in_subtree(element);

    // Attribute presence decides whether the element cascades presentational hints. Changing
    // only a value cannot move that fact, except for an input's type, which also decides its
    // box adjustments and whether it supports dimension attributes.
    if (old_value.has_value() != new_value.has_value() || name == HTML::AttributeNames::type)
        record_element_adjustment_facts(element);

    // What the replaced content of a form control or a canvas is sized from.
    if ((is<HTML::HTMLTextAreaElement>(element) && (name == HTML::AttributeNames::cols || name == HTML::AttributeNames::rows))
        || (is<HTML::HTMLInputElement>(element) && (name == HTML::AttributeNames::size || name == HTML::AttributeNames::type))
        || (is<HTML::HTMLCanvasElement>(element) && (name == HTML::AttributeNames::width || name == HTML::AttributeNames::height)))
        record_element_replaced_content_input(element);
    // A table's border attribute moves its cells' border hints.
    if (name == HTML::AttributeNames::border && is<HTML::HTMLTableElement>(element)) {
        element.for_each_in_subtree_of_type<HTML::HTMLTableCellElement>([](auto& cell) {
            republish_presentational_hints(cell);
            return TraversalDecision::Continue;
        });
    }

    // The new value crosses as an atom. Its text is recorded once per distinct value only when a
    // compiled selector for this attribute uses an operator that cannot compare atom identities,
    // or when an attr() can read the name, so the match evaluator reads it without asking the DOM.
    // The same name an arriving attribute publishes, with the same other forms noted alongside it.
    // See `StyleEngine::intern_attribute_name`.
    auto atom = style_engine->intern_attribute_name(name, namespace_uri);
    // OPTIMIZATION: Nothing of the engine reads this name, so the element's row need not hear of the change. The element
    //               is noted as an arrival withholding the name is, and is published if something comes to read it.
    if (style_engine->attribute_is_known_unread(atom)) {
        style_engine->note_attribute_withheld(name, namespace_uri);
        return;
    }
    auto kind_of = [](Optional<Utf16String> const& value) {
        return value.has_value() ? StyleEngineFFI::FfiFeatureValueKind::Atom : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };
    auto atom_of = [&](Optional<Utf16String> const& value) {
        return value.has_value() ? style_engine->intern_attribute_value(atom, *value) : 0;
    };
    auto new_kind = kind_of(new_value);
    auto new_atom = atom_of(new_value);
    record_feature(
        element,
        StyleEngineFFI::FfiFeatureKind::Attribute,
        atom,
        new_kind,
        new_atom);
}

// OPTIMIZATION: A document records its user-agent and user sheets once it knows its mode, which decides which user-agent
//               sheets it takes and how their selectors compile, rather than in its first style update. The render
//               owner takes their rules in beside the parser, compiling the selectors of the user agent rules the
//               process has not compiled yet, so the first style update does not wait on it.
void prepare_non_author_stylesheets(DOM::Document& document)
{
    // NB: A document nothing renders, as DOMParser makes, never styles anything.
    if (!document.navigable())
        return;
    record_non_author_stylesheets(document);
    StyleEngineFFI::style_engine_stream_rule_writes(document.style_computer().style_engine().host());
}

}
