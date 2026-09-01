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

    TextNode const* primary() const { return m_primary; }

    template<typename Callback>
    void for_each_fragment(Callback&& callback) const
    {
        if (m_first_letter_slice)
            callback(static_cast<TextNode const&>(*m_first_letter_slice));
        if (m_primary)
            callback(*m_primary);
    }

    template<typename Callback>
    void for_each_fragment(Callback&& callback)
    {
        if (m_first_letter_slice)
            callback(const_cast<TextNode&>(static_cast<TextNode const&>(*m_first_letter_slice)));
        if (m_primary)
            callback(const_cast<TextNode&>(*m_primary));
    }

    Vector<RustFFI::NodeSlotId, 2> slot_ids() const
    {
        Vector<RustFFI::NodeSlotId, 2> slots;
        for_each_fragment([&](TextNode const& slice) {
            slots.append(Node::slot_id(&slice));
        });
        return slots;
    }

    TextNode const* fragment_containing(size_t dom_offset) const;

private:
    // TextOffsetMapping is a short-lived stack object, and the layout nodes are kept alive by the document's layout
    // tree for the duration of its use, so there's no need to visit these.
    TextNode const* m_primary { nullptr };
    TextSliceNode const* m_first_letter_slice { nullptr };
};

}
