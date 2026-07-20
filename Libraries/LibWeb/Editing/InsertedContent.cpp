/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibWeb/DOM/DocumentFragment.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/InsertedContent.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/MutationTrackedRange.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Selection/CaretNavigation.h>

namespace Web::Editing {

InsertedContent::InsertedContent(GC::RootVector<GC::Ref<DOM::Node>>&& nodes)
    : m_nodes(move(nodes))
{
}

GC::Ref<DOM::Node> InsertedContent::first_node() const
{
    return m_nodes.first_node();
}

GC::RootVector<GC::Ref<DOM::Node>> InsertedContent::nodes() const
{
    return m_nodes.connected_nodes();
}

GC::Ref<DOM::Node> InsertedContent::last_node() const
{
    return m_nodes.last_node();
}

GC::RootVector<GC::Ref<DOM::Text>> InsertedContent::text_nodes() const
{
    return m_nodes.connected_text_nodes();
}

bool InsertedContent::contains(DOM::Text const& candidate) const
{
    return m_nodes.contains(candidate);
}

DOM::BoundaryPoint InsertedContent::start_boundary() const
{
    VERIFY(m_endpoints);
    return m_endpoints->start();
}

DOM::BoundaryPoint InsertedContent::completion_start_boundary() const
{
    VERIFY(m_replacement_range);
    VERIFY(m_replacement_topology.has_value());

    // INTEROP: Blink completes a replacement at the mutation-adjusted replacement position when an inline fragment
    //          starts with a text node, or when a block fragment's unwrapped leading text was integrated into the
    //          destination paragraph. Wrapped inline content and non-integrated blocks retain their own text seams.
    auto completion_begins_with_text = m_nodes.leading_content_kind() == InsertedNodes::LeadingContentKind::TextNode;
    if (*m_replacement_topology == ReplacementTopology::Block)
        completion_begins_with_text = m_start_paragraph_was_integrated
            && m_nodes.leading_content_kind() != InsertedNodes::LeadingContentKind::Other;
    return completion_begins_with_text && m_replacement_was_inside_inline_content
        ? m_replacement_range->range().start()
        : start_boundary();
}

DOM::BoundaryPoint InsertedContent::insertion_boundary() const
{
    VERIFY(m_insertion_range);
    return m_insertion_range->range().start();
}

DOM::BoundaryPoint InsertedContent::end_boundary() const
{
    VERIFY(m_endpoints);
    if (auto atomic_boundary = end_boundary_after_atomic_content(); atomic_boundary.has_value())
        return *atomic_boundary;
    auto boundary = m_endpoints->end();
    if (!is<DOM::Text>(*boundary.node) && boundary.offset > 0) {
        if (auto* preceding_child = boundary.node->child_at_index(boundary.offset - 1))
            return end_boundary_of_node(*preceding_child);
    }
    return boundary;
}

Optional<DOM::BoundaryPoint> InsertedContent::end_boundary_after_atomic_content() const
{
    auto last_atomic_node = m_nodes.last_atomic_node();
    if (!last_atomic_node || !last_atomic_node->parent())
        return {};
    return end_boundary_of_node(*last_atomic_node);
}

static DOM::BoundaryPoint start_boundary_of(DOM::Node& node)
{
    if (auto* text = as_if<DOM::Text>(node))
        return { *text, 0 };
    if (auto* first_child = node.first_child())
        return start_boundary_of(*first_child);
    return { node, 0 };
}

void InsertedContent::insert_before(DOM::Node& parent, GC::Ptr<DOM::Node> reference_node)
{
    auto insertion_offset = reference_node
        ? static_cast<WebIDL::UnsignedLong>(reference_node->index())
        : static_cast<WebIDL::UnsignedLong>(parent.child_count());
    track_insertion_boundary({ parent, insertion_offset });

    for (auto& node : m_nodes.connected_nodes())
        insert_node_before(node, parent, reference_node);
}

void InsertedContent::insert_into_range(DOM::Range& range, DOM::DocumentFragment& fragment)
{
    track_insertion_boundary(range.start());
    MUST(insert_node_into_range(range, fragment));
}

void InsertedContent::track_insertion_boundary(DOM::BoundaryPoint boundary)
{
    VERIFY(!m_insertion_range);
    // INTEROP: Completion treats a protected whitespace run before terminal atomic content differently inside a
    //          paragraph than at its boundary. Record that topology before insertion changes the visible positions.
    auto visible_position = VisiblePosition::create(boundary.node->document(), boundary);
    m_insertion_was_inside_paragraph = !visible_position.is_start_of_paragraph() && !visible_position.is_end_of_paragraph();
    if (auto previous_position = visible_position.previous(); previous_position.has_value()) {
        auto previous_boundary = previous_position->deep_equivalent();
        if (auto* text = as_if<DOM::Text>(*previous_boundary.node); text && text->length_in_utf16_code_units() > 0) {
            // An upstream-equivalent position can be represented by the end of the preceding text node. In that case
            // the code unit before the boundary, rather than the out-of-range code unit at the boundary, is relevant.
            auto code_unit_offset = min(previous_boundary.offset, text->length_in_utf16_code_units() - 1);
            auto code_unit = text->data().code_unit_at(code_unit_offset);
            m_insertion_was_after_whitespace = is_ascii_space(code_unit) || code_unit == 0xa0;
        }
    }
    // INTEROP: Blink retains the insertion position separately from the first inserted node. In particular, a block
    //          insertion can move or unwrap that node while the destination-side whitespace seam remains observable.
    m_insertion_range = make<MutationTrackedRange>(DOM::Range::create(boundary.node, boundary.offset, boundary.node, boundary.offset));
}

void InsertedContent::track_replacement_boundary(DOM::BoundaryPoint boundary, ReplacementTopology topology)
{
    VERIFY(!m_replacement_range);
    VERIFY(!m_replacement_topology.has_value());
    m_replacement_topology = topology;

    // INTEROP: Blink and WebKit retain the replacement position across the topology changes made while inserting a
    //          fragment. This is distinct from both the structural insertion position and the inserted content range.
    m_replacement_range = make<MutationTrackedRange>(DOM::Range::create(boundary.node, boundary.offset, boundary.node, boundary.offset));
    for (auto* ancestor = boundary.node.ptr(); ancestor && !is_block_node(*ancestor); ancestor = ancestor->parent()) {
        if (is<DOM::Element>(*ancestor)) {
            m_replacement_was_inside_inline_content = true;
            break;
        }
    }
}

void InsertedContent::begin_tracking_content_boundaries()
{
    // INTEROP: Blink and WebKit track inserted nodes while preparing the fragment, then establish the logical content
    //          range after redundant style removal has finalized its structural edges. Subsequent paragraph movement
    //          tracks these positions through DOM mutations instead of reconstructing the final caret from nodes.
    VERIFY(!m_endpoints);
    auto start = start_boundary_of(*m_nodes.first_node());
    auto end = end_boundary_of_node(*m_nodes.last_node());
    m_endpoints = make<ReplacementEndpoints>(start, end);
}

void InsertedContent::did_replace_node(DOM::Node& node, DOM::Node& replacement)
{
    did_replace_node(node, replacement, replacement);
}

void InsertedContent::begin_completion(DOM::BoundaryPoint end)
{
    VERIFY(m_endpoints);

    // INTEROP: Blink and WebKit stop consulting their structural inserted-node tracker once replacement completion
    //          begins. Resolve the semantic start once, then carry both endpoints explicitly through every mutation.
    m_endpoints->set_start(completion_start_boundary());
    m_endpoints->set_end(end);
}

void InsertedContent::did_replace_node(DOM::Node& node, DOM::Node& first_replacement, DOM::Node& last_replacement)
{
    m_nodes.did_replace_node(node, first_replacement, last_replacement);
}

void InsertedContent::did_move_start_paragraph(DOM::BoundaryPoint start)
{
    VERIFY(m_endpoints);
    m_start_paragraph_was_integrated = true;
    Web::Selection::CaretNavigator navigator(start.node->document());
    auto location = navigator.canonical_location_for_editing({ start.node, start.offset });

    // INTEROP: Blink re-establishes the start of inserted content at the most-forward equivalent caret after moving
    //          its first paragraph. The move can unwrap the original boundary container without orphaning its text.
    location = navigator.canonical_location_for_extension(location, Web::Selection::SelectionDirection::Forward).value_or(location);
    m_endpoints->set_start({ location.node, static_cast<WebIDL::UnsignedLong>(location.offset) });
}

void InsertedContent::will_integrate_end_paragraph(DOM::Node& paragraph)
{
    VERIFY(m_endpoints);
    m_end_paragraph_was_integrated = true;

    // INTEROP: Blink and WebKit move paragraphs with a nested replacement. If start integration folded the tracked
    //          replacement start into the paragraph which is subsequently moved for end integration, that nested
    //          replacement invalidates the old start and its boundary becomes an additional completion seam.
    if (m_start_paragraph_was_integrated && paragraph.is_inclusive_ancestor_of(*m_endpoints->start().node))
        m_normalize_end_text_seam = true;
}

}
