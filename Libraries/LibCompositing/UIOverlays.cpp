/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCompositing/RustFFI.h>
#include <LibCompositing/UIOverlays.h>

namespace Compositing {

int scaled_overlay_length(int length, double device_pixel_ratio)
{
    return RustFFI::compositing_scaled_overlay_length(length, device_pixel_ratio);
}

PictureInPictureControlsGeometry picture_in_picture_controls_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    RustFFI::PictureInPictureControlsGeometry geometry;
    RustFFI::compositing_picture_in_picture_controls_geometry(viewport_size, device_pixel_ratio, &geometry);
    return { .back_to_tab_button = geometry.back_to_tab_button, .close_button = geometry.close_button };
}

PausedDebuggerOverlayGeometry paused_debugger_overlay_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    RustFFI::PausedDebuggerOverlayGeometry geometry;
    RustFFI::compositing_paused_debugger_overlay_geometry(viewport_size, device_pixel_ratio, &geometry);
    return {
        .toolbar = geometry.toolbar,
        .message = geometry.message,
        .step_over_button = geometry.step_over_button,
        .continue_button = geometry.continue_button,
    };
}

}
