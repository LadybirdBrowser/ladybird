/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Forward.h>

namespace Gfx {

#define ENUMERATE_EXPORT_FORMATS(X) \
    X(Gray8)                        \
    X(Alpha8)                       \
    X(RGB565)                       \
    X(RGBA5551)                     \
    X(RGBA4444)                     \
    X(RGB888)                       \
    X(RGBA8888)

enum class ExportFormat : u8 {
#define ENUMERATE_EXPORT_FORMAT(format) format,
    ENUMERATE_EXPORT_FORMATS(ENUMERATE_EXPORT_FORMAT)
#undef ENUMERATE_EXPORT_FORMAT
};

[[nodiscard]] StringView export_format_name(ExportFormat);

struct ExportFlags {
    enum : u8 {
        PremultiplyAlpha = 1 << 0,
        FlipY = 1 << 1,
    };
};

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
