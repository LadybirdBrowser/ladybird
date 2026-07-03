/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/Layout/LineBoxFragment.h>

namespace Web::Layout {

class Box;

class LineBox {
public:
    struct StaticPositionMarker {
        Box const* box { nullptr };
        CSSPixels inline_offset { 0 };
        CSSPixels block_offset { 0 };
        CSS::WritingMode writing_mode { CSS::WritingMode::HorizontalTb };
        bool preceded_by_in_flow_content { false };

        CSSPixelPoint offset() const
        {
            if (writing_mode != CSS::WritingMode::HorizontalTb)
                return { block_offset, inline_offset };
            return { inline_offset, block_offset };
        }
    };

    LineBox(CSS::Direction direction, CSS::WritingMode writing_mode)
        : m_direction(direction)
        , m_writing_mode(writing_mode)
    {
    }

    CSSPixels width() const;
    CSSPixels height() const;
    CSSPixels bottom() const;

    CSSPixels inline_length() const { return m_inline_length; }
    CSSPixels block_length() const { return m_block_length; }
    CSSPixels baseline() const { return m_baseline; }

    void add_fragment(Node const& layout_node, size_t start, size_t length, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin, CSSPixels content_width, CSSPixels content_height, CSSPixels border_box_top, CSSPixels border_box_bottom, RefPtr<Gfx::GlyphRun> glyph_run = {});
    void add_static_position_marker(Box const&);

    Vector<LineBoxFragment> const& fragments() const { return m_fragments; }
    Vector<LineBoxFragment>& fragments() { return m_fragments; }
    Vector<StaticPositionMarker> const& static_position_markers() const { return m_static_position_markers; }
    Vector<StaticPositionMarker>& static_position_markers() { return m_static_position_markers; }

    CSSPixels get_trailing_whitespace_width() const;
    void trim_trailing_whitespace();
    void clamp_static_position_markers_to_inline_length();

    bool is_empty_or_ends_in_whitespace() const;
    bool is_empty() const { return m_fragments.is_empty() && !m_has_break; }
    bool has_forced_break() const { return m_has_forced_break; }

    AvailableSize original_available_width() const { return m_original_available_width; }

private:
    friend class BlockContainer;
    friend class InlineFormattingContext;
    friend class LineBuilder;

    enum class RemoveTrailingWhitespace : u8 {
        Yes,
        No,
    };

    CSSPixels calculate_or_trim_trailing_whitespace(RemoveTrailingWhitespace);

    Vector<LineBoxFragment> m_fragments;
    Vector<StaticPositionMarker> m_static_position_markers;
    CSSPixels m_inline_length { 0 };
    CSSPixels m_block_length { 0 };
    CSSPixels m_bottom { 0 };
    CSSPixels m_baseline { 0 };
    CSS::Direction m_direction { CSS::Direction::Ltr };
    CSS::WritingMode m_writing_mode { CSS::WritingMode::HorizontalTb };

    // The amount of available width that was originally available when creating this line box. Used for text justification.
    AvailableSize m_original_available_width { AvailableSize::make_indefinite() };

    bool m_has_break { false };
    bool m_has_forced_break { false };
};

}
