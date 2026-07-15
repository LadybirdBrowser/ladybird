/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16View.h>
#include <AK/WeakPtr.h>
#include <LibGfx/TextLayout.h>
#include <LibWeb/Export.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/PaintableTypes.h>
#include <LibWeb/Painting/ShadowData.h>
#include <LibWeb/PixelUnits.h>
#include <LibWeb/TextAffinity.h>

namespace Web::Painting {

class Paintable;
class PaintableWithLines;

class WEB_API PaintableFragment {
public:
    PaintableFragment(PaintableWithLines const&, Layout::LineBoxFragment const&, u32 line_index);

    Layout::Node const& layout_node() const
    {
        VERIFY(m_layout_node);
        return *m_layout_node;
    }
    Layout::NodeWithStyle const& style_source() const;
    bool has_layout_node() const { return !!m_layout_node; }
    // For in-place layout tree updates that replace the referenced node.
    void set_layout_node(Layout::Node const& layout_node) { m_layout_node = layout_node; }
    PaintableWithLines const& paintable_with_lines() const
    {
        VERIFY(m_paintable_with_lines);
        return *m_paintable_with_lines;
    }
    RefPtr<Paintable> containing_block_paintable() const;

    // Interrupting block-level boxes (block-in-inline) are recorded as phantom fragments; most
    // fragment consumers (hit testing, render spans, client rects) must skip them.
    bool is_block_level_box() const;

    size_t start_offset() const { return m_start_offset; }
    size_t length_in_code_units() const { return m_length_in_code_units; }

    size_t dom_start_offset_in_node() const { return m_dom_start_offset_in_node; }
    size_t dom_end_offset_in_node() const { return m_dom_start_offset_in_node + m_length_in_code_units; }

    // Whitespace stripped from the end of this fragment (e.g. the space at a soft wrap point). Caret positions
    // inside it render at the end of this fragment's line, hanging past the fragment's text.
    size_t trailing_whitespace_length_in_code_units() const { return m_trailing_whitespace_length_in_code_units; }
    size_t dom_end_offset_with_trailing_whitespace() const { return dom_end_offset_in_node() + m_trailing_whitespace_length_in_code_units; }

    // How a caret at the given offset relates to this fragment.
    enum class CaretMatch : u8 {
        None,
        Direct,
        // The offset sits at this fragment's whitespace-extended end with Downstream affinity; a later fragment
        // starting exactly at the offset takes precedence.
        SoftWrapFallback,
    };
    CaretMatch caret_match(size_t offset, TextAffinity) const;

    CSSPixels baseline() const { return m_baseline; }
    CSSPixelPoint offset() const { return m_offset; }
    void set_offset(CSSPixelPoint offset) { m_offset = offset; }
    CSSPixelSize size() const { return m_size; }
    u32 line_index() const { return m_line_index; }

    Vector<ShadowData> const& shadows() const { return m_shadows; }
    void set_shadows(Vector<ShadowData>&& shadows) { m_shadows = shadows; }

    CSSPixelRect const absolute_rect() const;
    CSSPixelRect const absolute_line_box_rect() const;

    RefPtr<Gfx::GlyphRun> glyph_run() const { return m_glyph_run; }
    Gfx::Orientation orientation() const;

    CSSPixelRect selection_rect() const;
    CSSPixelRect range_rect(SelectionState selection_state, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const;

    struct SelectionOffsets {
        size_t start;
        size_t end;
    };
    Optional<SelectionOffsets> selection_offsets() const;
    Optional<SelectionOffsets> selection_range_for_text_control() const;
    SelectionState selection_state() const { return m_selection_state; }
    void set_selection_state(SelectionState);

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

private:
    Optional<SelectionOffsets> compute_selection_offsets(SelectionState, size_t start_offset_in_code_units, size_t end_offset_in_code_units) const;
    CSSPixelRect rect_for_selection_offsets(SelectionOffsets const&) const;

    WeakPtr<Layout::Node const> m_layout_node;
    WeakPtr<PaintableWithLines const> m_paintable_with_lines;
    CSSPixelPoint m_offset;
    CSSPixelSize m_size;
    u32 m_line_index { 0 };
    size_t m_start_offset { 0 };
    size_t m_length_in_code_units { 0 };
    size_t m_dom_start_offset_in_node { 0 };
    RefPtr<Gfx::GlyphRun> m_glyph_run;
    Vector<ShadowData> m_shadows;
    CSSPixels m_baseline;
    CSSPixels m_text_decoration_thickness { 0 };
    CSS::WritingMode m_writing_mode;
    SelectionState m_selection_state { SelectionState::None };
    u32 m_trailing_whitespace_length_in_code_units { 0 };
};

}
