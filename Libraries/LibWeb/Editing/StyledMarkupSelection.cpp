/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/CharacterData.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/StyledMarkupSelection.h>
#include <LibWeb/HTML/HTMLBRElement.h>

namespace Web::Editing {

static bool needs_interchange_newline_after(VisiblePosition const& position)
{
    auto next = position.next();
    if (!next.has_value() || !position.is_end_of_paragraph() || !next->is_start_of_paragraph())
        return false;

    auto upstream_node = next->deep_equivalent().node;
    auto downstream_node = position.deep_equivalent().node;
    return !is<HTML::HTMLBRElement>(*upstream_node) || upstream_node != downstream_node;
}

static GC::Ref<DOM::Range> create_serialization_range(DOM::Range const& source_range, VisiblePosition const& visible_end)
{
    auto start = source_range.start();
    auto end = source_range.end();
    auto rendered_end = visible_end.deep_equivalent();
    // INTEROP: Blink and WebKit serialize an element boundary through the DOM position which represents the visible
    //          end of the selection. This includes structurally empty containers reached by the rendered selection,
    //          even when the raw Range ends at a child offset before them. Keep CharacterData endpoints exact because
    //          canonicalizing a text offset can cross into an adjacent formatting ancestor at the same visual point.
    if (!is<DOM::CharacterData>(*end.node))
        end = rendered_end;
    // INTEROP: A visible selection which reaches the start of an empty paragraph includes the paragraph break.
    //          Keep its placeholder br inside the traversal range so serializing and reinserting the selection
    //          recreates an empty paragraph instead of an empty block with no caret position.
    if (rendered_end.offset == 0 && is_prohibited_paragraph_child(rendered_end.node)
        && rendered_end.node->child_count() == 1 && is<HTML::HTMLBRElement>(*rendered_end.node->first_child())) {
        end = rendered_end;
        end.offset = 1;
    }
    return DOM::Range::create(start.node, start.offset, end.node, end.offset);
}

StyledMarkupSelection::StyledMarkupSelection(DOM::Range& range)
    : m_visible_start(VisiblePosition::create(range.start_container()->document(), range.start()))
    , m_visible_end(VisiblePosition::create(range.start_container()->document(), range.end()))
    , m_previous_visible_end(m_visible_end.previous())
    , m_serialization_range(create_serialization_range(range, m_visible_end))
{
}

bool StyledMarkupSelection::contains_only_interchange_newline() const
{
    if (!needs_interchange_newline_after(m_visible_start) || !m_previous_visible_end.has_value())
        return false;
    auto start = m_visible_start.deep_equivalent();
    auto previous_end = m_previous_visible_end->deep_equivalent();
    return start.node == previous_end.node && start.offset == previous_end.offset;
}

bool StyledMarkupSelection::has_leading_interchange_newline() const
{
    return needs_interchange_newline_after(m_visible_start);
}

bool StyledMarkupSelection::has_trailing_interchange_newline() const
{
    return m_previous_visible_end.has_value() && needs_interchange_newline_after(*m_previous_visible_end);
}

}
