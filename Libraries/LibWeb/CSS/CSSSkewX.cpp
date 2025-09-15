/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSSkewX.h"
#include <LibWeb/Bindings/CSSSkewXPrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSSkewX);

GC::Ref<CSSSkewX> CSSSkewX::create(JS::Realm& realm, GC::Ref<CSSNumericValue> ax)
{
    return realm.create<CSSSkewX>(realm, ax);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskewx-cssskewx
WebIDL::ExceptionOr<GC::Ref<CSSSkewX>> CSSSkewX::construct_impl(JS::Realm& realm, GC::Ref<CSSNumericValue> ax)
{
    // The CSSSkewX(ax) constructor must, when invoked, perform the following steps:

    // 1. If ax does not match <angle>, throw a TypeError.
    if (!ax->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkewX ax component doesn't match <angle>"sv };

    // 2. Return a new CSSSkewX object with its ax internal slot set to ax, and its is2D internal slot set to true.
    return CSSSkewX::create(realm, ax);
}

CSSSkewX::CSSSkewX(JS::Realm& realm, GC::Ref<CSSNumericValue> ax)
    : CSSTransformComponent(realm, Is2D::Yes)
    , m_ax(ax)
{
}

CSSSkewX::~CSSSkewX() = default;

void CSSSkewX::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSSkewX);
    Base::initialize(realm);
}

void CSSSkewX::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_ax);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssskewx
WebIDL::ExceptionOr<Utf16String> CSSSkewX::to_string() const
{
    // 1. Let s initially be "skewX(".
    StringBuilder builder { StringBuilder::Mode::UTF16 };
    builder.append("skewX("sv);

    // 2. Serialize this’s ax internal slot, and append it to s.
    builder.append(m_ax->to_string());

    // 3. Append ")" to s, and return s.
    builder.append(")"sv);
    return builder.to_utf16_string();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSSkewX::to_matrix() const
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
    matrix->set_m21(tanf(ax_rad));

    // 2. Return matrix.
    return matrix;
}

WebIDL::ExceptionOr<void> CSSSkewX::set_ax(GC::Ref<CSSNumericValue> ax)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values.
    if (!ax->type().matches_angle({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSSkewX ax component doesn't match <angle>"sv };
    m_ax = ax;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssskew-is2d
void CSSSkewX::set_is_2d(bool)
{
    // The is2D attribute of a CSSSkewX, CSSSkewXX, or CSSSkewXY object must, on setting, do nothing.
}

}
