/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/ExportFormat.h>
#include <LibGfx/Forward.h>

namespace Gfx {

[[nodiscard]] StringView export_format_name(ExportFormat);

struct BitmapExportResult {
    ByteBuffer buffer;
    int width { 0 };
    int height { 0 };
};

[[nodiscard]] ErrorOr<BitmapExportResult> export_bitmap_to_byte_buffer(
    Bitmap const&,
    ColorSpace const&,
    ExportFormat,
    int flags,
    Optional<int> target_width,
    Optional<int> target_height);

}
