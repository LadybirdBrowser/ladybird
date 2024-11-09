/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Point.h>

namespace Gfx {

class AnimationWriter {
public:
    virtual ~AnimationWriter();

    // Flushes the frame to disk.
    // IntRect { at, at + bitmap.size() } must fit in the dimensions
    // passed to `start_writing_animation()`.
    // FIXME: Consider passing in disposal method and blend mode.
    virtual ErrorOr<void> add_frame(Bitmap&, int duration_ms, IntPoint at = {}) = 0;

    ErrorOr<void> add_frame_relative_to_last_frame(Bitmap&, int duration_ms, RefPtr<Bitmap> last_frame);
};

}
