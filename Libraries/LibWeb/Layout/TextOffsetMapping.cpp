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
}

TextNode const* TextOffsetMapping::fragment_containing(size_t dom_offset) const
{
    if (!m_primary)
        return nullptr;
    auto slot = RustFFI::layout_arena_text_fragment_containing(m_primary->arena_handle(), Node::slot_id(m_primary), dom_offset, m_primary->text().length_in_code_units());
    return as_if<TextNode>(static_cast<Node const*>(RustFFI::layout_arena_node_shell_if_live(m_primary->arena_handle(), slot)));
}

}
