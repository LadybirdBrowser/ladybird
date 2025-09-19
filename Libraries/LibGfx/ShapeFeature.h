/*
 * Copyright (c) 2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Gfx {

struct ShapeFeature {
    char tag[4];
    u32 value;

    bool operator==(ShapeFeature const&) const = default;
    bool operator!=(ShapeFeature const&) const = default;
};

using ShapeFeatures = Vector<ShapeFeature, 4>;

}
