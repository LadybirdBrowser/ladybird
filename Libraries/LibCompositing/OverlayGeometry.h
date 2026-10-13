/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Math.h>
#include <AK/Optional.h>
#include <LibGfx/Point.h>
#include <LibGfx/Rect.h>

namespace Compositing {

inline int scaled_overlay_length(int length, double device_pixel_ratio)
{
    return max(1, static_cast<int>(round(length * device_pixel_ratio)));
}

enum class OverlayButtonShape : u8 {
    Rectangle,
    Circle,
};

template<typename Action>
struct OverlayButton {
    Action action;
    Gfx::IntRect bounds;
    OverlayButtonShape shape { OverlayButtonShape::Rectangle };

    // A round button leaves the corners of its bounds to whatever is below it.
    bool contains(Gfx::IntPoint position) const
    {
        if (shape == OverlayButtonShape::Rectangle)
            return bounds.contains(position);

        auto radius = bounds.width() / 2.0;
        auto distance_x = position.x() + 0.5 - (bounds.x() + radius);
        auto distance_y = position.y() + 0.5 - (bounds.y() + radius);
        return distance_x * distance_x + distance_y * distance_y <= radius * radius;
    }
};

template<typename Action, size_t button_count>
Optional<Action> overlay_action_at(Array<OverlayButton<Action>, button_count> const& buttons, Gfx::IntPoint position)
{
    for (auto const& button : buttons) {
        if (button.contains(position))
            return button.action;
    }
    return {};
}

template<typename Action, size_t button_count>
Gfx::IntRect overlay_buttons_bounds(Array<OverlayButton<Action>, button_count> const& buttons)
{
    Gfx::IntRect bounds;
    for (auto const& button : buttons)
        bounds.unite(button.bounds);
    return bounds;
}

}
