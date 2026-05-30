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

        auto outside_ellipse = [&](BorderRadiusData const& radius, CSSPixels center_x, CSSPixels center_y) {
            auto dx = (point.x() - center_x).to_double() / radius.horizontal_radius.to_double();
            auto dy = (point.y() - center_y).to_double() / radius.vertical_radius.to_double();
            return dx * dx + dy * dy > 1.0;
        };

        if (top_left) {
            auto center_x = rect.left() + top_left.horizontal_radius;
            auto center_y = rect.top() + top_left.vertical_radius;
            if (point.x() < center_x && point.y() < center_y && outside_ellipse(top_left, center_x, center_y))
                return false;
        }

        if (top_right) {
            auto center_x = rect.right() - top_right.horizontal_radius;
            auto center_y = rect.top() + top_right.vertical_radius;
            if (point.x() > center_x && point.y() < center_y && outside_ellipse(top_right, center_x, center_y))
                return false;
        }

        if (bottom_right) {
            auto center_x = rect.right() - bottom_right.horizontal_radius;
            auto center_y = rect.bottom() - bottom_right.vertical_radius;
            if (point.x() > center_x && point.y() > center_y && outside_ellipse(bottom_right, center_x, center_y))
                return false;
        }

        if (bottom_left) {
            auto center_x = rect.left() + bottom_left.horizontal_radius;
            auto center_y = rect.bottom() - bottom_left.vertical_radius;
            if (point.x() < center_x && point.y() > center_y && outside_ellipse(bottom_left, center_x, center_y))
                return false;
        }

        return true;
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
