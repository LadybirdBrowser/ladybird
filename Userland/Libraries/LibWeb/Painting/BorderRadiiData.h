/*
 * Copyright (c) 2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/AntiAliasingPainter.h>
#include <LibGfx/Forward.h>
#include <LibWeb/CSS/ComputedValues.h>

namespace Web::Painting {

struct BorderRadiusData {
    CSSPixels horizontal_radius { 0 };
    CSSPixels vertical_radius { 0 };

    Gfx::CornerRadius as_corner(PaintContext const& context) const;

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

    inline void union_max_radii(BorderRadiusData const& other)
    {
        horizontal_radius = max(horizontal_radius, other.horizontal_radius);
        vertical_radius = max(vertical_radius, other.vertical_radius);
    }
};

using CornerRadius = Gfx::CornerRadius;

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

    inline void union_max_radii(BorderRadiiData const& other)
    {
        top_left.union_max_radii(other.top_left);
        top_right.union_max_radii(other.top_right);
        bottom_right.union_max_radii(other.bottom_right);
        bottom_left.union_max_radii(other.bottom_left);
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

    inline CornerRadii as_corners(PaintContext const& context) const
    {
        return CornerRadii {
            top_left.as_corner(context),
            top_right.as_corner(context),
            bottom_right.as_corner(context),
            bottom_left.as_corner(context)
        };
    }
};

}
