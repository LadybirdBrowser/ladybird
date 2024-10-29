/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Rect.h>
#include <LibGfx/TextLayout.h>
#include <LibJS/Heap/GCPtr.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

class LineBoxFragment {
    friend class LineBox;

public:
    LineBoxFragment(Node const& layout_node, int start, int length, CSSPixels inline_offset, CSSPixels block_offset, CSSPixels inline_length, CSSPixels block_length, CSSPixels border_box_top, CSS::Direction, RefPtr<Gfx::GlyphRun>);

    Node const& layout_node() const { return m_layout_node; }
    int start() const { return m_start; }
    int length() const { return m_length; }
    CSSPixelRect const absolute_rect() const;

    CSSPixelPoint offset() const;
    CSSPixels inline_offset() const { return m_inline_offset; }
    CSSPixels block_offset() const { return m_block_offset; }
    void set_inline_offset(CSSPixels inline_offset) { m_inline_offset = inline_offset; }
    void set_block_offset(CSSPixels block_offset) { m_block_offset = block_offset; }

    // The baseline of a fragment is the number of pixels from the top to the text baseline.
    void set_baseline(CSSPixels y) { m_baseline = y; }
    CSSPixels baseline() const { return m_baseline; }

    CSSPixelSize size() const;
    CSSPixels width() const { return size().width(); }
    CSSPixels height() const { return size().height(); }
    CSSPixels inline_length() const { return m_inline_length; }
    CSSPixels block_length() const { return m_block_length; }
    void set_inline_length(CSSPixels inline_length) { m_inline_length = inline_length; }
    void set_block_length(CSSPixels block_length) { m_block_length = block_length; }

    CSSPixels border_box_top() const { return m_border_box_top; }

    bool ends_in_whitespace() const;
    bool is_justifiable_whitespace() const;
    StringView text() const;

    bool is_atomic_inline() const;

    RefPtr<Gfx::GlyphRun> glyph_run() const { return m_glyph_run; }
    void append_glyph_run(RefPtr<Gfx::GlyphRun> const&, CSSPixels run_width);

private:
    CSS::Direction resolve_glyph_run_direction(Gfx::GlyphRun::TextType) const;
    void append_glyph_run_ltr(RefPtr<Gfx::GlyphRun> const&, CSSPixels run_width);
    void append_glyph_run_rtl(RefPtr<Gfx::GlyphRun> const&, CSSPixels run_width);

    JS::NonnullGCPtr<Node const> m_layout_node;
    int m_start { 0 };
    int m_length { 0 };
    CSSPixels m_inline_offset;
    CSSPixels m_block_offset;
    CSSPixels m_inline_length;
    CSSPixels m_block_length;
    CSSPixels m_border_box_top { 0 };
    CSSPixels m_baseline { 0 };
    CSS::Direction m_direction { CSS::Direction::Ltr };

    RefPtr<Gfx::GlyphRun> m_glyph_run;
    float m_insert_position { 0 };
    CSS::Direction m_current_insert_direction { CSS::Direction::Ltr };
};

}
