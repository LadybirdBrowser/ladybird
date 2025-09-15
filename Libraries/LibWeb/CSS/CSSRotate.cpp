/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSRotate.h"
#include <LibWeb/Bindings/CSSRotatePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSRotate);

GC::Ref<CSSRotate> CSSRotate::create(JS::Realm& realm, Is2D is_2d, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z, GC::Ref<CSSNumericValue> angle)
{
    return realm.create<CSSRotate>(realm, is_2d, x, y, z, angle);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-cssrotate
WebIDL::ExceptionOr<GC::Ref<CSSRotate>> CSSRotate::construct_impl(JS::Realm& realm, GC::Ref<CSSNumericValue> angle)
{
    // The CSSRotate(angle) constructor must, when invoked, perform the following steps:

    // 1. If angle doesn’t match <angle>, throw a TypeError.
    if (!angle->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate angle component doesn't match <angle>"sv };

    // 2. Return a new CSSRotate with its angle internal slot set to angle, its x and y internal slots set to new unit
    //    values of (0, "number"), its z internal slot set to a new unit value of (1, "number"), and its is2D internal
    //    slot set to true.
    return realm.create<CSSRotate>(realm, Is2D::Yes,
        CSSUnitValue::create(realm, 0, "number"_fly_string),
        CSSUnitValue::create(realm, 0, "number"_fly_string),
        CSSUnitValue::create(realm, 1, "number"_fly_string),
        angle);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-cssrotate-x-y-z-anglec
WebIDL::ExceptionOr<GC::Ref<CSSRotate>> CSSRotate::construct_impl(JS::Realm& realm, CSSNumberish x, CSSNumberish y, CSSNumberish z, GC::Ref<CSSNumericValue> angle)
{
    // The CSSRotate(x, y, z, angle) constructor must, when invoked, perform the following steps:

    // 1. If angle doesn’t match <angle>, throw a TypeError.
    if (!angle->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate angle component doesn't match <angle>"sv };

    // 2. Let x, y, and z be replaced by the result of rectifying a numberish value.
    auto rectified_x = rectify_a_numberish_value(realm, x);
    auto rectified_y = rectify_a_numberish_value(realm, y);
    auto rectified_z = rectify_a_numberish_value(realm, z);

    // 3. If x, y, or z don’t match <number>, throw a TypeError.
    if (!rectified_x->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate x component doesn't match <number>"sv };
    if (!rectified_y->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate y component doesn't match <number>"sv };
    if (!rectified_z->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate z component doesn't match <number>"sv };

    // 4. Return a new CSSRotate with its angle internal slot set to angle, its x, y, z internal slots set to x, y, and
    //    z, and its is2D internal slot set to false.
    return realm.create<CSSRotate>(realm, Is2D::No, rectified_x, rectified_y, rectified_z, angle);
}

CSSRotate::CSSRotate(JS::Realm& realm, Is2D is_2d, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z, GC::Ref<CSSNumericValue> angle)
    : CSSTransformComponent(realm, is_2d)
    , m_x(x)
    , m_y(y)
    , m_z(z)
    , m_angle(angle)
{
}

CSSRotate::~CSSRotate() = default;

void CSSRotate::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSRotate);
    Base::initialize(realm);
}

void CSSRotate::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_z);
    visitor.visit(m_angle);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssrotate
WebIDL::ExceptionOr<Utf16String> CSSRotate::to_string() const
{
    // 1. Let s initially be the empty string.
    StringBuilder builder { StringBuilder::Mode::UTF16 };

    // 2. If this’s is2D internal slot is false:
    if (!is_2d()) {
        // 1. Append "rotate3d(" to s.
        builder.append("rotate3d("sv);

        // 2. Serialize this’s x internal slot, and append it to s.
        builder.append(m_x->to_string());

        // 3. Append ", " to s.
        builder.append(", "sv);

        // 4. Serialize this’s y internal slot, and append it to s.
        builder.append(m_y->to_string());

        // 5. Append ", " to s.
        builder.append(", "sv);

        // 6. Serialize this’s z internal slot, and append it to s.
        builder.append(m_z->to_string());

        // 7. Append "," to s.
        builder.append(", "sv);

        // 8. Serialize this’s angle internal slot, and append it to s.
        builder.append(m_angle->to_string());

        // 9. Append ")" to s, and return s.
        builder.append(")"sv);
        return builder.to_utf16_string();
    }
    // 2. Otherwise:
    else {
        // 1. Append "rotate(" to s.
        builder.append("rotate("sv);

        // 2. Serialize this’s angle internal slot, and append it to s.
        builder.append(m_angle->to_string());

        // 3. Append ")" to s, and return s.
        builder.append(")"sv);
        return builder.to_utf16_string();
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSRotate::to_matrix() const
{
    // 1. Let matrix be a new DOMMatrix object, initialized to this’s equivalent 4x4 transform matrix, as defined in
    //    CSS Transforms 1 § 12. Mathematical Description of Transform Functions, and with its is2D internal slot set
    //    to the same value as this’s is2D internal slot.
    //    NOTE: Recall that the is2D flag affects what transform, and thus what equivalent matrix, a
    //          CSSTransformComponent represents.
    //    As the entries of such a matrix are defined relative to the px unit, if any <length>s in this involved in
    //    generating the matrix are not compatible units with px (such as relative lengths or percentages), throw a
    //    TypeError.
    // 2. Return matrix.

    auto matrix = Geometry::DOMMatrix::create(realm());

    // NB: to() throws a TypeError if the conversion can't be done.
    auto angle = TRY(m_angle->to("deg"_fly_string))->value();

    if (is_2d())
        return matrix->rotate_axis_angle_self(0, 0, 1, angle);

    auto x = TRY(m_x->to("number"_fly_string))->value();
    auto y = TRY(m_y->to("number"_fly_string))->value();
    auto z = TRY(m_z->to("number"_fly_string))->value();

    return matrix->rotate_axis_angle_self(x, y, z, angle);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-x
WebIDL::ExceptionOr<void> CSSRotate::set_x(CSSNumberish value)
{
    // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set the
    // corresponding internal slot to the result of that.
    // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
    auto rectified_x = rectify_a_numberish_value(realm(), value);
    if (!rectified_x->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate x component doesn't match <number>"sv };
    m_x = rectified_x;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-y
WebIDL::ExceptionOr<void> CSSRotate::set_y(CSSNumberish value)
{
    // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set the
    // corresponding internal slot to the result of that.
    // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
    auto rectified_y = rectify_a_numberish_value(realm(), value);
    if (!rectified_y->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate y component doesn't match <number>"sv };
    m_y = rectified_y;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssrotate-z
WebIDL::ExceptionOr<void> CSSRotate::set_z(CSSNumberish value)
{
    // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set the
    // corresponding internal slot to the result of that.
    // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
    auto rectified_z = rectify_a_numberish_value(realm(), value);
    if (!rectified_z->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate z component doesn't match <number>"sv };
    m_z = rectified_z;
    return {};
}

WebIDL::ExceptionOr<void> CSSRotate::set_angle(GC::Ref<CSSNumericValue> value)
{
    // AD-HOC: Not specced. WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
    if (!value->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSRotate angle component doesn't match <angle>"sv };
    m_angle = value;
    return {};
}

}
