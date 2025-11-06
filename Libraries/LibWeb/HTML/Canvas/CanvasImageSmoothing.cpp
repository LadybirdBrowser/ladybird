/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/Canvas/CanvasImageSmoothing.h>
#include <LibWeb/HTML/Canvas/DrawingState.h>

namespace Web::HTML {

bool CanvasImageSmoothing::image_smoothing_enabled() const
{
    return drawing_state().image_smoothing_enabled;
}

void CanvasImageSmoothing::set_image_smoothing_enabled(bool enabled)
{
    drawing_state().image_smoothing_enabled = enabled;
}

Bindings::ImageSmoothingQuality CanvasImageSmoothing::image_smoothing_quality() const
{
    return drawing_state().image_smoothing_quality;
}

void CanvasImageSmoothing::set_image_smoothing_quality(Bindings::ImageSmoothingQuality quality)
{
    drawing_state().image_smoothing_quality = quality;
}

}
