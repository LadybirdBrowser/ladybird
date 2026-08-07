/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/Optional.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace WebView {

enum class PausedDebuggerOverlayAction : u8 {
    StepOver,
    Continue,
};

inline Optional<PausedDebuggerOverlayAction> paused_debugger_overlay_action_from_underlying(u8 value)
{
    switch (value) {
    case to_underlying(PausedDebuggerOverlayAction::StepOver):
        return PausedDebuggerOverlayAction::StepOver;
    case to_underlying(PausedDebuggerOverlayAction::Continue):
        return PausedDebuggerOverlayAction::Continue;
    default:
        return {};
    }
}

struct PausedDebuggerOverlayGeometry {
    Gfx::IntRect toolbar;
    Gfx::IntRect message;
    Gfx::IntRect step_over_button;
    Gfx::IntRect continue_button;
};

inline PausedDebuggerOverlayGeometry paused_debugger_overlay_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    auto scaled_dimension = [&](int value) {
        return max(1, static_cast<int>(round(value * device_pixel_ratio)));
    };

    auto width = min(viewport_size.width(), scaled_dimension(250));
    auto height = min(viewport_size.height(), scaled_dimension(38));
    auto x = max(0, (viewport_size.width() - width) / 2);
    auto y = min(scaled_dimension(30), max(0, viewport_size.height() - height));
    auto button_width = min(scaled_dimension(40), width / 2);
    auto message_width = width - button_width * 2;

    return {
        .toolbar = { x, y, width, height },
        .message = { x, y, message_width, height },
        .step_over_button = { x + message_width, y, button_width, height },
        .continue_button = { x + message_width + button_width, y, button_width, height },
    };
}

inline Optional<PausedDebuggerOverlayAction> paused_debugger_overlay_action_at(Gfx::IntPoint position, Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    auto geometry = paused_debugger_overlay_geometry(viewport_size, device_pixel_ratio);
    if (geometry.step_over_button.contains(position))
        return PausedDebuggerOverlayAction::StepOver;
    if (geometry.continue_button.contains(position))
        return PausedDebuggerOverlayAction::Continue;
    return {};
}

class PausedDebuggerOverlayPointerState {
public:
    void press(Optional<PausedDebuggerOverlayAction> action) { m_pressed_action = action; }
    void cancel() { m_pressed_action.clear(); }

    Optional<PausedDebuggerOverlayAction> release(Optional<PausedDebuggerOverlayAction> action)
    {
        auto pressed_action = m_pressed_action;
        m_pressed_action.clear();
        if (pressed_action == action)
            return action;
        return {};
    }

    bool is_active() const { return m_pressed_action.has_value(); }

private:
    Optional<PausedDebuggerOverlayAction> m_pressed_action;
};

}
