/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/VisualLines.h>

namespace Web::Editing {

VisiblePosition::VisiblePosition(DOM::Document& document, Web::Selection::CaretLocation boundary, Web::Selection::CaretLocation deep_equivalent)
    : m_document(document)
    , m_boundary(::move(boundary))
    , m_deep_equivalent(::move(deep_equivalent))
{
}

VisiblePosition VisiblePosition::create(DOM::Document& document, DOM::BoundaryPoint boundary, TextAffinity affinity)
{
    Web::Selection::CaretLocation location { boundary.node, boundary.offset, affinity };
    Web::Selection::CaretNavigator navigator(document);
    auto deep_equivalent = navigator.canonical_location_for_editing(location);
    deep_equivalent = navigator.upstream_equivalent_location(deep_equivalent);
    return { document, location, deep_equivalent };
}

Optional<VisiblePosition> VisiblePosition::move(Web::Selection::SelectionAlteration alteration, Web::Selection::SelectionDirection direction, Web::Selection::SelectionGranularity granularity, Optional<CSSPixels> preferred_inline_coordinate) const
{
    Web::Selection::CaretNavigator navigator(m_document);
    auto destination = navigator.move(m_boundary, alteration, direction, granularity, preferred_inline_coordinate);
    if (!destination.has_value())
        return {};
    auto deep_equivalent = navigator.canonical_location_for_editing(*destination);
    deep_equivalent = navigator.upstream_equivalent_location(deep_equivalent);
    return VisiblePosition { m_document, *destination, deep_equivalent };
}

static Optional<VisiblePosition> adjacent_visible_position(DOM::Document& document, Web::Selection::CaretLocation const& deep_equivalent, Web::Selection::SelectionDirection direction)
{
    Web::Selection::CaretNavigator navigator(document);
    auto destination = navigator.move(deep_equivalent, Web::Selection::SelectionAlteration::Move, direction, Web::Selection::SelectionGranularity::Character);
    if (!destination.has_value())
        return {};
    return VisiblePosition::create(document, { destination->node, static_cast<WebIDL::UnsignedLong>(destination->offset) }, destination->affinity);
}

Optional<VisiblePosition> VisiblePosition::next() const
{
    // INTEROP: Blink and WebKit find the next visually distinct position from the canonical deep equivalent. Starting
    //          at the DOM boundary instead can count another representation of the same rendered caret as a move.
    return adjacent_visible_position(m_document, m_deep_equivalent, Web::Selection::SelectionDirection::Forward);
}

Optional<VisiblePosition> VisiblePosition::previous() const
{
    return adjacent_visible_position(m_document, m_deep_equivalent, Web::Selection::SelectionDirection::Backward);
}

Optional<CSSPixels> VisiblePosition::inline_coordinate() const
{
    Web::Selection::CaretNavigator navigator(m_document);
    return navigator.inline_coordinate(m_boundary);
}

Optional<DOM::BoundaryPoint> VisiblePosition::canonical_boundary_for_extension(Web::Selection::SelectionDirection direction) const
{
    Web::Selection::CaretNavigator navigator(m_document);
    auto location = navigator.canonical_location_for_extension(m_boundary, direction);
    if (!location.has_value())
        return {};
    return DOM::BoundaryPoint { location->node, static_cast<WebIDL::UnsignedLong>(location->offset) };
}

static bool has_rendered_text_before(DOM::Text const& text, size_t offset)
{
    for (auto const& line : collect_visual_lines(text)) {
        for (auto const* fragment : line.fragments) {
            if (fragment->length_in_code_units() > 0 && fragment->dom_start_offset_in_node() < offset)
                return true;
        }
    }
    return false;
}

static bool has_rendered_text_after(DOM::Text const& text, size_t offset)
{
    for (auto const& line : collect_visual_lines(text)) {
        for (auto const* fragment : line.fragments) {
            if (fragment->length_in_code_units() > 0 && fragment->dom_end_offset_in_node() > offset)
                return true;
        }
    }
    return false;
}

static bool is_rendered_atomic_inline(DOM::Node const& node)
{
    auto const* layout_node = node.layout_node();
    return layout_node && layout_node->is_atomic_inline();
}

enum class AdjacentContent : u8 {
    BlockBoundary,
    ParagraphBoundary,
    RenderedContent,
};

enum class ScanDirection : u8 {
    Backward,
    Forward,
};

static bool has_sibling_toward(DOM::Node const& node, DOM::Node const& block, ScanDirection direction)
{
    for (auto const* ancestor = &node; ancestor != &block; ancestor = ancestor->parent()) {
        if (!ancestor)
            return false;
        if (direction == ScanDirection::Forward ? ancestor->next_sibling() : ancestor->previous_sibling())
            return true;
    }
    return false;
}

static AdjacentContent scan_adjacent_content(Web::Selection::CaretLocation const& position, DOM::Node& block, ScanDirection direction)
{
    auto scan_backward = direction == ScanDirection::Backward;
    if (auto const* text = as_if<DOM::Text>(*position.node)) {
        if ((scan_backward && has_rendered_text_before(*text, position.offset))
            || (!scan_backward && has_rendered_text_after(*text, position.offset)))
            return AdjacentContent::RenderedContent;
    }

    GC::Ptr<DOM::Node> node;
    if (!is<DOM::Text>(*position.node)) {
        if (scan_backward && position.offset > 0) {
            node = position.node->child_at_index(position.offset - 1);
            while (node && node->last_child())
                node = node->last_child();
        } else if (!scan_backward && position.offset < position.node->child_count()) {
            node = position.node->child_at_index(position.offset);
        }
    }
    if (!node && scan_backward) {
        node = position.node->previous_in_pre_order();
    } else if (!node) {
        // A boundary at the end of an element is after that element's subtree. next_in_pre_order() would enter its
        // first child and rescan content which is before the boundary, so climb until a following sibling is found.
        for (auto* ancestor = position.node.ptr(); ancestor && ancestor != &block; ancestor = ancestor->parent()) {
            if (ancestor->next_sibling()) {
                node = ancestor->next_sibling();
                break;
            }
        }
    }

    while (node && node != &block && block.is_inclusive_ancestor_of(*node)) {
        if (is<HTML::HTMLBRElement>(*node)) {
            // INTEROP: A br at the outer edge of its containing block is a caret placeholder, not an internal
            //          paragraph boundary. Blink and WebKit therefore treat the position before a trailing br as the
            //          end of both the paragraph and its block when planning a rich selection replacement.
            if (!has_sibling_toward(*node, block, direction))
                return AdjacentContent::BlockBoundary;
            return AdjacentContent::ParagraphBoundary;
        }
        if (is_block_node(*node))
            return AdjacentContent::ParagraphBoundary;
        if (auto const* text = as_if<DOM::Text>(*node); text
            && (scan_backward ? has_rendered_text_before(*text, text->length()) : has_rendered_text_after(*text, 0)))
            return AdjacentContent::RenderedContent;
        if (is_rendered_atomic_inline(*node))
            return AdjacentContent::RenderedContent;
        node = scan_backward ? node->previous_in_pre_order() : node->next_in_pre_order(&block);
    }
    return AdjacentContent::BlockBoundary;
}

bool VisiblePosition::is_start_of_paragraph() const
{
    m_document->update_layout_if_needed_for_node(m_boundary.node, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto block = block_node_of_node(m_boundary.node);
    if (!block)
        return false;

    return scan_adjacent_content(m_boundary, *block, ScanDirection::Backward) != AdjacentContent::RenderedContent;
}

bool VisiblePosition::is_end_of_paragraph() const
{
    m_document->update_layout_if_needed_for_node(m_boundary.node, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto block = block_node_of_node(m_boundary.node);
    if (!block)
        return false;

    return scan_adjacent_content(m_boundary, *block, ScanDirection::Forward) != AdjacentContent::RenderedContent;
}

bool VisiblePosition::is_start_of_containing_block() const
{
    m_document->update_layout_if_needed_for_node(m_boundary.node, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto block = block_node_of_node(m_boundary.node);
    if (!block)
        return false;

    return scan_adjacent_content(m_boundary, *block, ScanDirection::Backward) == AdjacentContent::BlockBoundary;
}

bool VisiblePosition::is_end_of_containing_block() const
{
    m_document->update_layout_if_needed_for_node(m_boundary.node, DOM::UpdateLayoutReason::CursorLineNavigation);
    auto block = block_node_of_node(m_boundary.node);
    if (!block)
        return false;

    return scan_adjacent_content(m_boundary, *block, ScanDirection::Forward) == AdjacentContent::BlockBoundary;
}

bool VisiblePosition::is_before_or_after_containing_block() const
{
    auto block = block_node_of_node(m_deep_equivalent.node);
    if (!block || m_deep_equivalent.node.ptr() != block)
        return false;

    // INTEROP: Blink and WebKit Positions retain whether a block-owned offset is anchored before or after an atomic
    //          inline. DOM::BoundaryPoint has no anchor type, but an adjacent rendered atomic child proves that the
    //          position is inside the paragraph rather than outside its containing block.
    auto* child_after = block->child_at_index(m_deep_equivalent.offset);
    auto* child_before = m_deep_equivalent.offset > 0 ? block->child_at_index(m_deep_equivalent.offset - 1) : nullptr;
    auto is_adjacent_atomic = [](DOM::Node const* child) {
        return child && is_rendered_atomic_inline(*child);
    };
    return !is_adjacent_atomic(child_before) && !is_adjacent_atomic(child_after);
}

}
