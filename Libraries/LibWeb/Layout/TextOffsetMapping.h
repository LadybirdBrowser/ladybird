/*
 * Copyright (c) 2026, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Forward.h>
#include <LibWeb/Layout/Box.h>
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
    TraversalDecision for_each_paintable_fragment(Callback&& callback) const
    {
        auto decision = TraversalDecision::Continue;
        for_each_fragment([&](TextNode const& fragment) {
            if (decision == TraversalDecision::Break)
                return;

            auto const* containing_block = fragment.containing_block();
            if (!containing_block)
                return;
            auto paintable_box = containing_block->paintable_box();
            if (!paintable_box)
                return;

            decision = paintable_box->template for_each_in_inclusive_subtree_of_type<Painting::PaintableWithLines>([&](auto const& paintable_with_lines) {
                for (auto const& paintable_fragment : paintable_with_lines.fragments()) {
                    if (&paintable_fragment.layout_node() != &fragment)
                        continue;
                    if (callback(paintable_fragment) == TraversalDecision::Break)
                        return TraversalDecision::Break;
                }
                return TraversalDecision::Continue;
            });
        });
        return decision;
    }

    template<typename Callback>
    TraversalDecision for_each_paintable_fragment(Callback&& callback)
    {
        auto decision = TraversalDecision::Continue;
        for_each_fragment([&](TextNode& fragment) {
            if (decision == TraversalDecision::Break)
                return;

            auto* containing_block = fragment.containing_block();
            if (!containing_block)
                return;
            auto paintable_box = containing_block->paintable_box();
            if (!paintable_box)
                return;

            decision = paintable_box->template for_each_in_inclusive_subtree_of_type<Painting::PaintableWithLines>([&](auto& paintable_with_lines) {
                for (auto& paintable_fragment : paintable_with_lines.fragments()) {
                    if (&paintable_fragment.layout_node() != &fragment)
                        continue;
                    if (callback(paintable_fragment) == TraversalDecision::Break)
                        return TraversalDecision::Break;
                }
                return TraversalDecision::Continue;
            });
        });
        return decision;
    }

    template<typename Callback>
    void for_each_paintable_fragment_in_dom_range(size_t dom_start, size_t dom_end, Callback&& callback) const
    {
        for_each_paintable_fragment([&](auto const& paintable_fragment) {
            auto const fragment_dom_start = paintable_fragment.dom_start_offset_in_node();
            auto const fragment_dom_end = paintable_fragment.dom_end_offset_in_node();
            if (fragment_dom_end <= dom_start || fragment_dom_start >= dom_end)
                return TraversalDecision::Continue;
            callback(paintable_fragment);
            return TraversalDecision::Continue;
        });
    }

    template<typename Callback>
    void for_each_paintable_fragment_in_dom_range(size_t dom_start, size_t dom_end, Callback&& callback)
    {
        for_each_paintable_fragment([&](auto& paintable_fragment) {
            auto const fragment_dom_start = paintable_fragment.dom_start_offset_in_node();
            auto const fragment_dom_end = paintable_fragment.dom_end_offset_in_node();
            if (fragment_dom_end <= dom_start || fragment_dom_start >= dom_end)
                return TraversalDecision::Continue;
            callback(paintable_fragment);
            return TraversalDecision::Continue;
        });
    }

private:
    // TextOffsetMapping is a short-lived stack object, and the layout nodes are kept alive by the document's layout
    // tree for the duration of its use, so there's no need to visit these.
    TextNode const* m_primary { nullptr };
    TextSliceNode const* m_first_letter_slice { nullptr };
};

}
