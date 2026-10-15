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

template<typename Action>
struct OverlayButton {
    Action action;
    Gfx::IntRect bounds;
};

template<typename Action, size_t button_count>
Optional<Action> overlay_action_at(Array<OverlayButton<Action>, button_count> const& buttons, Gfx::IntPoint position)
{
    for (auto const& button : buttons) {
        if (button.bounds.contains(position))
            return button.action;
    }
    return {};
}

}
