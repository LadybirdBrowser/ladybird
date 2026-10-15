/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <LibCompositing/OverlayGeometry.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace Compositing {

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

    Array<OverlayButton<PausedDebuggerOverlayAction>, 2> buttons() const
    {
        using Button = OverlayButton<PausedDebuggerOverlayAction>;
        return {
            Button { .action = PausedDebuggerOverlayAction::StepOver, .bounds = step_over_button },
            Button { .action = PausedDebuggerOverlayAction::Continue, .bounds = continue_button },
        };
    }
};

inline PausedDebuggerOverlayGeometry paused_debugger_overlay_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    auto width = min(viewport_size.width(), scaled_overlay_length(250, device_pixel_ratio));
    auto height = min(viewport_size.height(), scaled_overlay_length(38, device_pixel_ratio));
    auto x = max(0, (viewport_size.width() - width) / 2);
    auto y = min(scaled_overlay_length(30, device_pixel_ratio), max(0, viewport_size.height() - height));
    auto button_width = min(scaled_overlay_length(40, device_pixel_ratio), width / 2);
    auto message_width = width - button_width * 2;

    return {
        .toolbar = { x, y, width, height },
        .message = { x, y, message_width, height },
        .step_over_button = { x + message_width, y, button_width, height },
        .continue_button = { x + message_width + button_width, y, button_width, height },
    };
}

}
