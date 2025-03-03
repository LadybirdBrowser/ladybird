/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibGfx/Path.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Forward.h>
#include <LibWeb/Painting/BorderRadiiData.h>
#include <LibWeb/Painting/BordersData.h>

namespace Web::Painting {

enum class BorderEdge {
    Top,
    Right,
    Bottom,
    Left,
};

class BorderPainter final {
public:
    explicit BorderPainter(
        DisplayListRecorder& painter,
        DevicePixelRect const& border_rect,
        CornerRadii const& corner_radii,
        BordersDataDevicePixels const& borders_data)
        : m_painter(painter)
        , m_border_rect(border_rect)
        , m_corner_radii(corner_radii)
        , m_borders_data(borders_data)
    {
    }

    void paint_border(BorderEdge, DevicePixelRect const&, CornerRadius const&, CornerRadius const&, bool);
    void paint_simple_border(BorderEdge, DevicePixelRect const&, BorderDataDevicePixels const&, Gfx::LineStyle);
    void paint_joined_border(BorderEdge, DevicePixelRect const&, BorderDataDevicePixels const&, CornerRadius const&, CornerRadius const&, bool);

    BorderDataDevicePixels border_data_for_edge(BorderEdge) const;
    Gfx::Color border_color_for_edge(BorderEdge) const;

private:
    DisplayListRecorder& m_painter;
    DevicePixelRect const m_border_rect;
    CornerRadii const m_corner_radii;
    BordersDataDevicePixels const m_borders_data;
    Gfx::Path m_path;
};

// Returns OptionalNone if there is no outline to paint.
Optional<BordersData> borders_data_for_outline(Layout::Node const&, Color outline_color, CSS::OutlineStyle outline_style, CSSPixels outline_width);

void paint_all_borders(DisplayListRecorder& painter, DevicePixelRect const& border_rect, CornerRadii const& corner_radii, BordersDataDevicePixels const&);

}
