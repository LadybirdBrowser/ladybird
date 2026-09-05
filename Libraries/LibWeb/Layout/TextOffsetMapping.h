/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

class TextOffsetMapping {
public:
    explicit TextOffsetMapping(DOM::Text const&);

    template<typename Callback>
    void for_each_fragment(Callback&& callback) const
    {
        if (!m_primary)
            return;
        auto fragments = RustFFI::layout_arena_text_fragments(m_primary->arena_handle(), Node::slot_id(m_primary));
        for (auto slot : ReadonlySpan<RustFFI::NodeSlotId> { fragments.nodes, fragments.length }) {
            auto const* fragment = as_if<TextNode>(static_cast<Node const*>(RustFFI::layout_arena_node_shell_if_live(m_primary->arena_handle(), slot)));
            if (fragment)
                callback(*fragment);
        }
    }

    TextNode const* fragment_containing(size_t dom_offset) const;

private:
    // TextOffsetMapping is a short-lived stack object, and the layout nodes are kept alive by the document's layout
    // tree for the duration of its use, so there's no need to visit these.
    TextNode const* m_primary { nullptr };
};

}
