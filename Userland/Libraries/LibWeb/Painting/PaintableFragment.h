/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Layout/LineBoxFragment.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BackgroundPainting.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

class PaintableFragment {
    friend class InlinePaintable;
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

    BorderRadiiData const& border_radii_data() const { return m_border_radii_data; }
    void set_border_radii_data(BorderRadiiData const& border_radii_data) { m_border_radii_data = border_radii_data; }

    Vector<ShadowData> const& shadows() const { return m_shadows; }
    void set_shadows(Vector<ShadowData>&& shadows) { m_shadows = shadows; }

    ResolvedBackground const& resolved_background() const { return m_resolved_background; }
    void set_resolved_background(ResolvedBackground resolved_background) { m_resolved_background = resolved_background; }

    CSSPixelRect const absolute_rect() const;

    RefPtr<Gfx::GlyphRun> glyph_run() const { return m_glyph_run; }

    CSSPixelRect selection_rect(Gfx::Font const&) const;
    CSSPixelRect range_rect(Gfx::Font const&, DOM::Range const&) const;

    CSSPixels width() const { return m_size.width(); }
    CSSPixels height() const { return m_size.height(); }

    int text_index_at(CSSPixels) const;

    StringView string_view() const;

private:
    JS::NonnullGCPtr<Layout::Node const> m_layout_node;
    CSSPixelPoint m_offset;
    CSSPixelSize m_size;
    CSSPixels m_baseline;
    int m_start;
    int m_length;
    Painting::BorderRadiiData m_border_radii_data;
    RefPtr<Gfx::GlyphRun> m_glyph_run;
    Vector<ShadowData> m_shadows;
    ResolvedBackground m_resolved_background;
};

}
