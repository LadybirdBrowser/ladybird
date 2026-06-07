/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/Paintable.h>
#include <LibWeb/Painting/PaintableFragment.h>
#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Layout {

class TextOffsetMapping {
public:
    explicit TextOffsetMapping(DOM::Text const&);

    bool is_split() const { return m_first_letter_slice != nullptr; }
    TextNode const* primary() const { return m_primary; }
    TextSliceNode const* first_letter_slice() const { return m_first_letter_slice; }

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

    TextNode const* fragment_containing(size_t dom_offset) const;

    template<typename Callback>
    void for_each_paintable_fragment_in_dom_range(size_t dom_start, size_t dom_end, Callback&& callback) const
    {
        for_each_fragment([&](TextNode const& fragment) {
            auto fragment_paintable = fragment.first_paintable();
            if (!fragment_paintable)
                return;

            auto paintable_with_lines = fragment_paintable->template first_ancestor_of_type<Painting::PaintableWithLines>();
            if (!paintable_with_lines)
                return;
            for (auto const& paintable_fragment : paintable_with_lines->fragments()) {
                if (&paintable_fragment.paintable() != fragment_paintable)
                    continue;
                auto const fragment_dom_start = paintable_fragment.dom_start_offset_in_node();
                auto const fragment_dom_end = paintable_fragment.dom_end_offset_in_node();
                if (fragment_dom_end <= dom_start || fragment_dom_start >= dom_end)
                    continue;
                callback(paintable_fragment);
            }
        });
    }

private:
    // TextOffsetMapping is a short-lived stack object, and the layout nodes are kept alive by the document's layout
    // tree for the duration of its use, so there's no need to visit these.
    TextNode const* m_primary { nullptr };
    TextSliceNode const* m_first_letter_slice { nullptr };
};

}
