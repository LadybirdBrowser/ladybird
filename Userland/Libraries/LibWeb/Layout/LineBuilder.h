/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/InlineFormattingContext.h>

namespace Web::Layout {

class LineBuilder {
    AK_MAKE_NONCOPYABLE(LineBuilder);
    AK_MAKE_NONMOVABLE(LineBuilder);

public:
    LineBuilder(InlineFormattingContext&, LayoutState&, LayoutState::UsedValues& containing_block_used_values, CSS::Direction);
    ~LineBuilder();

    enum class ForcedBreak {
        No,
        Yes,
    };

    void break_line(ForcedBreak, Optional<CSSPixels> next_item_width = {});
    void append_box(Box const&, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin);
    void append_text_chunk(TextNode const&, size_t offset_in_node, size_t length_in_node, CSSPixels leading_size, CSSPixels trailing_size, CSSPixels leading_margin, CSSPixels trailing_margin, CSSPixels content_width, CSSPixels content_height, RefPtr<Gfx::GlyphRun>);

    // Returns whether a line break occurred.
    bool break_if_needed(CSSPixels next_item_width)
    {
        if (should_break(next_item_width)) {
            break_line(LineBuilder::ForcedBreak::No, next_item_width);
            return true;
        }
        return false;
    }

    void update_last_line();

    void remove_last_line_if_empty();

    CSSPixels current_y() const { return m_current_y; }
    void set_current_y(CSSPixels y) { m_current_y = y; }

    void recalculate_available_space();
    CSSPixels y_for_float_to_be_inserted_here(Box const&);

    auto& inline_formatting_context() { return m_context; }

private:
    void begin_new_line(bool increment_y, bool is_first_break_in_sequence = true);

    bool should_break(CSSPixels next_item_width);

    LineBox& ensure_last_line_box();

    InlineFormattingContext& m_context;
    LayoutState& m_layout_state;
    LayoutState::UsedValues& m_containing_block_used_values;
    AvailableSize m_available_width_for_current_line { AvailableSize::make_indefinite() };
    CSSPixels m_current_y { 0 };
    CSSPixels m_max_height_on_current_line { 0 };
    CSSPixels m_text_indent { 0 };
    CSS::Direction m_direction { CSS::Direction::Ltr };

    bool m_last_line_needs_update { false };
};

}
