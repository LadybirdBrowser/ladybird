/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/Forward.h>
#include <LibGfx/BitmapExportResult.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>

namespace Gfx {

struct ExportFlags {
    enum : u8 {
        PremultiplyAlpha = 1 << 0,
        FlipY = 1 << 1,
    };
};

[[nodiscard]] ErrorOr<BitmapExportResult> export_bitmap_to_byte_buffer(
    Bitmap const&,
    ColorSpace const&,
    BitmapFormat,
    int flags,
    Optional<int> target_width,
    Optional<int> target_height);

}
