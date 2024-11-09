/*
 * Copyright (c) 2023, Luke Wilde <lukew@serenityos.org>
 * Copyright (c) 2023, Bastiaan van der Plaat <bastiaan.v.d.plaat@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/Bindings/DOMMatrixPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Geometry {

JS_DEFINE_ALLOCATOR(DOMMatrix);

// https://drafts.fxtf.org/geometry/#dom-dommatrix-dommatrix
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::construct_impl(JS::Realm& realm, Optional<Variant<String, Vector<double>>> const& init)
{
    auto& vm = realm.vm();

    // -> If init is omitted
    if (!init.has_value()) {
        // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with the sequence [1, 0, 0, 1, 0, 0].
        return realm.heap().allocate<DOMMatrix>(realm, realm, 1, 0, 0, 1, 0, 0);
    }

    auto const& init_value = init.value();

    // -> If init is a DOMString
    if (init_value.has<String>()) {
        // 1. If current global object is not a Window object, then throw a TypeError exception.
        if (!is<HTML::Window>(realm.global_object()))
            return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "This can only be used in a Window context"_string };

        // 2. Parse init into an abstract matrix, and let matrix and 2dTransform be the result. If the result is failure, then throw a "SyntaxError" DOMException.
        auto result = TRY(parse_dom_matrix_init_string(realm, init_value.get<String>()));
        auto* elements = result.matrix.elements();

        // If 2dTransform is true
        if (result.is_2d_transform) {
            // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers, the values being the elements m11, m12, m21, m22, m41 and m42 of matrix.
            return realm.heap().allocate<DOMMatrix>(realm, realm, elements[0][0], elements[1][0], elements[0][1], elements[1][1], elements[0][3], elements[1][3]);
        }

        // Otherwise, return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers, the values being the 16 elements of matrix.
        return realm.heap().allocate<DOMMatrix>(realm, realm,
            elements[0][0], elements[1][0], elements[2][0], elements[3][0],
            elements[0][1], elements[1][1], elements[2][1], elements[3][1],
            elements[0][2], elements[1][2], elements[2][2], elements[3][2],
            elements[0][3], elements[1][3], elements[2][3], elements[3][3]);
    }

    auto const& double_sequence = init_value.get<Vector<double>>();

    // -> If init is a sequence with 6 elements
    if (double_sequence.size() == 6) {
        // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with the sequence init.
        return realm.heap().allocate<DOMMatrix>(realm, realm, double_sequence[0], double_sequence[1], double_sequence[2], double_sequence[3], double_sequence[4], double_sequence[5]);
    }

    // -> If init is a sequence with 16 elements
    if (double_sequence.size() == 16) {
        // Return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with the sequence init.
        return realm.heap().allocate<DOMMatrix>(realm, realm,
            double_sequence[0], double_sequence[1], double_sequence[2], double_sequence[3],
            double_sequence[4], double_sequence[5], double_sequence[6], double_sequence[7],
            double_sequence[8], double_sequence[9], double_sequence[10], double_sequence[11],
            double_sequence[12], double_sequence[13], double_sequence[14], double_sequence[15]);
    }

    // -> Otherwise, throw a TypeError exception.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, TRY_OR_THROW_OOM(vm, String::formatted("Sequence must contain exactly 6 or 16 elements, got {} element(s)", double_sequence.size())) };
}

// https://drafts.fxtf.org/geometry/#create-a-dommatrix-from-the-2d-dictionary
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::create_from_dom_matrix_2d_init(JS::Realm& realm, DOMMatrix2DInit& init)
{
    // 1. Validate and fixup (2D) other.
    TRY(validate_and_fixup_dom_matrix_2d_init(init));

    // These should all have values after calling `validate_and_fixup_dom_matrix_2d_init`
    VERIFY(init.m11.has_value());
    VERIFY(init.m12.has_value());
    VERIFY(init.m21.has_value());
    VERIFY(init.m22.has_value());
    VERIFY(init.m41.has_value());
    VERIFY(init.m42.has_value());

    // 2. Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers,
    //    the values being the 6 elements m11, m12, m21, m22, m41 and m42 of other in the given order.
    return realm.heap().allocate<DOMMatrix>(realm, realm, init.m11.value(), init.m12.value(), init.m21.value(), init.m22.value(), init.m41.value(), init.m42.value());
}

// https://drafts.fxtf.org/geometry/#create-a-dommatrix-from-the-dictionary
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::create_from_dom_matrix_init(JS::Realm& realm, DOMMatrixInit& init)
{
    // 1. Validate and fixup other.
    TRY(validate_and_fixup_dom_matrix_init(init));

    // 2. If the is2D dictionary member of other is true.
    if (init.is2d.has_value() && init.is2d.value()) {
        // Return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers, the values being the 6 elements m11, m12, m21, m22, m41 and m42 of other in the given order.
        return realm.heap().allocate<DOMMatrix>(realm, realm, init.m11.value(), init.m12.value(), init.m21.value(), init.m22.value(), init.m41.value(), init.m42.value());
    }

    // Otherwise, Return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers, the values being the 16 elements m11, m12, m13, ..., m44 of other in the given order.
    return realm.heap().allocate<DOMMatrix>(realm, realm, init.m11.value(), init.m12.value(), init.m13, init.m14,
        init.m21.value(), init.m22.value(), init.m23, init.m24,
        init.m31, init.m32, init.m33, init.m34,
        init.m41.value(), init.m42.value(), init.m43, init.m44);
}

JS::NonnullGCPtr<DOMMatrix> DOMMatrix::create_from_dom_matrix_read_only(JS::Realm& realm, DOMMatrixReadOnly const& read_only_matrix)
{
    return realm.heap().allocate<DOMMatrix>(realm, realm, read_only_matrix);
}

JS::NonnullGCPtr<DOMMatrix> DOMMatrix::create(JS::Realm& realm)
{
    return realm.heap().allocate<DOMMatrix>(realm, realm);
}

DOMMatrix::DOMMatrix(JS::Realm& realm, double m11, double m12, double m21, double m22, double m41, double m42)
    : DOMMatrixReadOnly(realm, m11, m12, m21, m22, m41, m42)
{
}

DOMMatrix::DOMMatrix(JS::Realm& realm, double m11, double m12, double m13, double m14, double m21, double m22, double m23, double m24, double m31, double m32, double m33, double m34, double m41, double m42, double m43, double m44)
    : DOMMatrixReadOnly(realm, m11, m12, m13, m14, m21, m22, m23, m24, m31, m32, m33, m34, m41, m42, m43, m44)
{
}

DOMMatrix::DOMMatrix(JS::Realm& realm, DOMMatrixReadOnly const& read_only_matrix)
    : DOMMatrixReadOnly(realm, read_only_matrix)
{
}

DOMMatrix::DOMMatrix(JS::Realm& realm)
    : DOMMatrixReadOnly(realm)
{
}

DOMMatrix::~DOMMatrix() = default;

void DOMMatrix::initialize(JS::Realm& realm)
{
    Base::initialize(realm);
    WEB_SET_PROTOTYPE_FOR_INTERFACE(DOMMatrix);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-frommatrix
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::from_matrix(JS::VM& vm, DOMMatrixInit other)
{
    return create_from_dom_matrix_init(*vm.current_realm(), other);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-fromfloat32array
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::from_float32_array(JS::VM& vm, JS::Handle<WebIDL::BufferSource> const& array32)
{
    if (!is<JS::Float32Array>(*array32->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float32Array");

    auto& realm = *vm.current_realm();
    auto& float32_array = static_cast<JS::Float32Array&>(*array32->raw_object());
    ReadonlySpan<float> elements = float32_array.data();

    // If array32 has 6 elements, return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers taking the values from array32 in the provided order.
    if (elements.size() == 6)
        return realm.heap().allocate<DOMMatrix>(realm, realm, elements.at(0), elements.at(1), elements.at(2), elements.at(3), elements.at(4), elements.at(5));

    // If array32 has 16 elements, return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers taking the values from array32 in the provided order.
    if (elements.size() == 16)
        return realm.heap().allocate<DOMMatrix>(realm, realm, elements.at(0), elements.at(1), elements.at(2), elements.at(3),
            elements.at(4), elements.at(5), elements.at(6), elements.at(7),
            elements.at(8), elements.at(9), elements.at(10), elements.at(11),
            elements.at(12), elements.at(13), elements.at(14), elements.at(15));

    // Otherwise, throw a TypeError exception.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Expected a Float32Array argument with 6 or 16 elements"_string };
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-fromfloat64array
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::from_float64_array(JS::VM& vm, JS::Handle<WebIDL::BufferSource> const& array64)
{
    if (!is<JS::Float64Array>(*array64->raw_object()))
        return vm.throw_completion<JS::TypeError>(JS::ErrorType::NotAnObjectOfType, "Float64Array");

    auto& realm = *vm.current_realm();
    auto& float64_array = static_cast<JS::Float64Array&>(*array64->raw_object());
    ReadonlySpan<double> elements = float64_array.data();

    // If array64 has 6 elements, return the result of invoking create a 2d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers taking the values from array64 in the provided order.
    if (elements.size() == 6)
        return realm.heap().allocate<DOMMatrix>(realm, realm, elements.at(0), elements.at(1), elements.at(2), elements.at(3), elements.at(4), elements.at(5));

    // If array64 has 16 elements, return the result of invoking create a 3d matrix of type DOMMatrixReadOnly or DOMMatrix as appropriate, with a sequence of numbers taking the values from array64 in the provided order.
    if (elements.size() == 16)
        return realm.heap().allocate<DOMMatrix>(realm, realm, elements.at(0), elements.at(1), elements.at(2), elements.at(3),
            elements.at(4), elements.at(5), elements.at(6), elements.at(7),
            elements.at(8), elements.at(9), elements.at(10), elements.at(11),
            elements.at(12), elements.at(13), elements.at(14), elements.at(15));

    // Otherwise, throw a TypeError exception.
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Expected a Float64Array argument with 6 or 16 elements"_string };
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m11
void DOMMatrix::set_m11(double value)
{
    // For the DOMMatrix interface, setting the m11 or the a attribute must set the m11 element to the new value.
    m_matrix.elements()[0][0] = value;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m12
void DOMMatrix::set_m12(double value)
{
    // For the DOMMatrix interface, setting the m12 or the b attribute must set the m12 element to the new value.
    m_matrix.elements()[1][0] = value;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m13
void DOMMatrix::set_m13(double value)
{
    // For the DOMMatrix interface, setting the m13 attribute must set the m13 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[2][0] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m14
void DOMMatrix::set_m14(double value)
{
    // For the DOMMatrix interface, setting the m14 attribute must set the m14 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[3][0] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m21
void DOMMatrix::set_m21(double value)
{
    // For the DOMMatrix interface, setting the m21 or the c attribute must set the m21 element to the new value.
    m_matrix.elements()[0][1] = value;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m22
void DOMMatrix::set_m22(double value)
{
    // For the DOMMatrix interface, setting the m22 or the d attribute must set the m22 element to the new value.
    m_matrix.elements()[1][1] = value;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m23
void DOMMatrix::set_m23(double value)
{
    // For the DOMMatrix interface, setting the m23 attribute must set the m23 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[2][1] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m24
void DOMMatrix::set_m24(double value)
{
    // For the DOMMatrix interface, setting the m24 attribute must set the m24 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[3][1] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m31
void DOMMatrix::set_m31(double value)
{
    // For the DOMMatrix interface, setting the m31 attribute must set the m31 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[0][2] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m32
void DOMMatrix::set_m32(double value)
{
    // For the DOMMatrix interface, setting the m32 attribute must set the m32 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[1][2] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m33
void DOMMatrix::set_m33(double value)
{
    // For the DOMMatrix interface, setting the m33 attribute must set the m33 element to the new value and, if the new value is not 1, set is 2D to false.
    m_matrix.elements()[2][2] = value;
    if (value != 1.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m34
void DOMMatrix::set_m34(double value)
{
    // For the DOMMatrix interface, setting the m34 attribute must set the m34 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[3][2] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m41
void DOMMatrix::set_m41(double value)
{
    // For the DOMMatrix interface, setting the m41 or the e attribute must set the m41 element to the new value.
    m_matrix.elements()[0][3] = value;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m42
void DOMMatrix::set_m42(double value)
{
    // For the DOMMatrix interface, setting the m42 or the f attribute must set the m42 element to the new value.
    m_matrix.elements()[1][3] = value;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m43
void DOMMatrix::set_m43(double value)
{
    // For the DOMMatrix interface, setting the m43 attribute must set the m43 element to the new value and, if the new value is not 0 or -0, set is 2D to false.
    m_matrix.elements()[2][3] = value;
    if (value != 0.0 && value != -0.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-m44
void DOMMatrix::set_m44(double value)
{
    // For the DOMMatrix interface, setting the m44 attribute must set the m44 element to the new value and, if the new value is not 1, set is 2D to false.
    m_matrix.elements()[3][3] = value;
    if (value != 1.0)
        m_is_2d = false;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-a
void DOMMatrix::set_a(double value)
{
    // For the DOMMatrix interface, setting the m11 or the a attribute must set the m11 element to the new value.
    set_m11(value);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-b
void DOMMatrix::set_b(double value)
{
    // For the DOMMatrix interface, setting the m12 or the b attribute must set the m12 element to the new value.
    set_m12(value);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-c
void DOMMatrix::set_c(double value)
{
    // For the DOMMatrix interface, setting the m21 or the c attribute must set the m21 element to the new value.
    set_m21(value);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-d
void DOMMatrix::set_d(double value)
{
    // For the DOMMatrix interface, setting the m22 or the d attribute must set the m22 element to the new value.
    set_m22(value);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-e
void DOMMatrix::set_e(double value)
{
    // For the DOMMatrix interface, setting the m41 or the e attribute must set the m41 element to the new value.
    set_m41(value);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrixreadonly-f
void DOMMatrix::set_f(double value)
{
    // For the DOMMatrix interface, setting the m42 or the f attribute must set the m42 element to the new value.
    set_m42(value);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-multiplyself
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::multiply_self(DOMMatrixInit other)
{
    // 1. Let otherObject be the result of invoking create a DOMMatrix from the dictionary other.
    auto other_object = TRY(DOMMatrix::create_from_dom_matrix_init(realm(), other));

    // 2. The otherObject matrix gets post-multiplied to the current matrix.
    m_matrix = m_matrix * other_object->m_matrix;

    // 3. If is 2D of otherObject is false, set is 2D of the current matrix to false.
    if (!other_object->m_is_2d)
        m_is_2d = false;

    // 4. Return the current matrix.
    return JS::NonnullGCPtr<DOMMatrix>(*this);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-premultiplyself
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::pre_multiply_self(DOMMatrixInit other)
{
    // 1. Let otherObject be the result of invoking create a DOMMatrix from the dictionary other.
    auto other_object = TRY(DOMMatrix::create_from_dom_matrix_init(realm(), other));

    // 2. The otherObject matrix gets pre-multiplied to the current matrix.
    m_matrix = other_object->m_matrix * m_matrix;

    // 3. If is 2D of otherObject is false, set is 2D of the current matrix to false.
    if (!other_object->m_is_2d)
        m_is_2d = false;

    // 4. Return the current matrix.
    return JS::NonnullGCPtr<DOMMatrix>(*this);
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-translateself
JS::NonnullGCPtr<DOMMatrix> DOMMatrix::translate_self(Optional<double> tx, Optional<double> ty, Optional<double> tz)
{
    // 1. Post-multiply a translation transformation on the current matrix. The 3D translation matrix is described in CSS Transforms.
    m_matrix = m_matrix * Gfx::translation_matrix(Vector3<double> { tx.value_or(0), ty.value_or(0), tz.value_or(0) });

    // 2. If tz is specified and not 0 or -0, set is 2D of the current matrix to false.
    if (tz.has_value() && (tz != 0 || tz != -0))
        m_is_2d = false;

    // 3. Return the current matrix.
    return *this;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-scaleself
JS::NonnullGCPtr<DOMMatrix> DOMMatrix::scale_self(Optional<double> scale_x, Optional<double> scale_y, Optional<double> scale_z, Optional<double> origin_x, Optional<double> origin_y, Optional<double> origin_z)
{
    // 1. Perform a translateSelf() transformation on the current matrix with the arguments originX, originY, originZ.
    translate_self(origin_x, origin_y, origin_z);

    // 2. If scaleY is missing, set scaleY to the value of scaleX.
    if (!scale_y.has_value())
        scale_y = scale_x.value_or(1);

    // 3. Post-multiply a non-uniform scale transformation on the current matrix. The 3D scale matrix is described in CSS Transforms with sx = scaleX, sy = scaleY and sz = scaleZ. [CSS3-TRANSFORMS]
    m_matrix = m_matrix * Gfx::scale_matrix(Vector3<double> { scale_x.value_or(1), scale_y.value(), scale_z.value_or(1) });

    // 4. Negate originX, originY and originZ.
    // 5. Perform a translateSelf() transformation on the current matrix with the arguments originX, originY, originZ.
    translate_self(-origin_x.value_or(0), -origin_y.value_or(0), -origin_z.value_or(0));

    // 6. If scaleZ is not 1, set is 2D of the current matrix to false.
    if (scale_z != 1)
        m_is_2d = false;

    // 7. Return the current matrix.
    return *this;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-scale3dself
JS::NonnullGCPtr<DOMMatrix> DOMMatrix::scale3d_self(Optional<double> scale, Optional<double> origin_x, Optional<double> origin_y, Optional<double> origin_z)
{
    // 1. Apply a translateSelf() transformation to the current matrix with the arguments originX, originY, originZ.
    translate_self(origin_x, origin_y, origin_z);

    // 2. Post-multiply a uniform 3D scale transformation (m11 = m22 = m33 = scale) on the current matrix. The 3D scale matrix is described in CSS Transforms with sx = sy = sz = scale. [CSS3-TRANSFORMS]
    m_matrix = m_matrix * Gfx::scale_matrix(Vector3<double> { scale.value_or(1), scale.value_or(1), scale.value_or(1) });

    // 3. Apply a translateSelf() transformation to the current matrix with the arguments -originX, -originY, -originZ.
    translate_self(-origin_x.value_or(0), -origin_y.value_or(0), -origin_z.value_or(0));

    // 4. If scale is not 1, set is 2D of the current matrix to false.
    if (scale != 1)
        m_is_2d = false;

    // 5. Return the current matrix.
    return *this;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-rotateself
JS::NonnullGCPtr<DOMMatrix> DOMMatrix::rotate_self(Optional<double> rot_x, Optional<double> rot_y, Optional<double> rot_z)
{
    // 1. If rotY and rotZ are both missing, set rotZ to the value of rotX and set rotX and rotY to 0.
    if (!rot_y.has_value() && !rot_z.has_value()) {
        rot_z = rot_x;
        rot_x = 0;
        rot_y = 0;
    }

    // 2. If rotY is still missing, set rotY to 0.
    if (!rot_y.has_value())
        rot_y = 0;

    // 3. If rotZ is still missing, set rotZ to 0.
    if (!rot_z.has_value())
        rot_z = 0;

    // 4. If rotX or rotY are not 0 or -0, set is 2D of the current matrix to false.
    if (rot_x != 0 || rot_x != -0 || rot_y != 0 || rot_y != -0)
        m_is_2d = false;

    // 5. Post-multiply a rotation transformation on the current matrix around the vector 0, 0, 1 by the specified rotation rotZ in degrees. The 3D rotation matrix is described in CSS Transforms with alpha = rotZ in degrees. [CSS3-TRANSFORMS]
    m_matrix = m_matrix * Gfx::rotation_matrix<double>(Vector3<double> { 0.0, 0.0, 1.0 }, AK::to_radians(rot_z.value()));

    // 6. Post-multiply a rotation transformation on the current matrix around the vector 0, 1, 0 by the specified rotation rotY in degrees. The 3D rotation matrix is described in CSS Transforms with alpha = rotY in degrees. [CSS3-TRANSFORMS]
    m_matrix = m_matrix * Gfx::rotation_matrix<double>(Vector3<double> { 0.0, 1.0, 0.0 }, AK::to_radians(rot_y.value()));

    // 7. Post-multiply a rotation transformation on the current matrix around the vector 1, 0, 0 by the specified rotation rotX in degrees. The 3D rotation matrix is described in CSS Transforms with alpha = rotX in degrees. [CSS3-TRANSFORMS]
    m_matrix = m_matrix * Gfx::rotation_matrix<double>(Vector3<double> { 1.0, 0.0, 0.0 }, AK::to_radians(rot_x.value()));

    // 8. Return the current matrix.
    return *this;
}

JS::NonnullGCPtr<DOMMatrix> DOMMatrix::rotate_from_vector_self(Optional<double> x, Optional<double> y)
{
    // 1. Post-multiply a rotation transformation on the current matrix.
    //    The rotation angle is determined by the angle between the vector (1,0)T and (x,y)T in the clockwise direction. If x and y should both be 0 or -0, the angle is specified as 0.
    double angle = (x == 0 || x == -0) && (y == 0 || y == -0) ? 0.0 : atan2(y.value_or(0), x.value_or(0));

    // The 2D rotation matrix is described in CSS Transforms where alpha is the angle between the vector (1,0)T and (x,y)T in degrees. [CSS3-TRANSFORMS]
    m_matrix = m_matrix * Gfx::rotation_matrix<double>(Vector3<double> { 0.0, 0.0, 1.0 }, angle);

    // 2. Return the current matrix.
    return *this;
}

JS::NonnullGCPtr<DOMMatrix> DOMMatrix::rotate_axis_angle_self(Optional<double> x, Optional<double> y, Optional<double> z, Optional<double> angle)
{
    // 1. Post-multiply a rotation transformation on the current matrix around the specified vector x, y, z by the specified rotation angle in degrees. The 3D rotation matrix is described in CSS Transforms with alpha = angle in degrees. [CSS3-TRANSFORMS]
    m_matrix = m_matrix * Gfx::rotation_matrix<double>(Vector3<double> { x.value_or(0), y.value_or(0), z.value_or(0) }.normalized(), AK::to_radians(angle.value()));

    // 2. If x or y are not 0 or -0, set is 2D of the current matrix to false.
    if (x != 0 || x != -0 || y != 0 || y != -0)
        m_is_2d = false;

    // 3. Return the current matrix.
    return *this;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-skewxself
JS::NonnullGCPtr<DOMMatrix> DOMMatrix::skew_x_self(double sx)
{
    // 1. Post-multiply a skewX transformation on the current matrix by the specified angle sx in degrees. The 2D skewX matrix is described in CSS Transforms with alpha = sx in degrees. [CSS3-TRANSFORMS]
    Gfx::DoubleMatrix4x4 skew_matrix = {
        1, tan(AK::to_radians(sx)), 0, 0,
        0, 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    m_matrix = m_matrix * skew_matrix;

    // 3. Return the current matrix.
    return *this;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-skewyself
JS::NonnullGCPtr<DOMMatrix> DOMMatrix::skew_y_self(double sy)
{
    // 1. Post-multiply a skewX transformation on the current matrix by the specified angle sy in degrees. The 2D skewY matrix is described in CSS Transforms with beta = sy in degrees. [CSS3-TRANSFORMS]
    Gfx::DoubleMatrix4x4 skew_matrix = {
        1, 0, 0, 0,
        tan(AK::to_radians(sy)), 1, 0, 0,
        0, 0, 1, 0,
        0, 0, 0, 1
    };
    m_matrix = m_matrix * skew_matrix;

    // 3. Return the current matrix.
    return *this;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-invertself
JS::NonnullGCPtr<DOMMatrix> DOMMatrix::invert_self()
{
    bool is_invertible = m_matrix.is_invertible();

    // 1. Invert the current matrix.
    if (is_invertible)
        m_matrix = m_matrix.inverse();

    // 2. If the current matrix is not invertible set all attributes to NaN and set is 2D to false.
    if (!is_invertible) {
        for (u8 i = 0; i < 4; i++) {
            for (u8 j = 0; j < 4; j++)
                m_matrix.elements()[j][i] = NAN;
        }
        m_is_2d = false;
    }

    // 3. Return the current matrix.
    return *this;
}

// https://drafts.fxtf.org/geometry/#dom-dommatrix-setmatrixvalue
WebIDL::ExceptionOr<JS::NonnullGCPtr<DOMMatrix>> DOMMatrix::set_matrix_value(String const& transform_list)
{
    // 1. Parse transformList into an abstract matrix, and let matrix and 2dTransform be the result. If the result is failure, then throw a "SyntaxError" DOMException.
    auto result = TRY(parse_dom_matrix_init_string(realm(), transform_list));

    // 2. Set is 2D to the value of 2dTransform.
    m_is_2d = result.is_2d_transform;

    // 3. Set m11 element through m44 element to the element values of matrix in column-major order.
    m_matrix = result.matrix;

    // 4. Return the current matrix.
    return JS::NonnullGCPtr<DOMMatrix>(*this);
}

}
