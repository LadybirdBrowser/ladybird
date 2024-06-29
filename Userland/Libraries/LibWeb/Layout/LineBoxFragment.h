/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
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
    LineBoxFragment(Node const& layout_node, int start, int length, CSSPixelPoint offset, CSSPixelSize size, CSSPixels border_box_top, RefPtr<Gfx::GlyphRun> glyph_run)
        : m_layout_node(layout_node)
        , m_start(start)
        , m_length(length)
        , m_offset(offset)
        , m_size(size)
        , m_border_box_top(border_box_top)
        , m_glyph_run(move(glyph_run))
    {
    }

    Node const& layout_node() const { return m_layout_node; }
    int start() const { return m_start; }
    int length() const { return m_length; }
    CSSPixelRect const absolute_rect() const;

    CSSPixelPoint offset() const { return m_offset; }
    void set_offset(CSSPixelPoint offset) { m_offset = offset; }

    // The baseline of a fragment is the number of pixels from the top to the text baseline.
    void set_baseline(CSSPixels y) { m_baseline = y; }
    CSSPixels baseline() const { return m_baseline; }

    CSSPixelSize size() const
    {
        return m_size;
    }
    void set_width(CSSPixels width) { m_size.set_width(width); }
    void set_height(CSSPixels height) { m_size.set_height(height); }
    CSSPixels width() const { return m_size.width(); }
    CSSPixels height() const { return m_size.height(); }

    CSSPixels border_box_top() const { return m_border_box_top; }

    bool ends_in_whitespace() const;
    bool is_justifiable_whitespace() const;
    StringView text() const;

    bool is_atomic_inline() const;

    RefPtr<Gfx::GlyphRun> glyph_run() const { return m_glyph_run; }

private:
    JS::NonnullGCPtr<Node const> m_layout_node;
    int m_start { 0 };
    int m_length { 0 };
    CSSPixelPoint m_offset;
    CSSPixelSize m_size;
    CSSPixels m_border_box_top { 0 };
    CSSPixels m_baseline { 0 };
    RefPtr<Gfx::GlyphRun> m_glyph_run;
};

}
