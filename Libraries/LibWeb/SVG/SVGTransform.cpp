/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2024, Jamie Mansfield <jmansfield@cadixdev.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <LibGC/Heap.h>
#include <LibWeb/Bindings/DOMMatrixReadOnly.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/SVG/SVGTransform.h>
#include <LibWeb/SVG/SVGTransformList.h>

namespace Web::SVG {

GC_DEFINE_ALLOCATOR(SVGTransform);

// NB: The elements are m11, m12, m13, m14, m21, ..., m44, and a 2D matrix is matrix(m11, m12, m21, m22, m41, m42).
static Array<double, 16> elements_from_2d_matrix(double a, double b, double c, double d, double e, double f)
{
    return { a, b, 0, 0, c, d, 0, 0, 0, 0, 1, 0, e, f, 0, 1 };
}

SVGTransform::Value SVGTransform::Value::from_transform(Transform const& transform)
{
    // NB: The matrix is computed in double precision, so that it matches the transform function value as closely as
    //     possible.
    return transform.operation.visit(
        [](Transform::Translate const& translate) {
            return Value { Type::Translate, 0, elements_from_2d_matrix(1, 0, 0, 1, translate.x, translate.y), true };
        },
        [](Transform::Scale const& scale) {
            return Value { Type::Scale, 0, elements_from_2d_matrix(scale.x, 0, 0, scale.y, 0, 0), true };
        },
        [](Transform::Rotate const& rotate) {
            // NB: This is translate(cx, cy) rotate(angle) translate(-cx, -cy).
            auto radians = AK::to_radians(static_cast<double>(rotate.a));
            auto cosine = AK::cos(radians);
            auto sine = AK::sin(radians);
            double cx = rotate.x;
            double cy = rotate.y;
            return Value { Type::Rotate, rotate.a, elements_from_2d_matrix(cosine, sine, -sine, cosine, cx - (cosine * cx) + (sine * cy), cy - (sine * cx) - (cosine * cy)), true };
        },
        [](Transform::SkewX const& skew_x) {
            return Value { Type::SkewX, skew_x.a, elements_from_2d_matrix(1, 0, AK::tan(AK::to_radians(static_cast<double>(skew_x.a))), 1, 0, 0), true };
        },
        [](Transform::SkewY const& skew_y) {
            return Value { Type::SkewY, skew_y.a, elements_from_2d_matrix(1, AK::tan(AK::to_radians(static_cast<double>(skew_y.a))), 0, 1, 0, 0), true };
        },
        [](Transform::Matrix const& matrix) {
            return Value { Type::Matrix, 0, elements_from_2d_matrix(matrix.a, matrix.b, matrix.c, matrix.d, matrix.e, matrix.f), true };
        });
}

SVGTransform::Value SVGTransform::Value::from_matrix(Array<double, 16> const& elements, bool is_2d)
{
    // A matrix(…) value if the matrix is 2D, and otherwise a matrix3d(…) value, whose type is SVG_TRANSFORM_UNKNOWN.
    return Value { is_2d ? Type::Matrix : Type::Unknown, 0, elements, is_2d };
}

Gfx::AffineTransform SVGTransform::Value::affine_transform() const
{
    return { static_cast<float>(elements[0]), static_cast<float>(elements[1]), static_cast<float>(elements[4]), static_cast<float>(elements[5]), static_cast<float>(elements[12]), static_cast<float>(elements[13]) };
}

GC::Ref<SVGTransform> SVGTransform::create()
{
    return create(Value {});
}

GC::Ref<SVGTransform> SVGTransform::create(Value value)
{
    return GC::Heap::the().allocate<SVGTransform>(value);
}

SVGTransform::SVGTransform(Value value)
    : m_value(value)
{
}

SVGTransform::~SVGTransform() = default;

void SVGTransform::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_matrix);
    visitor.visit(m_list);
}

SVGTransform::Value SVGTransform::value() const
{
    // An SVGTransform object's matrix object is always kept synchronized with its value.
    // FIXME: Have the matrix object update the value when it is modified, so that a presentation attribute that the
    //        transform reflects is reserialized at that point. For now, the value catches up when it is next read.
    // FIXME: It should not be possible to modify the matrix object of a read only SVGTransform. Until it is prevented,
    //        changes to it are not adopted into the value, so that a read only SVGTransform cannot be modified.
    if (m_matrix && m_read_only == ReadOnly::No) {
        auto elements = m_matrix->elements();
        auto is_2d = m_matrix->is2d();
        if (elements != m_value.elements || is_2d != m_value.is_2d)
            m_value = Value::from_matrix(elements, is_2d);
    }
    return m_value;
}

void SVGTransform::set_value(Value value)
{
    m_value = value;
    if (m_matrix)
        m_matrix->set_elements(m_value.elements, m_value.is_2d);
}

void SVGTransform::set_value_from_reflected_attribute(Value value)
{
    // 3. Set item's value to v.
    // 4. If item is an SVGTransform object, then set the components of its matrix object to match the new transform
    //    function value.
    set_value(value);
}

// https://svgwg.org/svg2-draft/types.html#TermAttach
void SVGTransform::attach(GC::Ref<SVGTransformList> list, ReadOnly read_only)
{
    // Associate the SVGTransform with the element that the list interface object is associated with. Set the
    // SVGTransform to reflect an element of a presentation attribute value.
    // NB: Items of a list that is read only are read only too.
    m_list = list;
    m_read_only = read_only;
}

// https://svgwg.org/svg2-draft/types.html#TermDetach
void SVGTransform::detach()
{
    // Set the SVGTransform to no longer be associated with any element. If the SVGTransform is read only, set it to be
    // no longer read only.
    m_list = nullptr;
    m_read_only = ReadOnly::No;
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__type
SVGTransform::Type SVGTransform::type() const
{
    // 1. If the SVGTransform's value is a matrix(…), translate(…), scale(…), rotate(…), skewX(…) or skewY(…) function,
    //    then return the corresponding constant value from the transform type table above.
    // 2. Otherwise, return SVG_TRANSFORM_UNKNOWN.
    return value().type;
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__matrix
GC::Ref<Geometry::DOMMatrix> SVGTransform::matrix()
{
    // The matrix IDL attribute represents the transform as a 4x4 homogeneous matrix, and on getting returns the
    // SVGTransform's matrix object. When the matrix object is first created, its values are set to match the
    // SVGTransform's transform function value, and is set to reflects the SVGTransform.
    if (!m_matrix) {
        Gfx::DoubleMatrix4x4 matrix;
        for (size_t row = 0; row < 4; ++row) {
            for (size_t column = 0; column < 4; ++column)
                matrix[row, column] = m_value.elements[(column * 4) + row];
        }
        m_matrix = Geometry::DOMMatrix::create(matrix, m_value.is_2d);
    }
    return *m_matrix;
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__angle
float SVGTransform::angle() const
{
    // 1. If the SVGTransform object's value is a rotate(…), skewX(…) or skewY(…) function, return its angle argument
    //    in degrees.
    auto value = this->value();
    if (value.type == Type::Rotate || value.type == Type::SkewX || value.type == Type::SkewY)
        return value.angle;

    // 2. Otherwise, return 0.
    return 0;
}

WebIDL::ExceptionOr<void> SVGTransform::set_value_and_reserialize(Value value)
{
    // 1. If the SVGTransform object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify a read-only SVGTransform"_utf16);

    // 2. Set the SVGTransform object's value to a new transform function value.
    // 3. Set the components of the SVGTransform object's matrix object to match the new transform function value.
    set_value(value);

    // 4. If the SVGTransform object reflects an element of a presentation attribute value, then reserialize the
    //    reflected attribute.
    if (m_list)
        m_list->reserialize_reflected_attribute();
    return {};
}

// https://drafts.fxtf.org/geometry/#validate-and-fixup-2d
static WebIDL::ExceptionOr<Array<double, 6>> validate_and_fixup_2d(Bindings::DOMMatrix2DInit const& dict)
{
    // 1. If if at least one of the following conditions are true for dict, then throw a TypeError exception and abort
    //    these steps.
    //    - a and m11 are both present and SameValueZero(a, m11) is false.
    //    - b and m12 are both present and SameValueZero(b, m12) is false.
    //    - c and m21 are both present and SameValueZero(c, m21) is false.
    //    - d and m22 are both present and SameValueZero(d, m22) is false.
    //    - e and m41 are both present and SameValueZero(e, m41) is false.
    //    - f and m42 are both present and SameValueZero(f, m42) is false.
    auto same_value_zero = [](double x, double y) { return (isnan(x) && isnan(y)) || x == y; };
    auto conflicts = [&](Optional<double> const& x, Optional<double> const& y) {
        return x.has_value() && y.has_value() && !same_value_zero(*x, *y);
    };
    if (conflicts(dict.a, dict.m11) || conflicts(dict.b, dict.m12) || conflicts(dict.c, dict.m21)
        || conflicts(dict.d, dict.m22) || conflicts(dict.e, dict.m41) || conflicts(dict.f, dict.m42))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "DOMMatrix2DInit has conflicting members"_utf16 };

    // 2. If m11 is not present then set it to the value of member a, or value 1 if a is also not present.
    // 3. If m12 is not present then set it to the value of member b, or value 0 if b is also not present.
    // 4. If m21 is not present then set it to the value of member c, or value 0 if c is also not present.
    // 5. If m22 is not present then set it to the value of member d, or value 1 if d is also not present.
    // 6. If m41 is not present then set it to the value of member e, or value 0 if e is also not present.
    // 7. If m42 is not present then set it to the value of member f, or value 0 if f is also not present.
    return Array<double, 6> {
        dict.m11.value_or(dict.a.value_or(1)),
        dict.m12.value_or(dict.b.value_or(0)),
        dict.m21.value_or(dict.c.value_or(0)),
        dict.m22.value_or(dict.d.value_or(1)),
        dict.m41.value_or(dict.e.value_or(0)),
        dict.m42.value_or(dict.f.value_or(0)),
    };
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__setMatrix
WebIDL::ExceptionOr<void> SVGTransform::set_matrix(Bindings::DOMMatrix2DInit const& matrix)
{
    // 1. If the SVGTransform object is read only, then throw a NoModificationAllowedError.
    if (m_read_only == ReadOnly::Yes)
        return WebIDL::NoModificationAllowedError::create("Cannot modify a read-only SVGTransform"_utf16);

    // 2. Let newMatrix be the result of DOMMatrixReadOnly.fromMatrix(matrix), including the validate and fix-up steps
    //    for missing values. If that method throws an error, then re-throw that error and abort these steps.
    // NB: A DOMMatrix2DInit always results in a 2D matrix.
    auto new_matrix = TRY(validate_and_fixup_2d(matrix));

    // 3. If newMatrix.is2D() would return true, then set the SVGTransform object's value to a matrix(…) value that
    //    represents the same matrix as newMatrix.
    // 4. In either case, matrix object gets synchronized to the SVGTransform object's value.
    set_value(Value { Type::Matrix, 0, { new_matrix[0], new_matrix[1], 0, 0, new_matrix[2], new_matrix[3], 0, 0, 0, 0, 1, 0, new_matrix[4], new_matrix[5], 0, 1 }, true });

    // 5. If the SVGTransform object reflects a presentation attribute value of an element, then reserialize the
    //    reflected attribute.
    if (m_list)
        m_list->reserialize_reflected_attribute();
    return {};
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__setTranslate
WebIDL::ExceptionOr<void> SVGTransform::set_translate(float tx, float ty)
{
    // setTranslate(tx, ty): the new transform function value is translate(tx, ty)
    return set_value_and_reserialize(Value::from_transform({ Transform::Translate { tx, ty } }));
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__setScale
WebIDL::ExceptionOr<void> SVGTransform::set_scale(float sx, float sy)
{
    // setScale(sx, sy): the new transform function value is scale(sx, sy)
    return set_value_and_reserialize(Value::from_transform({ Transform::Scale { sx, sy } }));
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__setRotate
WebIDL::ExceptionOr<void> SVGTransform::set_rotate(float angle, float cx, float cy)
{
    // setRotate(angle, cx, cy): the new transform function value is rotate(angle, cx, cy)
    return set_value_and_reserialize(Value::from_transform({ Transform::Rotate { angle, cx, cy } }));
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__setSkewX
WebIDL::ExceptionOr<void> SVGTransform::set_skew_x(float angle)
{
    // setSkewX(angle): the new transform function value is skewX(angle)
    return set_value_and_reserialize(Value::from_transform({ Transform::SkewX { angle } }));
}

// https://svgwg.org/svg2-draft/coords.html#__svg__SVGTransform__setSkewY
WebIDL::ExceptionOr<void> SVGTransform::set_skew_y(float angle)
{
    // setSkewY(angle): the new transform function value is skewY(angle)
    return set_value_and_reserialize(Value::from_transform({ Transform::SkewY { angle } }));
}

}
