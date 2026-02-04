/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class WEB_API PaintableFragment {
    friend class PaintableWithLines;

public:
    explicit PaintableFragment(Layout::LineBoxFragment const&);

    Layout::Node const& layout_node() const { return m_layout_node; }
    Paintable const& paintable() const { return *m_layout_node->first_paintable(); }

    size_t start_offset() const { return m_start_offset; }
    size_t length_in_code_units() const { return m_length_in_code_units; }

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
    CSSPixelRect range_rect(Paintable::SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const;

    struct SelectionOffsets {
        size_t start;
        size_t end;
        bool include_trailing_whitespace { false };
    };
    Optional<SelectionOffsets> selection_offsets() const;
    Optional<SelectionOffsets> selection_range_for_text_control() const;

    struct TextDecorationData {
        Vector<CSS::TextDecorationLine> line;
        CSS::TextDecorationStyle style;
        Color color;
    };

    struct FragmentSpan {
        PaintableFragment const& fragment;
        size_t start_code_unit;
        size_t end_code_unit;
        Color text_color;
        Color background_color;
        Optional<Vector<ShadowData>> shadow_layers;
        Optional<TextDecorationData> text_decoration;
    };

    CSSPixels width() const { return m_size.width(); }
    CSSPixels height() const { return m_size.height(); }

    size_t index_in_node_for_point(CSSPixelPoint) const;

    Utf16View text() const;

    CSSPixels text_decoration_thickness() const { return m_text_decoration_thickness; }
    void set_text_decoration_thickness(CSSPixels thickness) { m_text_decoration_thickness = thickness; }

    bool has_trailing_whitespace() const { return m_has_trailing_whitespace; }

private:
    Optional<SelectionOffsets> compute_selection_offsets(Paintable::SelectionState, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const;

    GC::Ref<Layout::Node const> m_layout_node;
    CSSPixelPoint m_offset;
    CSSPixelSize m_size;
    size_t m_start_offset { 0 };
    size_t m_length_in_code_units { 0 };
    RefPtr<Gfx::GlyphRun> m_glyph_run;
    Vector<ShadowData> m_shadows;
    CSSPixels m_baseline;
    CSSPixels m_text_decoration_thickness { 0 };
    CSS::WritingMode m_writing_mode;
    bool m_has_trailing_whitespace { false };
};

}
