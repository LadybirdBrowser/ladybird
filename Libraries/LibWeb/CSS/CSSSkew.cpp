/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSSkew.h"
#include <LibWeb/Bindings/CSSSkewPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSSkew);

GC::Ref<CSSSkew> CSSSkew::create(JS::Realm& realm, GC::Ref<CSSNumericValue> ax, GC::Ref<CSSNumericValue> ay)
{
    return realm.create<CSSSkew>(realm, ax, ay);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskew-cssskew
WebIDL::ExceptionOr<GC::Ref<CSSSkew>> CSSSkew::construct_impl(JS::Realm& realm, GC::Ref<CSSNumericValue> ax, GC::Ref<CSSNumericValue> ay)
{
    // The CSSSkew(ax, ay) constructor must, when invoked, perform the following steps:

    // 1. If ax or ay do not match <angle>, throw a TypeError.
    if (!ax->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkew ax component doesn't match <angle>"sv };
    if (!ay->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkew ay component doesn't match <angle>"sv };

    // 2. Return a new CSSSkew object with its ax and ay internal slots set to ax and ay, and its is2D internal slot
    //    set to true.
    return CSSSkew::create(realm, ax, ay);
}

CSSSkew::CSSSkew(JS::Realm& realm, GC::Ref<CSSNumericValue> ax, GC::Ref<CSSNumericValue> ay)
    : CSSTransformComponent(realm, Is2D::Yes)
    , m_ax(ax)
    , m_ay(ay)
{
}

CSSSkew::~CSSSkew() = default;

void CSSSkew::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSSkew);
    Base::initialize(realm);
}

void CSSSkew::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_ax);
    visitor.visit(m_ay);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssskew
WebIDL::ExceptionOr<Utf16String> CSSSkew::to_string() const
{
    // 1. Let s initially be "skew(".
    StringBuilder builder { StringBuilder::Mode::UTF16 };
    builder.append("skew("sv);

    // 2. Serialize this’s ax internal slot, and append it to s.
    builder.append(m_ax->to_string());

    // 3. If this’s ay internal slot is a CSSUnitValue with a value of 0, then append ")" to s and return s.
    if (auto* ay_unit_value = as_if<CSSUnitValue>(*m_ay); ay_unit_value && ay_unit_value->value() == 0) {
        builder.append(")"sv);
        return builder.to_utf16_string();
    }

    // 4. Otherwise, append ", " to s.
    builder.append(", "sv);

    // 5. Serialize this’s ay internal slot, and append it to s.
    builder.append(m_ay->to_string());

    // 6. Append ")" to s, and return s.
    builder.append(")"sv);
    return builder.to_utf16_string();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSSkew::to_matrix() const
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
    auto ax_rad = TRY(m_ax->to("rad"_fly_string))->value();
    auto ay_rad = TRY(m_ay->to("rad"_fly_string))->value();

    matrix->set_m21(tanf(ax_rad));
    matrix->set_m12(tanf(ay_rad));

    // 2. Return matrix.
    return matrix;
}

WebIDL::ExceptionOr<void> CSSSkew::set_ax(GC::Ref<CSSNumericValue> ax)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values.
    if (!ax->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkew ax component doesn't match <angle>"sv };
    m_ax = ax;
    return {};
}

WebIDL::ExceptionOr<void> CSSSkew::set_ay(GC::Ref<CSSNumericValue> ay)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values.
    if (!ay->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkew ay component doesn't match <angle>"sv };
    m_ay = ay;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskew-is2d
void CSSSkew::set_is_2d(bool)
{
    // The is2D attribute of a CSSSkew, CSSSkewX, or CSSSkewY object must, on setting, do nothing.
}

}
