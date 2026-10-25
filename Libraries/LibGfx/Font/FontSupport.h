/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Gfx {

enum class FontFormat : u8 {
    EmbeddedOpenType,
    OpenType,
    SVG,
    TrueType,
    TrueTypeCollection,
    WOFF,
    WOFF2,
};

bool font_format_is_supported(FontFormat);

}
