/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

namespace Gfx {

enum class CornerClip {
    Outside,
    Inside
};

struct CornerRadius {
    int horizontal_radius { 0 };
    int vertical_radius { 0 };

    inline operator bool() const
    {
        return horizontal_radius > 0 && vertical_radius > 0;
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

    void adjust_corners_for_spread_distance(int spread_distance);

    bool contains(IntPoint point, IntRect const& rect) const
    {
        if (!rect.contains(point))
            return false;

        if (!has_any_radius())
            return true;

        auto const px = point.x();
        auto const py = point.y();

        auto outside_ellipse = [&](CornerRadius const& r, int cx, int cy) {
            auto dx = static_cast<float>(px - cx) / r.horizontal_radius;
            auto dy = static_cast<float>(py - cy) / r.vertical_radius;
            return dx * dx + dy * dy > 1.f;
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
};

}
