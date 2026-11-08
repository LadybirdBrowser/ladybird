/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/AnyOf.h>
#include <AK/Format.h>
#include <AK/HashTable.h>
#include <AK/ScopeGuard.h>
#include <LibWeb/Animations/AnimationEffect.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyData.h>
#include <LibWeb/CSS/HighlightRegistry.h>
#include <LibWeb/CSS/MainEffects.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/CSS/StyleInvalidation.h>
#include <LibWeb/CSS/StyleUpdateInputs.h>
#include <LibWeb/DOM/AbstractElement.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/PseudoElement.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StaticRange.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/SpellingMarkers.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/HTML/HTMLSlotElement.h>
#include <LibWeb/HTML/LocalNavigable.h>
#include <LibWeb/HTML/NavigableContainer.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Selection/Selection.h>

namespace Web::DOM {

Document::HighlightStyleObservability& Document::highlight_style_observability(CSS::PseudoElement pseudo_element)
{
    VERIFY(CSS::is_highlight_pseudo_element(pseudo_element));
    switch (pseudo_element) {
    case CSS::PseudoElement::Selection:
        return m_highlight_style_observability[0];
    case CSS::PseudoElement::SpellingError:
        return m_highlight_style_observability[2];
    case CSS::PseudoElement::Highlight:
        return m_highlight_style_observability[3];
    default:
        return m_highlight_style_observability[1];
    }
}

void Document::set_needs_highlight_style_update(CSS::PseudoElement pseudo_element)
{
    highlight_style_observability(pseudo_element).needs_update = true;
}

bool Document::needs_highlight_style_update() const
{
    return any_of(m_highlight_style_observability, [](auto const& state) { return state.needs_update; })
        || custom_highlight_static_ranges_may_have_changed();
}

bool Document::custom_highlight_static_ranges_may_have_changed() const
{
    return m_custom_highlight_observers_read_static_ranges && character_data_version() != m_custom_highlight_observers_character_data_version;
}

static bool range_intersects_node(AbstractRange const& range, Node& node)
{
    if (auto const* live_range = as_if<Range>(range))
        return live_range->intersects_node(node);
    auto* parent = node.parent();
    if (!parent)
        return true;
    auto offset = static_cast<WebIDL::UnsignedLong>(node.index());
    return position_of_boundary_point_relative_to_other_boundary_point({ *parent, offset }, range.end()) == RelativeBoundaryPointPosition::Before
        && position_of_boundary_point_relative_to_other_boundary_point({ *parent, offset + 1 }, range.start()) == RelativeBoundaryPointPosition::After;
}

static void collect_highlight_observing_elements(Vector<CSS::StyleNodeID>& style_nodes, Node& root, AbstractRange const* range)
{
    auto record_element = [&](Node& node) {
        if (auto* element = as_if<Element>(node); element && element->style_node_id() != CSS::StyleNodeID {})
            style_nodes.append(element->style_node_id());
    };

    // NB: Ancestors supply inherited highlight styles, and text controls paint through
    //     their internal shadow trees. Neither requires visiting unrelated subtrees.
    for (auto* ancestor = root.flat_tree_parent(); ancestor; ancestor = ancestor->flat_tree_parent())
        record_element(*ancestor);
    root.for_each_shadow_including_inclusive_descendant([&](Node& node) {
        if (range && &node.root() == &range->start_container()->root() && !range_intersects_node(*range, node))
            return TraversalDecision::SkipChildrenAndContinue;
        record_element(node);
        return TraversalDecision::Continue;
    });
}

struct HighlightStyleInput {
    GC::Ref<Node> root;
    GC::Ptr<AbstractRange> range;

    void visit_edges(GC::Cell::Visitor& visitor)
    {
        visitor.visit(root);
        visitor.visit(range);
    }
};

static GC::RootVector<HighlightStyleInput, 2> selection_style_inputs(Document& document)
{
    GC::RootVector<HighlightStyleInput, 2> inputs;
    if (auto selection = document.get_selection(); selection && !selection->is_collapsed()) {
        auto range = selection->range();
        inputs.append({ range->common_ancestor_container(), range });
    }
    if (auto* text_control = as_if<HTML::FormAssociatedTextControlElement>(document.focused_area().ptr()); text_control && text_control->selection_start() != text_control->selection_end())
        inputs.append({ *document.focused_area(), nullptr });
    return inputs;
}

static GC::RootVector<HighlightStyleInput, 2> search_text_style_inputs(Document& document)
{
    GC::RootVector<HighlightStyleInput, 2> inputs;
    if (auto active_match = document.find_in_page_active_match(); active_match && !active_match->collapsed())
        inputs.append({ active_match->common_ancestor_container(), active_match });
    GC::RootHashTable<Node*> highlighted_roots;
    for (auto const& match : document.find_in_page_highlighted_matches()) {
        if (match->collapsed())
            continue;
        auto root = match->common_ancestor_container();
        if (highlighted_roots.set(root.ptr()) == HashSetResult::InsertedNewEntry)
            inputs.append({ root, nullptr });
    }
    return inputs;
}

static GC::RootVector<HighlightStyleInput, 2> spelling_error_style_inputs(Document& document)
{
    GC::RootVector<HighlightStyleInput, 2> inputs;
    if (!document.has_spelling_markers())
        return inputs;
    GC::RootHashTable<Node*> roots;
    document.spelling_markers().for_each_marked_text([&](Text& text, auto) {
        auto* parent = text.parent();
        if (parent && roots.set(parent) == HashSetResult::InsertedNewEntry)
            inputs.append({ *parent, nullptr });
    });
    // A root inside another root's subtree adds nothing, since the outer root's walk visits all of it.
    inputs.remove_all_matching([&](auto const& input) {
        for (auto* ancestor = input.root->parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
            if (roots.contains(ancestor))
                return true;
        }
        return false;
    });
    return inputs;
}

static Node& common_ancestor_container(AbstractRange const& range)
{
    if (auto const* live_range = as_if<Range>(range))
        return *live_range->common_ancestor_container();
    Node* container = range.start_container().ptr();
    while (!container->is_inclusive_ancestor_of(*range.end_container()))
        container = container->parent();
    return *container;
}

static bool collect_custom_highlight_observing_elements(Document& document, Vector<CSS::StyleNodeID>& style_nodes, Vector<CSS::StyleAtomID>& names)
{
    auto* registry = document.highlight_registry_if_exists();
    if (!registry)
        return false;
    bool read_static_ranges = false;
    auto& style_engine = document.style_computer().style_engine();
    registry->for_each_highlight_in_stacking_order([&](Utf16String const& name, ReadonlySpan<GC::Ref<AbstractRange>> ranges) {
        GC::RootVector<HighlightStyleInput, 2> inputs;
        GC::RootHashMap<Node const*, size_t> input_index_by_root;
        for (auto const& range : ranges) {
            read_static_ranges |= is<StaticRange>(*range);
            if (!CSS::HighlightRegistry::range_is_paintable(document, *range))
                continue;
            auto& root = common_ancestor_container(*range);
            if (auto index = input_index_by_root.get(&root); index.has_value()) {
                inputs[*index].range = nullptr;
                continue;
            }
            input_index_by_root.set(&root, inputs.size());
            inputs.append({ root, range });
        }
        if (inputs.is_empty())
            return;
        GC::RootHashTable<Node const*> roots_walked_in_full;
        for (auto const& input : inputs) {
            if (!input.range)
                roots_walked_in_full.set(input.root.ptr());
        }
        inputs.remove_all_matching([&](auto const& input) {
            for (auto* ancestor = input.root->parent_or_shadow_host(); ancestor; ancestor = ancestor->parent_or_shadow_host()) {
                if (roots_walked_in_full.contains(ancestor))
                    return true;
            }
            return false;
        });
        auto first = style_nodes.size();
        for (auto const& input : inputs)
            collect_highlight_observing_elements(style_nodes, input.root, input.range.ptr());
        auto atom = style_engine.intern_atom(Utf16FlyString { name });
        for (auto i = first; i < style_nodes.size(); ++i)
            names.append(atom);
    });
    return read_static_ranges;
}

void Document::update_highlight_style_observability(CSS::PseudoElement pseudo_element)
{
    // NB: Editing commands temporarily select content to restore its formatting. Style reads
    //     during the action must not activate selection styles throughout the document for these
    //     intermediate ranges. Observe the final selection after the action, including in input
    //     event handlers. Explicit ::selection queries can still compute their style on demand.
    if (pseudo_element == CSS::PseudoElement::Selection && m_running_editing_command_action)
        return;

    // OPTIMIZATION: Whatever moves what shows a highlight notes that it needs an update, so one that needs none has
    //               nothing to collect.
    auto& state = highlight_style_observability(pseudo_element);
    if (pseudo_element == CSS::PseudoElement::Highlight && custom_highlight_static_ranges_may_have_changed())
        state.needs_update = true;
    if (!state.needs_update)
        return;

    if (pseudo_element == CSS::PseudoElement::Highlight) {
        Vector<CSS::StyleNodeID> style_nodes;
        Vector<CSS::StyleAtomID> names;
        m_custom_highlight_observers_read_static_ranges = collect_custom_highlight_observing_elements(*this, style_nodes, names);
        m_custom_highlight_observers_character_data_version = character_data_version();
        state = { .observable = !style_nodes.is_empty(), .needs_update = false };
        style_computer().style_engine().set_custom_highlight_observing_elements(style_nodes, names);
        return;
    }

    auto inputs = [&] {
        switch (pseudo_element) {
        case CSS::PseudoElement::Selection:
            return selection_style_inputs(*this);
        case CSS::PseudoElement::SpellingError:
            return spelling_error_style_inputs(*this);
        default:
            return search_text_style_inputs(*this);
        }
    }();
    bool observable = !inputs.is_empty();
    state = { .observable = observable, .needs_update = false };
    // NB: The style engine restyles the elements that start or stop showing the highlight, which
    //     are the only ones whose highlight styles a style update keeps.
    Vector<CSS::StyleNodeID> style_nodes;
    for (auto const& input : inputs)
        collect_highlight_observing_elements(style_nodes, input.root, input.range.ptr());
    auto& style_engine = style_computer().style_engine();
    style_engine.set_highlight_observing_elements(pseudo_element, style_nodes);
    if (pseudo_element == CSS::PseudoElement::SearchText)
        style_engine.set_highlight_observing_elements(CSS::PseudoElement::SearchTextCurrent, style_nodes);
}

void Document::update_highlight_style_observability()
{
    update_highlight_style_observability(CSS::PseudoElement::Selection);
    update_highlight_style_observability(CSS::PseudoElement::SearchText);
    update_highlight_style_observability(CSS::PseudoElement::SpellingError);
    update_highlight_style_observability(CSS::PseudoElement::Highlight);
}

}
