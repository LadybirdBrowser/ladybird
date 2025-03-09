/*
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <core/SkMatrix.h>

namespace Gfx {

class AffineTransform;

SkMatrix to_skia_matrix(AffineTransform const& affine_transform);

}
