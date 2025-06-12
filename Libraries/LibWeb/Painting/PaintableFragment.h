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

    size_t start() const { return m_start; }
    size_t length() const { return m_length; }

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
    CSSPixelRect range_rect(size_t start_offset_in_code_units, size_t end_offset_in_code_units) const;

    CSSPixels width() const { return m_size.width(); }
    CSSPixels height() const { return m_size.height(); }

    size_t index_in_node_for_byte_offset(size_t) const;
    size_t index_in_node_for_point(CSSPixelPoint) const;

    Utf8View utf8_view() const;
    Utf16View utf16_view() const;

    CSSPixels text_decoration_thickness() const { return m_text_decoration_thickness; }
    void set_text_decoration_thickness(CSSPixels thickness) { m_text_decoration_thickness = thickness; }

private:
    GC::Ref<Layout::Node const> m_layout_node;
    CSSPixelPoint m_offset;
    CSSPixelSize m_size;
    CSSPixels m_baseline;
    size_t m_start;
    size_t m_length;
    RefPtr<Gfx::GlyphRun> m_glyph_run;
    CSS::WritingMode m_writing_mode;
    Vector<ShadowData> m_shadows;
    CSSPixels m_text_decoration_thickness { 0 };
    mutable Optional<AK::Utf16ConversionResult> m_text_in_utf16;
};

}
