/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/GenericShorthands.h>
#include <LibGfx/Color.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Export.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Painting {

enum class BorderEdge : u8 {
    Top,
    Right,
    Bottom,
    Left,
};

struct BorderDataDevicePixels {
    Color color { Color::Transparent };
    CSS::LineStyle line_style { CSS::LineStyle::None };
    DevicePixels width { 0 };

    bool operator==(BorderDataDevicePixels const&) const = default;
};

struct BordersDataDevicePixels {
    BorderDataDevicePixels top;
    BorderDataDevicePixels right;
    BorderDataDevicePixels bottom;
    BorderDataDevicePixels left;

    BorderDataDevicePixels& for_edge(BorderEdge edge)
    {
        switch (edge) {
        case BorderEdge::Top:
            return top;
        case BorderEdge::Right:
            return right;
        case BorderEdge::Bottom:
            return bottom;
        case BorderEdge::Left:
            return left;
        }
        ASSERT_NOT_REACHED();
    }

    BorderDataDevicePixels const& for_edge(BorderEdge edge) const
    {
        return const_cast<BordersDataDevicePixels&>(*this).for_edge(edge);
    }

    bool all_are_equal() const { return first_is_equal_to_all_of(top, right, bottom, left); }
};

struct WEB_API BordersData {
    CSS::BorderData top;
    CSS::BorderData right;
    CSS::BorderData bottom;
    CSS::BorderData left;

    BordersDataDevicePixels to_device_pixels(DisplayListRecordingContext const& context) const;
};

}
