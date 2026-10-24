/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Matrix4x4.h>
#include <LibWeb/Bindings/DOMMatrix.h>
#include <LibWeb/Geometry/DOMMatrixReadOnly.h>

namespace Web::Geometry {

// https://drafts.fxtf.org/geometry/#dommatrix
// The C++ side of a DOMMatrix, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/geometry): what CSS
// Typed OM and SVG create and transform.
class WEB_API DOMMatrix final : public DOMMatrixReadOnly {
    WEB_RUST_INTERFACE(DOMMatrix, dom_matrix)

public:
    // A new identity matrix, which is 2D.
    static GC::Ref<DOMMatrix> create();

    // A new matrix with the elements of the matrix, whose is 2D is the value.
    static GC::Ref<DOMMatrix> create(Gfx::DoubleMatrix4x4 const&, bool is_2d);

    // A new matrix with the elements of the matrix, which is 2D if the matrix is a 2D affine transform.
    static GC::Ref<DOMMatrix> create(Gfx::FloatMatrix4x4 const&);

    // A new matrix initialized to the values of the matrix.
    static GC::Ref<DOMMatrix> create_from_dom_matrix_read_only(DOMMatrixReadOnly const&);

    // Post-multiplies the other matrix to this one, https://drafts.fxtf.org/geometry/#dom-dommatrix-multiplyself
    void multiply_self(DOMMatrixReadOnly const&);

    // https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateaxisangleself
    void rotate_axis_angle_self(double x, double y, double z, double angle);

    // Sets the elements m11, m12, m13, m14, m21, ..., m44, in that order, and whether the matrix is 2D.
    void set_elements(Array<double, 16> const& elements, bool is_2d);
};

}
