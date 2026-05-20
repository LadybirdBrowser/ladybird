/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/CornerRadii.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Export.h>

namespace Web::Painting {

struct WEB_API BorderRadiusData {
    CSSPixels horizontal_radius { 0 };
    CSSPixels vertical_radius { 0 };

    Gfx::CornerRadius as_corner(DevicePixelConverter const& device_pixel_converter) const;

    inline operator bool() const
    {
        return horizontal_radius > 0 && vertical_radius > 0;
    }

    inline void shrink(CSSPixels horizontal, CSSPixels vertical)
    {
        if (horizontal_radius != 0)
            horizontal_radius = max(CSSPixels(0), horizontal_radius - horizontal);
        if (vertical_radius != 0)
            vertical_radius = max(CSSPixels(0), vertical_radius - vertical);
    }
};

struct BorderRadiiData {
    BorderRadiusData top_left;
    BorderRadiusData top_right;
    BorderRadiusData bottom_right;
    BorderRadiusData bottom_left;

    inline bool has_any_radius() const
    {
        return top_left || top_right || bottom_right || bottom_left;
    }

    bool contains(CSSPixelPoint point, CSSPixelRect const& rect) const
    {
        if (!rect.contains(point))
            return false;

        if (!has_any_radius())
            return true;

        auto to_corner = [](BorderRadiusData const& r) -> Gfx::CornerRadius {
            return { static_cast<int>(r.horizontal_radius.to_float()), static_cast<int>(r.vertical_radius.to_float()) };
        };
        Gfx::CornerRadii corners { to_corner(top_left), to_corner(top_right), to_corner(bottom_right), to_corner(bottom_left) };
        return corners.contains(point.to_type<int>(), rect.to_type<int>());
    }

    inline void shrink(CSSPixels top, CSSPixels right, CSSPixels bottom, CSSPixels left)
    {
        top_left.shrink(left, top);
        top_right.shrink(right, top);
        bottom_right.shrink(right, bottom);
        bottom_left.shrink(left, bottom);
    }

    inline void inflate(CSSPixels top, CSSPixels right, CSSPixels bottom, CSSPixels left)
    {
        shrink(-top, -right, -bottom, -left);
    }

    inline Gfx::CornerRadii as_corners(DevicePixelConverter const& device_pixel_converter) const
    {
        if (!has_any_radius())
            return {};
        return Gfx::CornerRadii {
            top_left.as_corner(device_pixel_converter),
            top_right.as_corner(device_pixel_converter),
            bottom_right.as_corner(device_pixel_converter),
            bottom_left.as_corner(device_pixel_converter)
        };
    }
};

}
