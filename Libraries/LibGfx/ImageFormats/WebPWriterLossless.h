/*
 * Copyright (c) 2024, Nico Weber <thakis@chromium.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <LibGfx/Forward.h>

namespace Gfx {

struct VP8LEncoderOptions {
    // For each TransformType, set bit `1 << transform_type` if that transform type is allowed.
    unsigned allowed_transforms { 0xf };
};

ErrorOr<ByteBuffer> compress_VP8L_image_data(Bitmap const&, VP8LEncoderOptions const&, bool& is_fully_opaque);

}
