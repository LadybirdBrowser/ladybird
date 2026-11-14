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

// The compositor paints the overlay with this same geometry.
COMPOSITING_API PausedDebuggerOverlayGeometry paused_debugger_overlay_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio);

}
