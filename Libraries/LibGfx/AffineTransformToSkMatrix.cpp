/*
 * Copyright (c) 2025, Tuur Martens <tuurmartens4@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AffineTransformToSkMatrix.h"

#include "AffineTransform.h"

SkMatrix Gfx::to_skia_matrix(Gfx::AffineTransform const& affine_transform)
{
    SkScalar affine[6];
    affine[0] = affine_transform.a();
    affine[1] = affine_transform.b();
    affine[2] = affine_transform.c();
    affine[3] = affine_transform.d();
    affine[4] = affine_transform.e();
    affine[5] = affine_transform.f();

    SkMatrix matrix;
    matrix.setAffine(affine);
    return matrix;
}
