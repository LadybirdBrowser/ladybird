/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/TextLayout.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class PaintableFragment {
    friend class PaintableWithLines;

public:
    explicit PaintableFragment(Layout::LineBoxFragment const&);

    Layout::Node const& layout_node() const { return m_layout_node; }
    Paintable const& paintable() const { return *m_layout_node->first_paintable(); }

    int start() const { return m_start; }
    int length() const { return m_length; }

    CSSPixels baseline() const { return m_baseline; }
    CSSPixelPoint offset() const { return m_offset; }
    void set_offset(CSSPixelPoint offset) { m_offset = offset; }
    CSSPixelSize size() const { return m_size; }

    Vector<ShadowData> const& shadows() const { return m_shadows; }
    void set_shadows(Vector<ShadowData>&& shadows) { m_shadows = shadows; }

    CSSPixelRect const absolute_rect() const;

    RefPtr<Gfx::GlyphRun> glyph_run() const { return m_glyph_run; }
    Gfx::Orientation orientation() const;

    CSSPixelRect selection_rect() const;
    CSSPixelRect range_rect(size_t start_offset, size_t end_offset) const;

    CSSPixels width() const { return m_size.width(); }
    CSSPixels height() const { return m_size.height(); }

    int text_index_at(CSSPixelPoint) const;

    StringView string_view() const;

private:
    GC::Ref<Layout::Node const> m_layout_node;
    CSSPixelPoint m_offset;
    CSSPixelSize m_size;
    CSSPixels m_baseline;
    int m_start;
    int m_length;
    RefPtr<Gfx::GlyphRun> m_glyph_run;
    CSS::WritingMode m_writing_mode;
    Vector<ShadowData> m_shadows;
};

}
