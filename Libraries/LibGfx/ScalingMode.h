/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

namespace Gfx {

enum class ScalingMode {
    None,
    Bilinear,
    BilinearMipmap,
    NearestNeighbor,
};

}
