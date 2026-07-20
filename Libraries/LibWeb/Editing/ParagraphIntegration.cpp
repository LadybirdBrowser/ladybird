/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Editing/InsertedContent.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/ParagraphIntegration.h>
#include <LibWeb/Editing/VisiblePosition.h>
#include <LibWeb/HTML/HTMLBRElement.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/TagNames.h>

namespace Web::Editing {

ParagraphIntegration::ParagraphIntegration(DOM::Document& document, InsertedContent const& inserted_content, ParagraphBoundaryState boundary_state)
    : m_document(document)
    , m_inserted_content(inserted_content)
    , m_boundary_state(boundary_state)
{
}

static GC::Ptr<DOM::Node> nearest_inclusive_ancestor_with_tag(DOM::Node& node, Utf16FlyString const& tag_name)
{
    for (auto* ancestor = &node; ancestor; ancestor = ancestor->parent()) {
        auto* element = as_if<DOM::Element>(*ancestor);
        if (element && element->local_name() == tag_name)
            return *element;
    }
    return nullptr;
}

static GC::Ptr<DOM::Node> enclosing_table_cell(DOM::Node& node)
{
    for (auto* ancestor = &node; ancestor; ancestor = ancestor->parent()) {
        auto* element = as_if<DOM::Element>(*ancestor);
        if (element && element->local_name().is_one_of(HTML::TagNames::td, HTML::TagNames::th))
            return *element;
    }
    return nullptr;
}

static bool is_heading(DOM::Node const& node)
{
    auto const* element = as_if<DOM::Element>(node);
    return element && element->local_name().is_one_of(HTML::TagNames::h1, HTML::TagNames::h2, HTML::TagNames::h3, HTML::TagNames::h4, HTML::TagNames::h5, HTML::TagNames::h6);
}

static bool can_merge_paragraphs(VisiblePosition const& source, VisiblePosition const& destination)
{
    auto source_position = source.deep_equivalent();
    auto destination_position = destination.deep_equivalent();
    auto source_block = block_node_of_node(source_position.node);
    auto destination_block = block_node_of_node(destination_position.node);
    if (!source_block || !destination_block)
        return false;

    auto* source_block_element = as_if<DOM::Element>(*source_block);
    if (source_block_element && source_block_element->local_name() == HTML::TagNames::blockquote)
        return false;

    // INTEROP: Blink and WebKit intentionally compare the source block's list child with the destination position's
    //          list child. The asymmetry preserves a destination caret's list context when its enclosing block is a
    //          container outside that list. Table cells, in contrast, are compared from both exact positions.
    if (nearest_inclusive_ancestor_with_tag(*source_block, HTML::TagNames::li)
        != nearest_inclusive_ancestor_with_tag(*destination_position.node, HTML::TagNames::li))
        return false;
    if (enclosing_table_cell(*source_position.node) != enclosing_table_cell(*destination_position.node))
        return false;

    if (is_heading(*source_block)) {
        auto const* source_element = as_if<DOM::Element>(*source_block);
        auto const* destination_element = as_if<DOM::Element>(*destination_block);
        if (!source_element || !destination_element || source_element->local_name() != destination_element->local_name())
            return false;
    }

    // A position before or after a block is not a paragraph move. A block-owned position adjacent to atomic inline
    // content is an interior caret whose anchor type was lost when represented as a DOM boundary.
    return !source.is_before_or_after_containing_block()
        && !destination.is_before_or_after_containing_block();
}

static Optional<VisiblePosition> adjacent_position(VisiblePosition const& position, Web::Selection::SelectionDirection direction)
{
    return position.move(
        Web::Selection::SelectionAlteration::Move,
        direction,
        Web::Selection::SelectionGranularity::Character);
}

bool ParagraphIntegration::should_merge_start() const
{
    if (m_boundary_state.selection_start_was_start_of_paragraph)
        return false;

    auto start = VisiblePosition::create(m_document, m_inserted_content.start_boundary());
    auto previous = adjacent_position(start, Web::Selection::SelectionDirection::Backward);
    if (!previous.has_value() || !start.is_start_of_paragraph())
        return false;
    if (is<HTML::HTMLBRElement>(*start.deep_equivalent().node))
        return false;

    auto source_block = block_node_of_node(start.deep_equivalent().node);
    if (source_block && start.deep_equivalent().node == source_block && is_non_list_single_line_container(*source_block)
        && !has_visible_children(*source_block)) {
        // INTEROP: A boundary-only clipboard paragraph may be represented by an empty block with no rendered caret
        //          inside it. Blink folds that transport wrapper into the preceding paragraph instead of leaving an
        //          empty nested block behind.
        return true;
    }
    return can_merge_paragraphs(start, *previous);
}

bool ParagraphIntegration::should_merge_end() const
{
    if (m_boundary_state.selection_end_was_end_of_paragraph)
        return false;

    auto end = VisiblePosition::create(m_document, m_inserted_content.end_boundary());
    auto next = adjacent_position(end, Web::Selection::SelectionDirection::Forward);
    if (!next.has_value() || !end.is_end_of_paragraph())
        return false;
    if (is<HTML::HTMLBRElement>(*end.deep_equivalent().node))
        return false;
    return can_merge_paragraphs(end, *next);
}

ParagraphIntegrationDecision ParagraphIntegration::decide() const
{
    // INTEROP: Determine both seams before moving either paragraph. A start merge can change the visible block at the
    //          other end of a one-paragraph insertion, but it must not change whether the original end seam merges.
    return {
        .merge_start = should_merge_start(),
        .merge_end = should_merge_end(),
    };
}

}
