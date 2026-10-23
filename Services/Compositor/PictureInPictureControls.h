/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <Compositor/Forward.h>
#include <LibCompositing/Forward.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>

namespace Compositor {

void paint_picture_in_picture_controls(Gfx::PaintingSurface&, Gfx::IntSize viewport_size, double device_pixel_ratio, Compositing::PictureInPictureControlsState const&, float opacity);

}
