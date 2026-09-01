/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Painting/BoxModelMetrics.h>

namespace Web::Painting {

PixelBox BoxModelMetrics::border_box() const
{
    return {
        border.top + padding.top,
        border.right + padding.right,
        border.bottom + padding.bottom,
        border.left + padding.left,
    };
}

}
