/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/TypeCasts.h>
#include <AK/kmalloc.h>

namespace Gfx {

// What a rasterizer keeps with a typeface or a font, for as long as the typeface or the font lives.
class RasterizerData {
    AK_ALLOC_WITH_KMALLOC;

public:
    virtual ~RasterizerData() = default;
};

}
