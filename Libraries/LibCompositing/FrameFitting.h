/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/StdLibExtras.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>

namespace Compositing {

// Where a frame shows when it is scaled to fit within the target, keeping its aspect ratio, in the target's middle.
inline Gfx::IntRect rect_fitting_frame(Gfx::IntSize frame_size, Gfx::IntSize target_size)
{
    auto scale = min(static_cast<float>(target_size.width()) / frame_size.width(), static_cast<float>(target_size.height()) / frame_size.height());
    auto fitted_size = frame_size.to_type<float>().scaled(scale).to_rounded<int>();
    fitted_size = { max(1, fitted_size.width()), max(1, fitted_size.height()) };
    return Gfx::IntRect { {}, fitted_size }.centered_within({ {}, target_size });
}

}
