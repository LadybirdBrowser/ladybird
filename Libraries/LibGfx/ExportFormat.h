/*
 * Copyright (c) 2024-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

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

struct ExportFlags {
    enum : u8 {
        PremultiplyAlpha = 1 << 0,
        FlipY = 1 << 1,
    };
};

}
