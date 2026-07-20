/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/ReplacementEndpoints.h>

namespace Web::Editing {

ReplacementEndpoints::ReplacementEndpoints(DOM::BoundaryPoint start, DOM::BoundaryPoint end)
    : m_start(make<MutationTrackedRange>(DOM::Range::create(start.node, start.offset, start.node, start.offset)))
    , m_end(make<MutationTrackedRange>(DOM::Range::create(end.node, end.offset, end.node, end.offset)))
{
}

DOM::BoundaryPoint ReplacementEndpoints::start() const
{
    return m_start->range().start();
}

DOM::BoundaryPoint ReplacementEndpoints::end() const
{
    return m_end->range().start();
}

void ReplacementEndpoints::set_start(DOM::BoundaryPoint start)
{
    MUST(m_start->range().set_start(start.node, start.offset));
    MUST(m_start->range().set_end(start.node, start.offset));
}

void ReplacementEndpoints::set_end(DOM::BoundaryPoint end)
{
    MUST(m_end->range().set_start(end.node, end.offset));
    MUST(m_end->range().set_end(end.node, end.offset));
}

void ReplacementEndpoints::prepare_for_merging_left_text_into_right(DOM::Text& left, DOM::Text& right, u32 left_length)
{
    relocate_for_merging_left_text_into_right(*m_start, left, right, left_length);
    relocate_for_merging_left_text_into_right(*m_end, left, right, left_length);
}

void ReplacementEndpoints::prepare_for_merging_right_text_into_left(DOM::Text& left, DOM::Text& right, u32 left_length)
{
    relocate_for_merging_right_text_into_left(*m_start, left, right, left_length);
    relocate_for_merging_right_text_into_left(*m_end, left, right, left_length);
}

void ReplacementEndpoints::relocate_for_merging_left_text_into_right(MutationTrackedRange& endpoint, DOM::Text& left, DOM::Text& right, u32 left_length)
{
    auto boundary = endpoint.range().start();
    // Inserting at offset zero has already shifted live points with nonzero offsets. Offset zero itself stays put and
    // must be moved explicitly to retain its position relative to the text which used to begin the right node.
    if (boundary.node.ptr() == &right && boundary.offset == 0)
        boundary.offset = left_length;
    else if (boundary.node.ptr() == &left)
        boundary.node = right;
    MUST(endpoint.range().set_start(boundary.node, boundary.offset));
    MUST(endpoint.range().set_end(boundary.node, boundary.offset));
}

void ReplacementEndpoints::relocate_for_merging_right_text_into_left(MutationTrackedRange& endpoint, DOM::Text& left, DOM::Text& right, u32 left_length)
{
    auto boundary = endpoint.range().start();
    if (boundary.node.ptr() == &right) {
        boundary.node = left;
        boundary.offset += left_length;
    }
    MUST(endpoint.range().set_start(boundary.node, boundary.offset));
    MUST(endpoint.range().set_end(boundary.node, boundary.offset));
}

}
