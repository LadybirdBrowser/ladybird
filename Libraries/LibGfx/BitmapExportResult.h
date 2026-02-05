/*
 * Copyright (c) 2025, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>

namespace Gfx {

struct BitmapExportResult {
    ByteBuffer buffer;
    int width { 0 };
    int height { 0 };
};

}
