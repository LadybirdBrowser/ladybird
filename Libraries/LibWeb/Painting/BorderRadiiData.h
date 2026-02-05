/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/Export.h>

namespace Web::Painting {

struct CornerRadius {
    int horizontal_radius { 0 };
    int vertical_radius { 0 };

    inline operator bool() const
    {
        return horizontal_radius > 0 && vertical_radius > 0;
    }
};

struct WEB_API BorderRadiusData {
    CSSPixels horizontal_radius { 0 };
    CSSPixels vertical_radius { 0 };

    CornerRadius as_corner(DevicePixelConverter const& device_pixel_converter) const;

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

struct CornerRadii {
    CornerRadius top_left;
    CornerRadius top_right;
    CornerRadius bottom_right;
    CornerRadius bottom_left;

    inline bool has_any_radius() const
    {
        return top_left || top_right || bottom_right || bottom_left;
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

        auto const px = point.x();
        auto const py = point.y();

        auto outside_ellipse = [&](BorderRadiusData const& r, CSSPixels cx, CSSPixels cy) {
            auto dx = (px - cx).to_float() / r.horizontal_radius.to_float();
            auto dy = (py - cy).to_float() / r.vertical_radius.to_float();
            return dx * dx + dy * dy > 1.0f;
        };

        if (top_left) {
            auto cx = rect.left() + top_left.horizontal_radius;
            auto cy = rect.top() + top_left.vertical_radius;
            if (px < cx && py < cy && outside_ellipse(top_left, cx, cy))
                return false;
        }

        if (top_right) {
            auto cx = rect.right() - top_right.horizontal_radius;
            auto cy = rect.top() + top_right.vertical_radius;
            if (px > cx && py < cy && outside_ellipse(top_right, cx, cy))
                return false;
        }

        if (bottom_right) {
            auto cx = rect.right() - bottom_right.horizontal_radius;
            auto cy = rect.bottom() - bottom_right.vertical_radius;
            if (px > cx && py > cy && outside_ellipse(bottom_right, cx, cy))
                return false;
        }

        if (bottom_left) {
            auto cx = rect.left() + bottom_left.horizontal_radius;
            auto cy = rect.bottom() - bottom_left.vertical_radius;
            if (px < cx && py > cy && outside_ellipse(bottom_left, cx, cy))
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

    inline CornerRadii as_corners(DevicePixelConverter const& device_pixel_converter) const
    {
        if (!has_any_radius())
            return {};
        return CornerRadii {
            top_left.as_corner(device_pixel_converter),
            top_right.as_corner(device_pixel_converter),
            bottom_right.as_corner(device_pixel_converter),
            bottom_left.as_corner(device_pixel_converter)
        };
    }
};

}
