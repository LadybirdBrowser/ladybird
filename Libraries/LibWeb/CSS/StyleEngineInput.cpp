/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashTable.h>
#include <AK/QuickSort.h>
#include <AK/SetUnion.h>
#include <AK/TemporaryChange.h>
#include <LibWeb/Animations/KeyframeEffect.h>
#include <LibWeb/CSS/CSSAnimation.h>
#include <LibWeb/CSS/CSSPropertyRule.h>
#include <LibWeb/CSS/CSSStyleRule.h>
#include <LibWeb/CSS/ElementBoxKind.h>
#include <LibWeb/CSS/Invalidation/LanguageInvalidator.h>
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
#include <LibWeb/HTML/HTMLObjectElement.h>
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
static void record_element_initial_features(DOM::Element&);
static void record_element_inline_style_properties(DOM::Element&);
static void record_heading_levels_in_subtree(DOM::Element&);
static void republish_assigned_slot_of(DOM::Node&);
static Optional<StyleEngineFFI::FfiStateFact> state_fact_for(PseudoClass);
static StyleAtomID intern_id_or_class_atom(StyleEngine&, DOM::Element const&, Utf16FlyString const&);

static constexpr StyleNodeID no_style_node;
// Shadow trees get their own scopes with the shadow surface; today everything names the document.
static constexpr TreeScopeID document_tree_scope;

static bool has_pending_initial_features(DOM::Element const& element)
{
    return element.document().style_computer().style_engine().has_deferred_element_initial_features(element.style_node_id());
}

static StyleEngine* style_engine_for(DOM::Node& node)
{
    if (!node.is_connected() || !node.document().style_engine_tracks_tree())
        return nullptr;
    return &node.document().style_computer().style_engine();
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

static StyleEngineFFI::FfiTreeRelations relations_of(DOM::Element& element, StyleEngine& style_engine, TreeScopeID tree_scope)
{
    auto assigned_slot = no_style_node;
    if (auto slot = element.assigned_slot_internal())
        assigned_slot = slot->style_node_id();

    return StyleEngineFFI::FfiTreeRelations {
        .parent = style_tree_parent_of(element, style_engine).value(),
        .previous_element_sibling = identity_of_arrived_previous_sibling(element.previous_element_sibling()).value(),
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
    StyleEngineFFI::FfiFeatureValueKind old_kind,
    StyleAtomID old_atom,
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

static void record_element_arrival_delta(DOM::Element& element, StyleEngine& style_engine, TreeScopeID tree_scope)
{
    // A shadow root that took its identity before its host had one is still waiting to be linked to
    // it. A sheet adopted into a shadow tree names that root, so the root can be identified first,
    // and the link is what lets a `:host` or `::slotted()` rule in that tree reach the host instead
    // of the document.
    if (auto shadow_root = element.shadow_root(); shadow_root && shadow_root->style_node_id() != no_style_node)
        StyleEngineFFI::style_engine_set_shadow_root(style_engine.host(), element.style_node_id(), shadow_root->style_node_id());
    style_engine.record_tree_delta({
        .node = element.style_node_id().value(),
        .old_connected = false,
        .new_connected = true,
        .old_relations = detached_relations(),
        .new_relations = relations_of(element, style_engine, tree_scope),
    });
    style_engine.defer_element_initial_features(element.style_node_id());

    // A slot can take its identity after the nodes assigned to it took theirs - a shadow tree built
    // from markup assigns before the slot connects - and a slottable's assignment is published as
    // the slot's identity, so it was published as no slot at all. Publishing it again from the
    // slot's own arrival is what lets `::slotted()` be answered.
    if (auto* slot = as_if<HTML::HTMLSlotElement>(element)) {
        for (auto const& slottable : slot->assigned_nodes_internal()) {
            if (auto const* assigned = slottable.get_pointer<GC::Ref<DOM::Element>>())
                record_element_assigned_slot_changed(**assigned, nullptr);
        }
    }
}

void publish_pending_element_features(StyleEngine& style_engine, StyleComputer& style_computer)
{
    auto nodes = style_engine.take_deferred_element_initial_features().values();
    // Feature postings are ordered by node identity. Publish arrivals in that order so growing
    // their indexes appends members instead of repeatedly searching and splitting posting chunks.
    quick_sort(nodes);
    for (auto node : nodes) {
        auto element = style_computer.element_for_style_node(node);
        if (!element)
            continue;
        // An inserted subtree takes its identities before its insertion steps mark its nodes connected, in tree order.
        // A step that submits the recorded input on the way, as a <style> writing its sheet to the engine in place
        // while a style transaction flies, finds the elements after it unconnected: their arrivals are submitted, and
        // their features wait for the next submit, which folds them onto those arrivals.
        if (!element->is_connected()) {
            style_engine.defer_element_initial_features(node);
            continue;
        }
        record_element_initial_features(*element);
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
    node.set_style_arrival_pending(true);
    for (auto* ancestor = node.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (ancestor->descendant_style_arrival_pending() || ancestor->style_arrival_pending())
            break;
        ancestor->set_descendant_style_arrival_pending(true);
    }
}

// Whether a subtree already waiting to arrive holds the node. An element without an identity is always in one.
static bool waits_to_arrive_with_an_ancestor(DOM::Node& node)
{
    for (auto* ancestor = node.parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
        if (ancestor->style_arrival_pending())
            return true;
        if (auto* element = as_if<DOM::Element>(*ancestor))
            return element->style_node_id() == no_style_node;
        if (!is<DOM::ShadowRoot>(*ancestor))
            return false;
    }
    return false;
}

static void record_node_connected(DOM::Node& node, StyleEngine& style_engine)
{
    if (waits_to_arrive_with_an_ancestor(node))
        style_engine.note_pending_arrivals(1);
    else
        mark_style_arrival_pending(node, style_engine);
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
    style_engine->set_text_data(text.style_node_id(), text.data());
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

// Every node in the subtrees waiting to arrive, in tree order, takes its identity, and every element records its
// arrival. The subtrees are taken in together, so each identity is assigned before the first arrival is recorded:
// an arrival names its parent and siblings, and a sibling that waited in another subtree has arrived by then.
static void record_subtree_arrivals(DOM::Document& document, ReadonlySpan<GC::Ref<DOM::Node>> roots)
{
    auto& style_computer = document.style_computer();
    auto& style_engine = style_computer.style_engine();
    struct Arrival {
        GC::Ref<DOM::Node> node;
        TreeScopeID tree_scope;
    };
    Vector<Arrival, 64> arrivals;
    Vector<GC::Ref<DOM::Text>, 64> text_arrivals;
    // Elements and text nodes in tree order, which is the order their places in the DOM child sequence can be taken
    // in: each node's previous sibling has taken its place first.
    Vector<GC::Ref<DOM::Node>, 64> dom_order_arrivals;
    size_t element_count = 0;
    auto collect = [&](DOM::Node& node, TreeScopeID tree_scope) {
        node.set_style_arrival_pending(false);
        node.set_descendant_style_arrival_pending(false);
        if (auto* element = as_if<DOM::Element>(node); element && element->style_node_id() == no_style_node) {
            arrivals.append({ *element, tree_scope });
            dom_order_arrivals.append(*element);
            ++element_count;
        } else if (auto* shadow_root = as_if<DOM::ShadowRoot>(node); shadow_root && shadow_root->style_node_id() == no_style_node) {
            arrivals.append({ *shadow_root, tree_scope });
        } else if (auto* text = as_if<DOM::Text>(node); text && text->style_node_id() == no_style_node) {
            text_arrivals.append(*text);
            dom_order_arrivals.append(*text);
        }
    };
    for (auto const& root : roots) {
        ensure_dom_order_parent_identity(root->parent(), style_engine);
        for_each_shadow_including_inclusive_descendant_with_scope(*root, tree_scope_of(root->root()), collect);
    }

    if (!text_arrivals.is_empty()) {
        Vector<StyleNodeID, 64> identities;
        identities.resize(text_arrivals.size());
        style_engine.mint_text_style_nodes(identities.span());
        style_computer.ensure_style_node_slot(identities.last());
        for (size_t i = 0; i < text_arrivals.size(); ++i) {
            text_arrivals[i]->set_style_node_id(identities[i]);
            style_computer.register_style_node(identities[i], text_arrivals[i]);
            StyleEngineFFI::style_engine_set_text_is_ascii_whitespace(style_engine.host(), identities[i], text_arrivals[i]->data().is_ascii_whitespace());
            StyleEngineFFI::style_engine_set_text_is_in_user_agent_shadow_tree(style_engine.host(), identities[i], text_is_in_user_agent_shadow_tree(*text_arrivals[i]));
            StyleEngineFFI::style_engine_set_text_is_password_input(style_engine.host(), identities[i], text_arrivals[i]->is_password_input());
            style_engine.set_text_data(identities[i], text_arrivals[i]->data());
        }
    }

    if (!arrivals.is_empty()) {
        Vector<StyleNodeID, 64> identities;
        identities.resize(arrivals.size());
        style_engine.mint_style_nodes(identities.span());
        style_computer.ensure_style_node_slot(identities.last());
        size_t next_element_identity = 0;
        size_t next_shadow_root_identity = element_count;
        for (auto const& arrival : arrivals) {
            if (auto* element = as_if<DOM::Element>(*arrival.node)) {
                auto identity = identities[next_element_identity++];
                element->set_style_node_id(identity);
                style_computer.register_style_node(identity, *element);
                StyleEngineFFI::style_engine_set_element_unique_node_id(style_engine.host(), identity, static_cast<u64>(element->unique_id().value()));
                Layout::publish_table_spans(*element);
                if (element->style_recomputes_on_environment_move())
                    element->publish_style_recomputes_on_environment_move();
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

        for (auto const& arrival : arrivals) {
            if (auto* element = as_if<DOM::Element>(*arrival.node))
                record_element_arrival_delta(*element, style_engine, arrival.tree_scope);
        }
    }

    if (dom_order_arrivals.is_empty())
        return;
    Vector<u32, 192> links;
    links.ensure_capacity(dom_order_arrivals.size() * 3);
    for (auto const& node : dom_order_arrivals)
        append_dom_order_link(links, node);
    StyleEngineFFI::style_engine_link_style_nodes_in_dom_order(style_engine.host(), links.span());

    for (auto const& node : dom_order_arrivals)
        republish_assigned_slot_of(node);

    // What the insertion steps publish about a node under its identity waited for the identity (see Node::inserted()).
    auto focused_area = document.focused_area();
    auto const* focused_text_control = is<HTML::FormAssociatedTextControlElement>(focused_area.ptr()) ? focused_area.ptr() : nullptr;
    for (auto const& node : dom_order_arrivals) {
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
        auto const* parent = root->parent();
        if (parent && (parent->is_html_style_element() || parent->is_svg_style_element()) && !parent->has_layout_box())
            continue;
        root->set_needs_layout_tree_update(true, DOM::SetNeedsLayoutTreeUpdateReason::NodeInsertBefore);
    }
}

// The subtrees waiting to arrive, in tree order, found along the marks their ancestors carry. The marks are cleared on
// the way: a subtree that left the tree before this walk leaves marks behind that nothing waits under.
static void collect_pending_style_arrival_roots(DOM::Node& node, Vector<GC::Ref<DOM::Node>, 16>& roots)
{
    node.set_descendant_style_arrival_pending(false);
    auto visit = [&](DOM::Node& child) {
        if (child.style_arrival_pending())
            roots.append(child);
        else if (child.descendant_style_arrival_pending())
            collect_pending_style_arrival_roots(child, roots);
    };
    if (auto* element = as_if<DOM::Element>(node); element && element->shadow_root())
        visit(*element->shadow_root());
    for (auto* child = node.first_child(); child; child = child->next_sibling())
        visit(*child);
}

static bool s_taking_in_pending_style_arrivals = false;

void take_in_pending_style_arrivals(DOM::Document& document)
{
    if (s_taking_in_pending_style_arrivals)
        return;
    // Nodes that waited and left the tree again are counted too, and nothing is left of them to take in.
    document.style_computer().style_engine().forget_pending_arrivals();
    if (!document.descendant_style_arrival_pending())
        return;
    TemporaryChange taking_in { s_taking_in_pending_style_arrivals, true };
    Vector<GC::Ref<DOM::Node>, 16> roots;
    collect_pending_style_arrival_roots(document, roots);
    if (!roots.is_empty() && document.style_engine_tracks_tree())
        record_subtree_arrivals(document, roots);
}

// Publish every selector-visible fact intrinsic to one element.
// The element-backed pseudo-element an element in its host's shadow tree stands for, as one plus
// its kind, or zero.
static u8 associated_pseudo_kind_plus_one(DOM::Element const& element)
{
    auto pseudo_element = element.associated_shadow_host_pseudo_element();
    return pseudo_element.has_value() ? static_cast<u8>(to_underlying(*pseudo_element) + 1) : 0;
}

template<typename PublishFeature, typename PublishEmptiness>
static void publish_element_selector_features(StyleEngine& style_engine, DOM::Element& element, StyleNodeID node, PublishFeature publish_feature, PublishEmptiness publish_emptiness)
{
    // Slot identity and namespace never change during an element's lifetime.
    auto is_slot = is<HTML::HTMLSlotElement>(element);
    StyleAtomID namespace_atom;
    if (auto const& namespace_uri = element.namespace_uri(); namespace_uri.has_value() && !namespace_uri->is_empty())
        namespace_atom = style_engine.intern_atom(*namespace_uri);

    publish_feature(StyleEngineFFI::FfiFeatureKind::TagName, StyleAtomID {}, StyleEngineFFI::FfiFeatureValueKind::Atom, style_engine.intern_atom(element.local_name()));
    if (auto folded_name = element.local_name().to_ascii_lowercase(); folded_name != element.local_name())
        publish_feature(StyleEngineFFI::FfiFeatureKind::FoldedTagName, StyleAtomID {}, StyleEngineFFI::FfiFeatureValueKind::Atom, style_engine.intern_atom(folded_name));
    if (auto const& id = element.id(); id.has_value())
        publish_feature(StyleEngineFFI::FfiFeatureKind::Id, StyleAtomID {}, StyleEngineFFI::FfiFeatureValueKind::Atom, intern_id_or_class_atom(style_engine, element, *id));
    for (auto const& class_name : element.class_names())
        publish_feature(StyleEngineFFI::FfiFeatureKind::Class, intern_id_or_class_atom(style_engine, element, class_name), StyleEngineFFI::FfiFeatureValueKind::Present, StyleAtomID {});
    element.for_each_attribute([&](DOM::QualifiedName const& name, Utf16String const& value) {
        auto name_atom = style_engine.intern_attribute_name(name.local_name(), name.namespace_());
        publish_feature(StyleEngineFFI::FfiFeatureKind::Attribute, name_atom, StyleEngineFFI::FfiFeatureValueKind::Atom, style_engine.intern_attribute_value(name_atom, value));
    });

    bool has_nonempty_text_child = false;
    for (GC::Ptr<DOM::Node const> child = element.first_child(); child; child = child->next_sibling()) {
        if (GC::Ptr<DOM::Text const> text = as_if<DOM::Text>(*child); text && !text->data().is_empty()) {
            has_nonempty_text_child = true;
            break;
        }
    }
    publish_emptiness(has_nonempty_text_child);

    auto states = SelectorMatching::element_states(element);
    for (size_t index = 0; index < to_underlying(PseudoClass::__Count); ++index) {
        auto pseudo_class = static_cast<PseudoClass>(index);
        if (!states.get(pseudo_class))
            continue;
        auto fact = state_fact_for(pseudo_class);
        if (fact.has_value())
            style_engine.record_state_delta({ .node = node.value(), .fact = *fact, .new_value = true });
    }

    // The tag is computed afresh and not left cached. One cached where the element used to be says nothing about where
    // it arrives, as a subtree that moves while detached is never reached by the walk a `lang` change runs, and one
    // read now may not be final, as an XML parser sets attributes after insertion.
    element.invalidate_lang_value();
    auto const language = element.lang_view();
    auto language_atom = language.has_value() ? style_engine.intern_language_atom(*language) : StyleAtomID {};
    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? "rtl"_utf16_fly_string : "ltr"_utf16_fly_string;
    auto directionality_atom = style_engine.intern_atom(directionality);
    element.invalidate_lang_value();

    GC::Ptr<HTML::HTMLHeadingElement const> heading = as_if<HTML::HTMLHeadingElement>(element);
    auto heading_level = static_cast<u8>(min(heading ? heading->heading_level() : 0, 255u));

    Vector<StyleAtomID> custom_states;
    if (auto states = element.custom_state_set()) {
        for (auto const& state : states->states())
            custom_states.append(style_engine.intern_atom(state));
    }
    style_engine.record_element_arrival({
                                            .node = node.value(),
                                            .namespace_atom = namespace_atom.value(),
                                            .language_atom = language_atom.value(),
                                            .directionality_atom = directionality_atom.value(),
                                            .custom_state_offset = 0,
                                            .custom_state_count = 0,
                                            .heading_level = heading_level,
                                            .is_slot = is_slot,
                                            .associated_pseudo_kind_plus_one = associated_pseudo_kind_plus_one(element),
                                            .box_kind = to_underlying(element.box_kind()),
                                            .adjustment_facts = element_style_adjustment_facts(element),
                                            .construction_facts = element_construction_facts(element),
                                        },
        custom_states);
}

// An element's hints move when something beside its own attributes that they are mapped from
// moves: the table a cell is under, the <source> an image takes its dimensions from, the body's
// link colours for a link, an input entering or leaving the Image Button state. Publish them again.
void republish_presentational_hints(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
    StyleComputer::collect_presentational_hint_properties({ element });
    // The hints moved, and so does the style they are cascaded into.
    record_element_declarations_changed(element, ElementDeclarationKind::SvgPresentationAttribute, true, true);
}

static bool element_has_presentational_hints_to_publish(DOM::Element const& element)
{
    if (element.publishes_presentational_hints_on_arrival())
        return true;
    // A table cell can take hints from its table's border and cellpadding, and an image from its
    // <picture>'s <source>, without a presentational attribute of its own.
    if (element.namespace_uri() == Namespace::HTML && first_is_one_of(element.local_name(), HTML::TagNames::td, HTML::TagNames::th, HTML::TagNames::img))
        return true;
    // A body's link, vlink and alink attributes are presentational hints on every link, by the
    // link's :link, :visited and :active state.
    if ((element.matches_link_pseudo_class() || element.matches_visited_pseudo_class())
        && (element.document().normal_link_color().has_value() || element.document().visited_link_color().has_value() || element.document().active_link_color().has_value()))
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
    bool has_presentational_hint = false;
    element.for_each_attribute([&](Utf16FlyString const& name, Utf16View) {
        if (!has_presentational_hint && element.is_presentational_hint(name))
            has_presentational_hint = true;
    });
    return has_presentational_hint;
}

u32 element_box_type_adjustment_facts(DOM::Element const& element)
{
    bool is_html_element = element.namespace_uri() == Namespace::HTML;
    bool is_svg_element = element.namespace_uri() == Namespace::SVG;
    bool is_mathml_element = element.namespace_uri() == Namespace::MathML;
    auto local_name = element.local_name();

    bool input_allows_adjustment = false;
    bool input_is_single_line = false;
    if (is<HTML::HTMLInputElement>(element)) {
        auto const& input = static_cast<HTML::HTMLInputElement const&>(element);
        input_allows_adjustment = !first_is_one_of(
            input.type_state(),
            HTML::HTMLInputElement::TypeAttributeState::Hidden,
            HTML::HTMLInputElement::TypeAttributeState::SubmitButton,
            HTML::HTMLInputElement::TypeAttributeState::Button,
            HTML::HTMLInputElement::TypeAttributeState::ResetButton,
            HTML::HTMLInputElement::TypeAttributeState::ImageButton,
            HTML::HTMLInputElement::TypeAttributeState::Checkbox,
            HTML::HTMLInputElement::TypeAttributeState::RadioButton);
        input_is_single_line = input_allows_adjustment && input.is_single_line();
    }

    bool is_outermost_svg_element = false;
    bool force_position_static = false;
    if (is_svg_element) {
        force_position_static = true;
        if (local_name == "svg"sv) {
            is_outermost_svg_element = true;
            for (auto ancestor = element.parent_element(); ancestor; ancestor = ancestor->parent_element()) {
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "foreignObject"sv)
                    break;
                if (ancestor->namespace_uri() == Namespace::SVG && ancestor->local_name() == "svg"sv) {
                    is_outermost_svg_element = false;
                    break;
                }
            }
            force_position_static = !is_outermost_svg_element;
        }
    }

    bool force_symbol_display_inline = false;
    if (is_svg_element && local_name == "symbol"sv) {
        if (auto* shadow_root = as_if<DOM::ShadowRoot>(element.parent())) {
            if (auto* host = shadow_root->host())
                force_symbol_display_inline = host->namespace_uri() == Namespace::SVG && host->local_name() == "use"sv;
        }
    }

    bool display_contents_computes_to_none = false;

    // https://drafts.csswg.org/css-display-3/#unbox-svg
    // - An svg element that has CSS box layout (this includes all svg whose parent is an HTML element, as well as
    //   document root elements):
    //     display: contents computes to display: none.
    display_contents_computes_to_none |= is_outermost_svg_element;

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
    set(input_allows_adjustment || display_contents_computes_to_none || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::canvas, HTML::TagNames::object, HTML::TagNames::iframe, HTML::TagNames::progress, HTML::TagNames::embed, HTML::TagNames::frame, HTML::TagNames::meter, HTML::TagNames::frameset, HTML::TagNames::img)), ElementStyleAdjustmentFact::DisallowDisplayContents);
    set(input_allows_adjustment || (is_html_element && first_is_one_of(local_name, HTML::TagNames::textarea, HTML::TagNames::audio, HTML::TagNames::video, HTML::TagNames::select)), ElementStyleAdjustmentFact::RewriteInlineFlow);
    set(is_html_element && local_name == HTML::TagNames::button, ElementStyleAdjustmentFact::IsButton);
    set(is_html_element && local_name == HTML::TagNames::select, ElementStyleAdjustmentFact::ForceLineHeightNormal);
    set(input_is_single_line, ElementStyleAdjustmentFact::CheckInputLineHeight);
    set(is_html_element && local_name == HTML::TagNames::audio && !element.has_attribute(HTML::AttributeNames::controls), ElementStyleAdjustmentFact::HideAudioWithoutControls);
    set(is_html_element && local_name == HTML::TagNames::table, ElementStyleAdjustmentFact::IsTable);
    set(force_position_static, ElementStyleAdjustmentFact::ForcePositionStatic);
    set(force_symbol_display_inline, ElementStyleAdjustmentFact::ForceSymbolDisplayInline);
    set(is_mathml_element, ElementStyleAdjustmentFact::IsMathML);
    set(local_name.equals_ignoring_ascii_case("mtable"sv), ElementStyleAdjustmentFact::IsMathMLMtable);
    set(local_name.equals_ignoring_ascii_case("mtr"sv), ElementStyleAdjustmentFact::IsMathMLMtr);
    set(local_name.equals_ignoring_ascii_case("mtd"sv), ElementStyleAdjustmentFact::IsMathMLMtd);
    set(is_html_element && local_name.equals_ignoring_ascii_case(HTML::TagNames::th), ElementStyleAdjustmentFact::IsTh);
    set(element.is_document_element(), ElementStyleAdjustmentFact::IsDocumentElement);
    return facts;
}

u32 element_construction_facts(DOM::Element const& element)
{
    u32 facts = 0;
    auto set = [&](bool condition, ElementConstructionFact fact) {
        if (condition)
            facts |= fact;
    };
    auto shadow_root = element.containing_shadow_root();
    auto const* html_element = as_if<HTML::HTMLElement>(element);
    set(is<HTML::HTMLInputElement>(element), ElementConstructionFact::IsHtmlInputElement);
    set(element.is_html_html_element(), ElementConstructionFact::ConstructedAsHtmlHtmlElement);
    set(shadow_root && shadow_root->is_user_agent_internal(), ElementConstructionFact::IsInUserAgentShadowTree);
    set(html_element && html_element->uses_button_layout(), ElementConstructionFact::UsesButtonLayout);
    set(element.is_editing_host(), ElementConstructionFact::IsEditingHost);
    set(&element == element.document().body(), ElementConstructionFact::IsBody);
    set(element.is_document_element(), ElementConstructionFact::ConstructedAsDocumentElement);
    return facts;
}

u32 element_style_adjustment_facts(DOM::Element const& element)
{
    auto facts = element_box_type_adjustment_facts(element);
    auto set = [&](bool condition, ElementStyleAdjustmentFact fact) {
        if (condition)
            facts |= fact;
    };
    // Admission facts do not participate in box transformations.
    // An animation the element is associated with composes into its style once it is relevant,
    // which its timeline can make it after the element's arrival.
    set(element.has_relevant_animations() || element.has_associated_animations(), ElementStyleAdjustmentFact::HasAnimations);
    set(element.associated_shadow_host_pseudo_element().has_value(), ElementStyleAdjustmentFact::IsShadowHostPseudoElement);
    set(is<SVG::SVGElement>(element), ElementStyleAdjustmentFact::IsSvgElement);
    set(is<SVG::SVGGraphicsElement>(element), ElementStyleAdjustmentFact::IsSvgGraphicsElement);
    set(element.is_html_html_element(), ElementStyleAdjustmentFact::IsHtmlHtmlElement);
    set(is<HTML::HTMLBodyElement>(element), ElementStyleAdjustmentFact::IsHtmlBodyElement);
    set(is<SVG::SVGSwitchElement>(element), ElementStyleAdjustmentFact::IsSvgSwitchElement);
    set(element.is_svg_container(), ElementStyleAdjustmentFact::IsSvgContainer);
    set(element.requires_svg_container(), ElementStyleAdjustmentFact::RequiresSvgContainer);
    set(element.is_svg_foreign_object_element(), ElementStyleAdjustmentFact::IsSvgForeignObjectElement);
    set(is<SVG::SVGMaskElement>(element), ElementStyleAdjustmentFact::IsSvgMaskElement);
    set(is<SVG::SVGClipPathElement>(element), ElementStyleAdjustmentFact::IsSvgClipPathElement);
    set(is<SVG::SVGPatternElement>(element), ElementStyleAdjustmentFact::IsSvgPatternElement);
    set(element.rendered_in_top_layer(), ElementStyleAdjustmentFact::RenderedInTopLayer);
    set(is<HTML::HTMLFrameSetElement>(element), ElementStyleAdjustmentFact::IsHtmlFramesetElement);
    return facts;
}

void record_element_adjustment_facts(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
    StyleEngineFFI::style_engine_set_element_adjustment_facts(style_engine->host(), element.style_node_id(), element_style_adjustment_facts(element));
    StyleEngineFFI::style_engine_set_element_construction_facts(style_engine->host(), element.style_node_id(), element_construction_facts(element));
    StyleEngineFFI::style_engine_set_element_box_kind(style_engine->host(), element.style_node_id(), to_underlying(element.box_kind()));
    StyleEngineFFI::style_engine_set_element_associated_pseudo_kind(style_engine->host(), element.style_node_id(), associated_pseudo_kind_plus_one(element));
}

void record_element_box_kind(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
    StyleEngineFFI::style_engine_set_element_box_kind(style_engine->host(), element.style_node_id(), to_underlying(element.box_kind()));
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
    using Kind = StyleEngineFFI::FfiReplacedContentInputKind;
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
    auto node = element.style_node_id();
    if (auto const* text_area = as_if<HTML::HTMLTextAreaElement>(element)) {
        set_replaced_content_input(*style_engine, node, Kind::TextArea, 0, text_area->cols(), text_area->rows());
        return;
    }
    if (auto const* image = as_if<HTML::HTMLImageElement>(element)) {
        set_image_natural_size_input(*style_engine, node, *image);
        return;
    }
    if (auto const* object = as_if<HTML::HTMLObjectElement>(element)) {
        // An object representing its content navigable is sized from the SVG document it shows.
        if (object->represents_its_content_navigable())
            set_natural_size_input(*style_engine, node, object->natural_size_of_content_document());
        else
            set_image_natural_size_input(*style_engine, node, *object);
        return;
    }
    if (auto const* input = as_if<HTML::HTMLInputElement>(element)) {
        // An image button's box is an image box, which no size attribute sizes.
        if (input->type_state() == HTML::HTMLInputElement::TypeAttributeState::ImageButton) {
            set_image_natural_size_input(*style_engine, node, *input);
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
        set_replaced_content_input(*style_engine, node, kind, 0, input->size());
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
        set_natural_size_input(*style_engine, node, natural_size);
        return;
    }
    if (auto const* image = as_if<SVG::SVGImageElement>(element)) {
        // An SVG <image> takes its image's natural size as the image reports it, and the default object size once
        // something has decoded.
        set_natural_size_input(*style_engine, node, { image->intrinsic_width(), image->intrinsic_height(), image->intrinsic_aspect_ratio() },
            image->decoded_image_data() ? Kind::DecodedSvgImage : Kind::NaturalSize);
        return;
    }
    if (auto const* canvas = as_if<HTML::HTMLCanvasElement>(element))
        set_replaced_content_input(*style_engine, node, Kind::Canvas, 0, canvas->width(), canvas->height());
}

void record_element_construction_facts(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
    StyleEngineFFI::style_engine_set_element_construction_facts(style_engine->host(), element.style_node_id(), element_construction_facts(element));
}

void publish_required_attribute_value_texts(StyleEngine& style_engine, StyleComputer& style_computer)
{
    style_computer.for_each_style_node([&](DOM::Element& element) {
        element.for_each_attribute([&](DOM::QualifiedName const& name, Utf16String const& value) {
            auto name_atom = style_engine.intern_attribute_name(name.local_name(), name.namespace_());
            style_engine.backfill_attribute_value_text_if_required(name_atom, value);
        });
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

// An element arrives with facts already true of it, and the engine has heard none of them. They are
// published as ordinary deltas from absent, so the resident fact store is built by exactly the same
// path later mutations take rather than by a second one.
static Optional<StyleEngineFFI::FfiStateFact> state_fact_for(PseudoClass);

static void record_element_initial_features(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    publish_element_selector_features(*style_engine, element, element.style_node_id(), [&](auto kind, auto name_atom, auto value_kind, auto value_atom) { style_engine->record_local_feature_delta({
                                                                                                                                                              .node = element.style_node_id().value(),
                                                                                                                                                              .feature_kind = kind,
                                                                                                                                                              .name_atom = name_atom.value(),
                                                                                                                                                              .old_kind = StyleEngineFFI::FfiFeatureValueKind::Absent,
                                                                                                                                                              .old_atom = 0,
                                                                                                                                                              .new_kind = value_kind,
                                                                                                                                                              .new_atom = value_atom.value(),
                                                                                                                                                          }); }, [&](bool has_nonempty_text_child) { style_engine->record_local_feature_delta({
                                                                                                                                                                                                                                                                                                                                                                                                                                                                               .node = element.style_node_id().value(),
                                                                                                                                                                                                                                                                                                                                                                                                                                                                               .feature_kind = StyleEngineFFI::FfiFeatureKind::Emptiness,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                               .name_atom = 0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                               .old_kind = has_nonempty_text_child ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                               .old_atom = 0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                               .new_kind = has_nonempty_text_child ? StyleEngineFFI::FfiFeatureValueKind::Absent : StyleEngineFFI::FfiFeatureValueKind::Present,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                               .new_atom = 0,
                                                                                                                                                                                                                                                                                                                                                                                                                                                                           }); });

    if (auto const& id = element.id(); id.has_value())
        StyleEngineFFI::style_engine_set_element_id_name(style_engine->host(), element.style_node_id(), style_engine->intern_atom(*id));

    if (!element.part_names().is_empty())
        record_element_parts_changed(element);
    // NB: Asking the block itself does not build the views of its declarations.
    if (auto const inline_style = element.inline_style(); inline_style && !inline_style->declaration_block().is_empty())
        record_element_inline_style_properties(element);
    if (element_has_presentational_hints_to_publish(element))
        StyleComputer::collect_presentational_hint_properties({ element });
    record_element_replaced_content_input(element);
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

void record_element_moved(DOM::Element& element, DOM::Node* old_parent, DOM::Element* old_previous_sibling, DOM::Element* old_next_sibling)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
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
    if (previous.parent == relations.parent
        && previous.previous_element_sibling == relations.previous_element_sibling
        && previous.next_element_sibling == relations.next_element_sibling) {
        return;
    }
    // A style transaction that flew styled the element where it was, and what inherits from it under where it was.
    style_engine->note_style_node_arrived_or_retired(element.style_node_id());

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
    }

    // The relinking path rather than the neighbour one. A move rewrites the DOM's own links without
    // going through insertion or removal, so nothing has told the engine's relation columns that
    // anything happened: they are the engine's copy of the child sequence, and only a delta splices
    // them. Publishing this as a neighbour change leaves the columns holding the order the element
    // left, which is the order every positional question is then answered in.
    style_engine->record_tree_delta({
        .node = element.style_node_id().value(),
        .old_connected = true,
        .new_connected = true,
        .old_relations = previous,
        .new_relations = relations,
    });

    // A move across parents is a departure from one child list and an arrival in another, so both
    // parents' emptiness can have moved even though no node was created or destroyed.
    if (old_parent != element.parent()) {
        if (auto* old_parent_element = as_if<DOM::Element>(old_parent))
            record_element_emptiness_changed(*old_parent_element, element, true, false);
        if (auto* new_parent = as_if<DOM::Element>(element.parent()))
            record_element_emptiness_changed(*new_parent, element, false, true);
    }
}

void record_element_assigned_slot_changed(DOM::Element& element, DOM::Element* old_slot)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    auto relations = relations_of(element, *style_engine);
    auto previous = relations;
    previous.assigned_slot = identity_of(old_slot).value();
    if (previous.assigned_slot == relations.assigned_slot)
        return;

    // The relinking path rather than the neighbour one: the engine holds the slot a slottable is
    // assigned to, and only relinking updates it. Every other relation is identical on both sides,
    // so unlinking and linking again leaves them where they were.
    style_engine->record_tree_delta({
        .node = element.style_node_id().value(),
        .old_connected = true,
        .new_connected = true,
        .old_relations = previous,
        .new_relations = relations,
    });
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
    for (auto const& member : members) {
        if (member->style_node_id() != no_style_node)
            identities.unchecked_append(member->style_node_id());
    }
    StyleEngineFFI::style_engine_set_top_layer_elements(document.style_computer().style_engine().host(), identities.span());
}

// Assignment runs inside the insertion that connects a node, which happens before the subtree it arrived in is named,
// and the list published then names only the members that already had an identity. Both ends of the relation therefore
// republish on arrival: a slottable the list it has just become a member of, and a slot the list it arrived owning.
static void republish_assigned_slot_of(DOM::Node& node)
{
    if (auto slot = DOM::assigned_slot_for_node(node))
        record_slot_assignment_changed(*slot);
    if (auto* slot = as_if<HTML::HTMLSlotElement>(node))
        record_slot_assignment_changed(*slot);
}

static void record_element_disconnecting(DOM::Element& element, TreeScopeID tree_scope)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    auto node = element.style_node_id();
    if (node == no_style_node)
        return;

    // Removing a slot can leave its slottables with no replacement. Publish their departure while
    // the slot still has an identity; assignment runs after the disconnect walk and cannot name the
    // old slot by then. If another slot takes over, the ordinary assignment path publishes the
    // subsequent arrival.
    if (auto* slot = as_if<HTML::HTMLSlotElement>(element)) {
        for (auto const& slottable : slot->assigned_nodes_internal()) {
            auto const* assigned_element = slottable.get_pointer<GC::Ref<DOM::Element>>();
            if (!assigned_element || (*assigned_element)->style_node_id() == no_style_node)
                continue;
            auto old_relations = relations_of(**assigned_element, *style_engine);
            auto new_relations = old_relations;
            new_relations.assigned_slot = no_style_node.value();
            style_engine->record_tree_delta({
                .node = (*assigned_element)->style_node_id().value(),
                .old_connected = true,
                .new_connected = true,
                .old_relations = old_relations,
                .new_relations = new_relations,
            });
        }
    }

    style_engine->cancel_deferred_element_initial_features(node);

    style_engine->record_tree_delta({
        .node = node.value(),
        .old_connected = true,
        .new_connected = false,
        .old_relations = relations_of(element, *style_engine, tree_scope),
        .new_relations = detached_relations(),
    });

    // Dropping the identity is what makes a second removal a no-op rather than a double retirement.
    element.document().style_computer().unregister_style_node(node);
    element.set_style_node_id(no_style_node);
}

// The animation names an element's computed style references.
//
// Not an input: the element was recomputed by whatever moved its `animation-name`. This is the index
// that lets a `@keyframes` rule find the elements running the animation it describes, which nothing
// about selector matching can say.
void record_element_animation_names(DOM::Element& element, ReadonlySpan<Utf16FlyString> names)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    Vector<StyleAtomID> atoms;
    atoms.ensure_capacity(names.size());
    for (auto const& name : names)
        atoms.unchecked_append(style_engine->intern_atom(name));
    StyleEngineFFI::style_engine_set_element_animation_names(style_engine->host(), element.style_node_id(), atoms);
}

// The names of the CSS animations the element owns, in one of its per-pseudo-element lists, and the
// definition each one last had applied.
//
// This one is an input: the computation of an element's animation definitions matches them
// against the animations the element already has, and this is what it matches them against.
void record_element_css_defined_animations(DOM::Element& element, u8 slot, ReadonlySpan<Utf16FlyString> names, ReadonlySpan<StyleEngineFFI::FfiAppliedAnimationDefinition> definitions)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    // The names travel as one buffer of code units with a length each, since a list is almost
    // always a single name and a handle per name would cost more than the names do.
    Vector<u32> lengths;
    Vector<u16> units;
    lengths.ensure_capacity(names.size());
    for (auto const& name : names) {
        auto view = name.view();
        lengths.unchecked_append(static_cast<u32>(view.length_in_code_units()));
        units.ensure_capacity(units.size() + view.length_in_code_units());
        for (size_t index = 0; index < view.length_in_code_units(); ++index)
            units.unchecked_append(view.code_unit_at(index));
    }
    StyleEngineFFI::style_engine_set_element_css_defined_animations(style_engine->host(), element.style_node_id(), slot, lengths, units, definitions);
}

// A keyframe's composite operation as a published keyframe spells it.
static StyleValueFFI::FfiCompositeOperation published_composite_operation(Bindings::CompositeOperation operation)
{
    switch (operation) {
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
    keyframe.first_linear_point = static_cast<u32>(points.size());
    easing.visit(
        [&](LinearEasingFunction const& linear) {
            keyframe.easing_kind = StyleEngineFFI::FfiPublishedEasingKind::Linear;
            for (auto const& point : linear.control_points)
                points.append({ .input = point.input, .output = point.output });
        },
        [&](CubicBezierEasingFunction const& cubic_bezier) {
            keyframe.easing_kind = StyleEngineFFI::FfiPublishedEasingKind::CubicBezier;
            keyframe.x1 = cubic_bezier.x1;
            keyframe.y1 = cubic_bezier.y1;
            keyframe.x2 = cubic_bezier.x2;
            keyframe.y2 = cubic_bezier.y2;
        },
        [&](StepsEasingFunction const& steps) {
            keyframe.easing_kind = StyleEngineFFI::FfiPublishedEasingKind::Steps;
            keyframe.interval_count = steps.interval_count;
            keyframe.step_position = static_cast<u8>(to_underlying(steps.position));
        });
    keyframe.linear_point_count = static_cast<u32>(points.size()) - keyframe.first_linear_point;
}

// The effects one of an element's animation lists holds, in composite order, described for the style
// engine to sample them from, unless it already describes exactly these versions of them: every change
// that moves what a description says moves its effect's generation.
//
// Everything a keyframe declares that does not depend on the element being sampled is settled here. What
// does, a value or an easing still to be substituted against the element, travels as written.
void record_element_animation_effect_descriptions(DOM::Element& element, u8 slot, ReadonlySpan<GC::Ref<Animations::KeyframeEffect>> effects)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    Vector<StyleEngineFFI::FfiAnimationEffectVersion, 4> versions;
    versions.ensure_capacity(effects.size());
    for (auto const& effect : effects)
        versions.unchecked_append({ .identity = effect->animation_preparation_identity(), .generation = effect->animation_preparation_generation() });
    if (StyleEngineFFI::style_engine_describes_animation_effects(style_engine->host(), element.style_node_id().value(), slot, versions.data(), versions.size()))
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
            keyframe.composite = published_composite_operation([&] {
                switch (it->composite) {
                case Bindings::CompositeOperationOrAuto::Accumulate:
                    return Bindings::CompositeOperation::Accumulate;
                case Bindings::CompositeOperationOrAuto::Add:
                    return Bindings::CompositeOperation::Add;
                case Bindings::CompositeOperationOrAuto::Replace:
                    return Bindings::CompositeOperation::Replace;
                case Bindings::CompositeOperationOrAuto::Auto:
                    return effect->composite();
                }
                VERIFY_NOT_REACHED();
            }());
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
                // A shorthand's pending substitution animates nothing, and neither does a value that
                // is invalid at computed-value time.
                // https://drafts.csswg.org/css-values-5/#invalid-at-computed-value-time
                if (!use_initial && (data->tag == StyleValueFFI::StyleValueData::Tag::PendingSubstitution || (data->tag == StyleValueFFI::StyleValueData::Tag::GuaranteedInvalid && !property.is_custom_property())))
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

// The custom properties an element declares or references.
//
// Also an index rather than an input, and for the same reason as the animation names: what it answers
// is which elements an `@property` registration reaches, and nothing about selector matching or the
// element's own recomputation can say that.
void record_element_custom_property_names(DOM::Element& element, CustomPropertyData const* data, ReadonlySpan<RefPtr<CustomPropertyData const>> pseudo_element_data, ReadonlySpan<Utf16FlyString> references, bool uses_unnamed, bool uses_custom_functions)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    // OPTIMIZATION: Each environment hands out its declared names sorted and deduplicated, so they are merged
    //               rather than sorted once more for every element that holds that environment.
    ReadonlySpan<StyleAtomID> published;
    Vector<StyleAtomID> atoms;
    Vector<StyleAtomID> merged;
    auto merge_names = [&](ReadonlySpan<StyleAtomID> names) {
        if (names.is_empty())
            return;
        if (published.is_empty()) {
            published = names;
            return;
        }
        merged.clear_with_capacity();
        merged.ensure_capacity(published.size() + names.size());
        set_union(published, names, merged);
        swap(atoms, merged);
        published = atoms;
    };
    Vector<CustomPropertyData const*, 4> merged_environments;
    auto merge_declared_names = [&](CustomPropertyData const* custom_property_data) {
        if (!custom_property_data || merged_environments.contains_slow(custom_property_data))
            return;
        merged_environments.append(custom_property_data);
        merge_names(custom_property_data->declared_name_atoms(bit_cast<FlatPtr>(&element.document()), style_engine->atom_generation(), [&](Utf16FlyString const& name) { return style_engine->intern_atom(name); }));
    };
    merge_declared_names(data);
    for (auto const& pseudo_data : pseudo_element_data)
        merge_declared_names(pseudo_data.ptr());
    // The references arrive deduplicated, and interning is injective, so their atoms only need sorting.
    Vector<StyleAtomID> reference_atoms;
    if (!references.is_empty()) {
        reference_atoms.ensure_capacity(references.size());
        for (auto const& name : references)
            reference_atoms.unchecked_append(style_engine->intern_atom(name));
        quick_sort(reference_atoms);
        merge_names(reference_atoms);
    }
    StyleEngineFFI::style_engine_set_element_custom_property_names(style_engine->host(), element.style_node_id(), published, uses_unnamed, uses_custom_functions);
}

void record_element_custom_property_names(DOM::Element& element, ReadonlySpan<Utf16FlyString> names, bool uses_unnamed, bool uses_custom_functions)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    Vector<StyleAtomID> atoms;
    atoms.ensure_capacity(names.size());
    for (auto const& name : names)
        atoms.unchecked_append(style_engine->intern_atom(name));
    StyleEngineFFI::style_engine_set_element_custom_property_names(style_engine->host(), element.style_node_id(), atoms, uses_unnamed, uses_custom_functions);
}

// An element's heading level, which `:heading()` tests. It follows from what the element is plus
// the heading offset its ancestors declare, so it moves when the element does and when one of those
// attributes changes - not with anything the element itself publishes.
static void record_element_heading_level(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
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
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;

    auto const language = element.lang_view();
    style_engine->set_element_language(
        element.style_node_id(),
        language.has_value() ? style_engine->intern_text_atom(*language) : 0,
        language.value_or({}));

    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? "rtl"sv : "ltr"sv;
    StyleEngineFFI::style_engine_set_element_directionality(style_engine->host(), element.style_node_id(), style_engine->intern_text_atom(Utf16View { directionality }));

    // Reading the tag caches it, and this can run while the element is still being built - an XML
    // parser sets attributes after insertion, so the tag read here may not be the one it ends up
    // with. Drop the cache so the next read computes it from the finished element. The published
    // fact is not wrong for having been early: what a `:lang()` rule is routed by is that the tag
    // moved, and the move to its real value is published like any other.
    element.invalidate_lang_value();
}

void record_element_directionality(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;

    auto const directionality = element.directionality() == DOM::Element::Directionality::Rtl ? "rtl"sv : "ltr"sv;
    StyleEngineFFI::style_engine_set_element_directionality(style_engine->host(), element.style_node_id(), style_engine->intern_text_atom(Utf16View { directionality }));
}

void record_element_custom_states_changed(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
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
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
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
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    // Whether the element is empty either side of the change is decided by the child that moved
    // together with the ones that did not, and the ones that did not are the same both times.
    auto empty_but_for_the_changing_child = SelectorMatching::element_is_empty_ignoring_child(element, changing_child);
    auto was_empty = empty_but_for_the_changing_child && !counted_before;
    auto is_empty = empty_but_for_the_changing_child && !counts_after;
    if (was_empty == is_empty)
        return;

    auto kind_of = [](bool empty) {
        return empty ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };
    record_feature(element, StyleEngineFFI::FfiFeatureKind::Emptiness, 0, kind_of(was_empty), 0, kind_of(is_empty), 0);
}

// An element can arrive with a style attribute already written, so this is published on arrival as
// well as when the block is edited.
static void record_element_inline_style_properties(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;
    auto const inline_style = element.inline_style();
    style_engine->set_element_inline_style_properties(element.style_node_id(), inline_style ? &inline_style->declaration_block() : nullptr);
}

// The hints an element's attributes map to, published at its arrival, wherever what they are mapped
// from moves, and from where the cascade collects them.
bool record_element_presentational_hint_properties(DOM::Element& element, ReadonlySpan<StyleProperty> hints)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return false;
    // NB: The SvgPresentationAttribute kind is the one whose declarations the engine takes as
    //     current: every element's hints are published where they move.
    style_engine->set_element_presentational_hint_properties(element.style_node_id(), StyleEngineFFI::FfiElementDeclarationKind::SvgPresentationAttribute, hints);
    return true;
}

void record_element_declarations_changed(DOM::Element& element, ElementDeclarationKind kind, bool had_declarations, bool has_declarations)
{
    if (element.style_node_id() != no_style_node)
        element.document().style_computer().style_engine().note_element_declarations_changed(element.style_node_id());
    element.document().flush_deferred_style_change_event();
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node || has_pending_initial_features(element))
        return;
    if (!had_declarations && !has_declarations)
        return;

    auto ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::InlineStyle;
    switch (kind) {
    case ElementDeclarationKind::InlineStyle:
        ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::InlineStyle;
        break;
    case ElementDeclarationKind::PresentationalHint:
        ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::PresentationalHint;
        break;
    case ElementDeclarationKind::SvgPresentationAttribute:
        ffi_kind = StyleEngineFFI::FfiElementDeclarationKind::SvgPresentationAttribute;
        break;
    }

    // The block's contents moved even where the CSSOM object did not, so the identity that makes
    // this a change is a version rather than the object's address. The engine mints it as it
    // applies the delta.
    style_engine->record_element_declaration_delta({
        .node = element.style_node_id().value(),
        .kind = ffi_kind,
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
    // Only the root leaves a child sequence that stays in the tree. Every node below it leaves with the sequence it
    // belongs to.
    if (auto identity = dom_order_identity_of(root); style_engine && identity != no_style_node)
        StyleEngineFFI::style_engine_unlink_style_node_from_dom_order(style_engine->host(), identity, dom_order_parent_of(root.parent()));
    auto root_tree_scope = tree_scope_of(root.root());
    Vector<GC::Ref<DOM::ShadowRoot>> shadow_roots;
    Vector<StyleNodeID, 64> departing_texts;
    auto disconnect_element = [&](DOM::Node& node, TreeScopeID tree_scope) {
        if (auto* element = as_if<DOM::Element>(node)) {
            record_element_disconnecting(*element, tree_scope);
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
            ? style_engine.intern_case_sensitive_text_atom(Namespace::HTML.view())
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
        HashTable<DOM::Document*> documents;
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
    auto const* owner_node = sheet.owner_node();
    if (!owner_node || !(owner_node->is_html_style_element() || owner_node->is_svg_style_element()))
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
        return *existing;
    }
    auto contents = StyleSheetState::create(sheet.native_rules().clone_shared_contents(), &document, RustMediaList {}, {});
    contents->set_base_url(sheet.style_resource_base_url().value_or(document.base_url()));
    auto sheet_id = style_engine.add_sheet(
        static_cast<u32>(reinterpret_cast<FlatPtr>(contents.ptr()) >> 3),
        StyleEngineFFI::FfiCascadeOrigin::Author);
    contents->set_style_engine_sheet_id(sheet_id);
    RuleCompilationContext context { style_engine, sheet_id, 0, document, style_computer };
    compile_rules_into(context, *contents);
    contents->evaluate_media_queries(document);
    contents->load_pending_image_resources(document);
    auto shared_compiled_style_sheet = make_ref_counted<SharedCompiledStyleSheet>(move(key), move(contents), sheet_id);
    shared_compiled_style_sheets.set(shared_compiled_style_sheet->key(), shared_compiled_style_sheet);
    return shared_compiled_style_sheet;
}

static void detach_shared_compiled_style_sheet(SharedCompiledStyleSheet& sheet, u64 occurrence, TreeScopeID tree_scope, StyleComputer& style_computer)
{
    auto& style_engine = style_computer.style_engine();
    StyleEngineFFI::style_engine_detach_sheet_occurrence(style_engine.host(), tree_scope, occurrence);
    sheet.remove_attachment(tree_scope);
    if (sheet.has_attachments())
        return;

    style_engine.begin_sheet_rules_replacement(sheet.sheet_id());
    style_engine.finish_sheet_rules_replacement(sheet.sheet_id());
    auto& shared_compiled_style_sheets = style_computer.shared_compiled_style_sheets();
    shared_compiled_style_sheets.remove(sheet.key());
    if (shared_compiled_style_sheets.is_empty())
        shared_compiled_style_sheets.clear();
}

bool stop_sharing_compiled_style_sheet(StyleSheetState& sheet)
{
    auto* shared_compiled_style_sheet = sheet.shared_compiled_style_sheet();
    if (!shared_compiled_style_sheet)
        return false;
    flush_deferred_style_change_events_for_sheet(sheet);
    sheet.mark_compiled_style_sheet_unshareable();
    Vector<GC::Ref<DOM::Node>> owners;
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
        style_engine.begin_sheet_rules_replacement(sheet_id);
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
            // The CSSOM object's identity is what the program keys its wrapper by; the semantic sheet
            // is a separate identity that survives edits to its contents.
            sheet_id = style_engine.add_sheet(
                static_cast<u32>(reinterpret_cast<FlatPtr>(&sheet) >> 3),
                StyleEngineFFI::FfiCascadeOrigin::Author);
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
// the set is compared by identity and re-attached whole when it differs.
void record_non_author_stylesheets(DOM::Document& document)
{
    auto& style_computer = document.style_computer();
    auto& style_scope = document.style_scope();

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
    for (auto const& entry : recorded)
        StyleEngineFFI::style_engine_detach_sheet(style_engine.host(), entry.sheet_id, document_tree_scope);
    recorded.clear();

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

    for (size_t index = 0; index < sheets.size(); ++index) {
        auto sheet_id = style_engine.add_sheet(
            static_cast<u32>(reinterpret_cast<FlatPtr>(sheets[index].ptr()) >> 3),
            origins[index]);
        StyleEngineFFI::style_engine_attach_sheet(style_engine.host(), sheet_id, document_tree_scope, first_author_sheet);
        RuleCompilationContext context { style_engine, sheet_id, 0, document, style_computer };
        compile_rules_into(context, *sheets[index]);
        recorded.append({ sheets[index], sheet_id });
    }
}

static StyleSheetState* owning_engine_sheet(StyleSheetState& sheet)
{
    // A constructed sheet is always its own engine sheet; its per-document ids make the raw member 0
    // without its rules living in any other sheet's program.
    if (sheet.constructed())
        return &sheet;
    auto* engine_sheet = &sheet;
    while (engine_sheet->style_engine_sheet_id() == 0) {
        auto* owner = engine_sheet->owner_import();
        if (!owner)
            return nullptr;
        engine_sheet = owner->parent_style_sheet();
        if (!engine_sheet)
            return nullptr;
    }
    return engine_sheet;
}

void record_stylesheet_rule_conditions(StyleSheetState& sheet)
{
    auto* engine_sheet = owning_engine_sheet(sheet);
    if (!engine_sheet)
        return;
    for_each_document_with_engine_copy(*engine_sheet, [&](DOM::Document& document) {
        record_stylesheet_rule_conditions(sheet, document);
    });
}

void record_stylesheet_rule_conditions(StyleSheetState& sheet, DOM::Document& document)
{
    auto* engine_sheet = sheet.owner_import() ? owning_engine_sheet(sheet) : &sheet;
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
    auto* engine_sheet = owning_engine_sheet(sheet);
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

bool can_record_element_state_change(DOM::Element& element)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return false;
    auto node = element.style_node_id();
    if (node == no_style_node || has_pending_initial_features(element))
        return false;
    return true;
}

void record_element_state_changed(DOM::Element& element, PseudoClass pseudo_class, bool new_value)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    auto node = element.style_node_id();
    if (node == no_style_node || has_pending_initial_features(element))
        return;
    auto fact = state_fact_for(pseudo_class);
    if (!fact.has_value())
        return;

    style_engine->record_state_delta({
        .node = node.value(),
        .fact = *fact,
        .new_value = new_value,
    });
}

static void record_feature(
    DOM::Element& element,
    StyleEngineFFI::FfiFeatureKind kind,
    StyleAtomID name_atom,
    StyleEngineFFI::FfiFeatureValueKind old_kind,
    StyleAtomID old_atom,
    StyleEngineFFI::FfiFeatureValueKind new_kind,
    StyleAtomID new_atom)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine)
        return;
    auto node = element.style_node_id();
    if (node == no_style_node || has_pending_initial_features(element))
        return;

    style_engine->record_local_feature_delta(
        {
            .node = node.value(),
            .feature_kind = kind,
            .name_atom = name_atom.value(),
            .old_kind = old_kind,
            .old_atom = old_atom.value(),
            .new_kind = new_kind,
            .new_atom = new_atom.value(),
        });
}

void record_element_id_changed(DOM::Element& element, Optional<Utf16FlyString> const& old_value, Optional<Utf16FlyString> const& new_value)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    auto atom_of = [&](Optional<Utf16FlyString> const& value) -> StyleAtomID {
        return value.has_value() ? intern_id_or_class_atom(*style_engine, element, *value) : 0;
    };
    auto kind_of = [](Optional<Utf16FlyString> const& value) {
        return value.has_value() ? StyleEngineFFI::FfiFeatureValueKind::Atom : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };

    record_feature(element, StyleEngineFFI::FfiFeatureKind::Id, 0, kind_of(old_value), atom_of(old_value), kind_of(new_value), atom_of(new_value));

    // `getElementById` is case-sensitive in every mode, so the name the inverse index is keyed by
    // is the one written rather than the one a quirks-mode selector folds it to.
    StyleEngineFFI::style_engine_set_element_id_name(style_engine->host(), element.style_node_id(), new_value.has_value() ? style_engine->intern_atom(*new_value) : StyleAtomID {});
}

void record_element_class_list_changed(DOM::Element& element, ReadonlySpan<Utf16FlyString> old_classes, ReadonlySpan<Utf16FlyString> new_classes)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;

    // One class delta per class that actually gained or lost membership. A class present on both
    // sides is not a change, and journalling it would be exactly the amplification the engine
    // exists to avoid.
    // Two class names that differ only in case are one class where the document folds them, so
    // membership is decided between the folded names rather than the written ones.
    auto folded = [&](Utf16FlyString const& name) {
        return element.document().in_quirks_mode() ? name.to_ascii_lowercase() : name;
    };
    auto contains_folded = [&](ReadonlySpan<Utf16FlyString> names, Utf16FlyString const& folded_name) {
        return any_of(names, [&](auto const& name) { return folded(name) == folded_name; });
    };

    auto record_membership = [&](Utf16FlyString const& folded_name, bool was_present, bool is_present) {
        if (was_present == is_present)
            return;
        auto atom = style_engine->intern_atom(folded_name);
        record_feature(
            element,
            StyleEngineFFI::FfiFeatureKind::Class,
            atom,
            was_present ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent,
            0,
            is_present ? StyleEngineFFI::FfiFeatureValueKind::Present : StyleEngineFFI::FfiFeatureValueKind::Absent,
            0);
    };

    for (auto const& name : old_classes)
        record_membership(folded(name), true, contains_folded(new_classes, folded(name)));
    for (auto const& name : new_classes) {
        if (!contains_folded(old_classes, folded(name)))
            record_membership(folded(name), false, true);
    }
}

void record_element_attribute_changed(DOM::Element& element, Utf16FlyString const& name, Optional<Utf16FlyString> const& namespace_uri, Optional<Utf16String> const& old_value, Optional<Utf16String> const& new_value)
{
    auto* style_engine = style_engine_for(element);
    if (!style_engine || element.style_node_id() == no_style_node)
        return;
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

    // Both values cross as atoms. Their text is recorded once per distinct value only when a
    // compiled selector for this attribute uses an operator that cannot compare atom identities,
    // or when an attr() can read the name. This lets the match evaluator reconstruct either side
    // of such a transaction without asking the DOM, and two different values cannot cancel in the
    // journal merely because both are present.
    // The same name an arriving attribute publishes, with the same other forms noted alongside it.
    // See `StyleEngine::intern_attribute_name`.
    auto atom = style_engine->intern_attribute_name(name, namespace_uri);
    auto kind_of = [](Optional<Utf16String> const& value) {
        return value.has_value() ? StyleEngineFFI::FfiFeatureValueKind::Atom : StyleEngineFFI::FfiFeatureValueKind::Absent;
    };
    auto atom_of = [&](Optional<Utf16String> const& value) {
        return value.has_value() ? style_engine->intern_attribute_value(atom, *value) : 0;
    };
    auto old_kind = kind_of(old_value);
    auto old_atom = atom_of(old_value);
    auto new_kind = kind_of(new_value);
    auto new_atom = atom_of(new_value);
    record_feature(
        element,
        StyleEngineFFI::FfiFeatureKind::Attribute,
        atom,
        old_kind,
        old_atom,
        new_kind,
        new_atom);
}

}
