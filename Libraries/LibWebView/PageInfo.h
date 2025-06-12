/*
 * Copyright (c) 2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>

namespace WebView {

enum class PageInfoType {
    Text = 1 << 0,
    LayoutTree = 1 << 2,
    PaintTree = 1 << 3,
    GCGraph = 1 << 4,
};

AK_ENUM_BITWISE_OPERATORS(PageInfoType);

}
