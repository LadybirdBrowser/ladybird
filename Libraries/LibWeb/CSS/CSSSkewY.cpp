/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSSkewY.h"
#include <LibWeb/Bindings/CSSSkewYPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSSkewY);

GC::Ref<CSSSkewY> CSSSkewY::create(JS::Realm& realm, GC::Ref<CSSNumericValue> ay)
{
    return realm.create<CSSSkewY>(realm, ay);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskewy-cssskewy
WebIDL::ExceptionOr<GC::Ref<CSSSkewY>> CSSSkewY::construct_impl(JS::Realm& realm, GC::Ref<CSSNumericValue> ay)
{
    // The CSSSkewY(ay) constructor must, when invoked, perform the following steps:

    // 1. If ay does not match <angle>, throw a TypeError.
    if (!ay->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkewY ay component doesn't match <angle>"sv };

    // 2. Return a new CSSSkewY object with its ay internal slot set to ay, and its is2D internal slot set to true.
    return CSSSkewY::create(realm, ay);
}

CSSSkewY::CSSSkewY(JS::Realm& realm, GC::Ref<CSSNumericValue> ay)
    : CSSTransformComponent(realm, Is2D::Yes)
    , m_ay(ay)
{
}

CSSSkewY::~CSSSkewY() = default;

void CSSSkewY::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSSkewY);
    Base::initialize(realm);
}

void CSSSkewY::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_ay);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssskewy
WebIDL::ExceptionOr<Utf16String> CSSSkewY::to_string() const
{
    // 1. Let s initially be "skewY(".
    StringBuilder builder { StringBuilder::Mode::UTF16 };
    builder.append("skewY("sv);

    // 2. Serialize this’s ay internal slot, and append it to s.
    builder.append(m_ay->to_string());

    // 3. Append ")" to s, and return s.
    builder.append(")"sv);
    return builder.to_utf16_string();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSSkewY::to_matrix() const
{
    // 1. Let matrix be a new DOMMatrix object, initialized to this’s equivalent 4x4 transform matrix, as defined in
    //    CSS Transforms 1 § 12. Mathematical Description of Transform Functions, and with its is2D internal slot set
    //    to the same value as this’s is2D internal slot.
    //    NOTE: Recall that the is2D flag affects what transform, and thus what equivalent matrix, a
    //          CSSTransformComponent represents.
    //    As the entries of such a matrix are defined relative to the px unit, if any <length>s in this involved in
    //    generating the matrix are not compatible units with px (such as relative lengths or percentages), throw a
    //    TypeError.
    auto matrix = Geometry::DOMMatrix::create(realm());

    // NB: to() throws a TypeError if the conversion can't be done.
    auto ay_rad = TRY(m_ay->to("rad"_fly_string))->value();
    matrix->set_m12(tanf(ay_rad));

    // 2. Return matrix.
    return matrix;
}

WebIDL::ExceptionOr<void> CSSSkewY::set_ay(GC::Ref<CSSNumericValue> ay)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values.
    if (!ay->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkewY ay component doesn't match <angle>"sv };
    m_ay = ay;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskew-is2d
void CSSSkewY::set_is_2d(bool)
{
    // The is2D attribute of a CSSSkewY, CSSSkewYX, or CSSSkewYY object must, on setting, do nothing.
}

}
