/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/HostClassBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/Geometry/DOMMatrix.h>

namespace Web::Geometry {

extern "C" {

// Defined in Bindings/Rust/src/geometry/exports.rs.
void* web_dom_matrix_create(JSVM*, Array<double, 16> const* elements, bool is_2d);
void* web_dom_matrix_create_from_read_only(JSVM*, void const* matrix);
void web_dom_matrix_multiply_self(void* matrix, void const* other);
void web_dom_matrix_rotate_axis_angle_self(void* matrix, double x, double y, double z, double angle);
void web_dom_matrix_set_elements(void* matrix, Array<double, 16> const* elements, bool is_2d);
}

GC::Ref<DOMMatrix> DOMMatrix::create()
{
    return create(Gfx::DoubleMatrix4x4::identity(), true);
}

GC::Ref<DOMMatrix> DOMMatrix::create(Gfx::DoubleMatrix4x4 const& matrix, bool is_2d)
{
    // NB: Gfx::Matrix4x4 is row by row, so the element mRC of the matrix is in row C and column R.
    Array<double, 16> elements;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column)
            elements[(column * 4) + row] = matrix[row, column];
    }
    return GC::Ref { *static_cast<DOMMatrix*>(web_dom_matrix_create(JS::HostABI::vm_to_abi(JS::VM::the()), &elements, is_2d)) };
}

GC::Ref<DOMMatrix> DOMMatrix::create(Gfx::FloatMatrix4x4 const& matrix)
{
    Gfx::DoubleMatrix4x4 double_matrix;
    for (size_t row = 0; row < 4; ++row) {
        for (size_t column = 0; column < 4; ++column)
            double_matrix[row, column] = matrix[row, column];
    }
    return create(double_matrix, Gfx::is_2d_affine_transform(matrix));
}

GC::Ref<DOMMatrix> DOMMatrix::create_from_dom_matrix_read_only(DOMMatrixReadOnly const& matrix)
{
    return GC::Ref { *static_cast<DOMMatrix*>(web_dom_matrix_create_from_read_only(JS::HostABI::vm_to_abi(JS::VM::the()), &matrix)) };
}

void DOMMatrix::multiply_self(DOMMatrixReadOnly const& other)
{
    web_dom_matrix_multiply_self(this, &other);
}

void DOMMatrix::rotate_axis_angle_self(double x, double y, double z, double angle)
{
    web_dom_matrix_rotate_axis_angle_self(this, x, y, z, angle);
}

void DOMMatrix::set_elements(Array<double, 16> const& elements, bool is_2d)
{
    web_dom_matrix_set_elements(this, &elements, is_2d);
}

}
