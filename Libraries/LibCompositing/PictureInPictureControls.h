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

enum class PictureInPictureControl : u8 {
    BackToTab,
    Close,
};

inline Optional<PictureInPictureControl> picture_in_picture_control_from_underlying(u8 value)
{
    switch (value) {
    case to_underlying(PictureInPictureControl::BackToTab):
        return PictureInPictureControl::BackToTab;
    case to_underlying(PictureInPictureControl::Close):
        return PictureInPictureControl::Close;
    default:
        return {};
    }
}

struct PictureInPictureControlsGeometry {
    Gfx::IntRect back_to_tab_button;
    Gfx::IntRect close_button;

    Array<OverlayButton<PictureInPictureControl>, 2> buttons() const
    {
        using Button = OverlayButton<PictureInPictureControl>;
        return {
            Button { .action = PictureInPictureControl::BackToTab, .bounds = back_to_tab_button, .shape = OverlayButtonShape::Circle },
            Button { .action = PictureInPictureControl::Close, .bounds = close_button, .shape = OverlayButtonShape::Circle },
        };
    }
};

inline PictureInPictureControlsGeometry picture_in_picture_controls_geometry(Gfx::IntSize viewport_size, double device_pixel_ratio)
{
    // Close is in the top left corner, and the way back to the tab in the top right one.
    auto button_size = scaled_overlay_length(28, device_pixel_ratio);
    auto inset = scaled_overlay_length(10, device_pixel_ratio);

    return {
        .back_to_tab_button = { viewport_size.width() - inset - button_size, inset, button_size, button_size },
        .close_button = { inset, inset, button_size, button_size },
    };
}

}
