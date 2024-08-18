/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Layout/LineBoxFragment.h>

namespace Web::Layout {

class LineBox {
public:
    LineBox(CSS::Direction direction)
        : m_direction(direction)
    {
    }

    CSSPixels width() const { return m_width; }
    CSSPixels height() const { return m_height; }
    CSSPixels bottom() const { return m_bottom; }
    CSSPixels baseline() const { return m_baseline; }

    void add_fragment(Node const& layout_node, int start, int length, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin, CSSPixels content_width, CSSPixels content_height, CSSPixels border_box_top, CSSPixels border_box_bottom, RefPtr<Gfx::GlyphRun> glyph_run = {});

    Vector<LineBoxFragment> const& fragments() const { return m_fragments; }
    Vector<LineBoxFragment>& fragments() { return m_fragments; }

    void trim_trailing_whitespace();

    bool is_empty_or_ends_in_whitespace() const;
    bool is_empty() const { return m_fragments.is_empty() && !m_has_break; }

    AvailableSize original_available_width() const { return m_original_available_width; }

private:
    friend class BlockContainer;
    friend class InlineFormattingContext;
    friend class LineBuilder;

    Vector<LineBoxFragment> m_fragments;
    CSSPixels m_width { 0 };
    CSSPixels m_height { 0 };
    CSSPixels m_bottom { 0 };
    CSSPixels m_baseline { 0 };
    CSS::Direction m_direction { CSS::Direction::Ltr };

    // The amount of available width that was originally available when creating this line box. Used for text justification.
    AvailableSize m_original_available_width { AvailableSize::make_indefinite() };

    bool m_has_break { false };
    bool m_has_forced_break { false };
};

}
