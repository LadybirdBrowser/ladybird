/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Text.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/TextOffsetMapping.h>

namespace Web::Layout {

TextOffsetMapping::TextOffsetMapping(DOM::Text const& text)
{
    m_primary = as_if<TextNode>(text.unsafe_layout_node());
    if (auto* primary_slice = as_if<TextSliceNode>(m_primary.ptr()))
        m_first_letter_slice = primary_slice->first_letter_slice();
}

TextNode const* TextOffsetMapping::fragment_containing(size_t dom_offset) const
{
    auto contains = [&](TextNode const& fragment) {
        auto const start = fragment.dom_start_offset();
        return dom_offset >= start && dom_offset <= start + fragment.dom_length();
    };

    if (m_first_letter_slice && contains(*m_first_letter_slice))
        return m_first_letter_slice;
    if (m_primary && contains(*m_primary))
        return m_primary;
    return nullptr;
}

}
