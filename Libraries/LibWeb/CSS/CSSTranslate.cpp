/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSTranslate.h"
#include <LibWeb/Bindings/CSSTranslatePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSTranslate);

GC::Ref<CSSTranslate> CSSTranslate::create(JS::Realm& realm, Is2D is_2d, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z)
{
    return realm.create<CSSTranslate>(realm, is_2d, x, y, z);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstranslate-csstranslate
WebIDL::ExceptionOr<GC::Ref<CSSTranslate>> CSSTranslate::construct_impl(JS::Realm& realm, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ptr<CSSNumericValue> z)
{
    // The CSSTranslate(x, y, z) constructor must, when invoked, perform the following steps:

    // 1. If x or y don’t match <length-percentage>, throw a TypeError.
    if (!x->type().matches_length_percentage({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSTranslate x component doesn't match <length-percentage>"sv };

    if (!y->type().matches_length_percentage({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSTranslate y component doesn't match <length-percentage>"sv };

    // 2. If z was passed, but doesn’t match <length>, throw a TypeError.
    if (z && !z->type().matches_length({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSTranslate z component doesn't match <length>"sv };

    // 3. Let this be a new CSSTranslate object, with its x and y internal slots set to x and y.
    // 4. If z was passed, set this’s z internal slot to z, and set this’s is2D internal slot to false.
    // 5. If z was not passed, set this’s z internal slot to a new unit value of (0, "px"), and set this’s is2D internal slot to true.
    Is2D is_2d = Is2D::No;
    if (!z) {
        is_2d = Is2D::Yes;
        z = CSSUnitValue::create(realm, 0, "px"_fly_string);
    }
    auto this_ = realm.create<CSSTranslate>(realm, is_2d, x, y, z.as_nonnull());

    // 6. Return this.
    return this_;
}

CSSTranslate::CSSTranslate(JS::Realm& realm, Is2D is_2d, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z)
    : CSSTransformComponent(realm, is_2d)
    , m_x(x)
    , m_y(y)
    , m_z(z)
{
}

CSSTranslate::~CSSTranslate() = default;

void CSSTranslate::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSTranslate);
    Base::initialize(realm);
}

void CSSTranslate::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_z);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-csstranslate
WebIDL::ExceptionOr<Utf16String> CSSTranslate::to_string() const
{
    // 1. Let s initially be the empty string.
    StringBuilder builder { StringBuilder::Mode::UTF16 };

    // 2. If this’s is2D internal slot is false:
    if (!is_2d()) {
        // 1. Append "translate3d(" to s.
        builder.append("translate3d("sv);

        // 2. Serialize this’s x internal slot, and append it to s.
        builder.append(TRY(m_x->to_string()));

        // 3. Append ", " to s.
        builder.append(", "sv);

        // 4. Serialize this’s y internal slot, and append it to s.
        builder.append(TRY(m_y->to_string()));

        // 5. Append ", " to s.
        builder.append(", "sv);

        // 6. Serialize this’s z internal slot, and append it to s.
        builder.append(TRY(m_z->to_string()));

        // 7. Append ")" to s, and return s.
        builder.append(")"sv);
        return builder.to_utf16_string();
    }
    // 3. Otherwise:
    else {
        // 1. Append "translate(" to s.
        builder.append("translate("sv);

        // 2. Serialize this’s x internal slot, and append it to s.
        builder.append(TRY(m_x->to_string()));

        // 3. Append ", " to s.
        builder.append(", "sv);

        // 4. Serialize this’s y internal slot, and append it to s.
        builder.append(TRY(m_y->to_string()));

        // 5. Append ")" to s, and return s.
        builder.append(")"sv);
        return builder.to_utf16_string();
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSTranslate::to_matrix() const
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
    matrix->set_m41(TRY(m_x->to("px"_fly_string))->value());
    matrix->set_m42(TRY(m_y->to("px"_fly_string))->value());
    if (!is_2d())
        matrix->set_m43(TRY(m_z->to("px"_fly_string))->value());

    // 2. Return matrix.
    return matrix;
}

WebIDL::ExceptionOr<void> CSSTranslate::set_x(GC::Ref<CSSNumericValue> x)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values.
    if (!x->type().matches_length_percentage({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSTranslate x component doesn't match <length-percentage>"sv };
    m_x = x;
    return {};
}

WebIDL::ExceptionOr<void> CSSTranslate::set_y(GC::Ref<CSSNumericValue> y)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values.
    if (!y->type().matches_length_percentage({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSTranslate y component doesn't match <length-percentage>"sv };
    m_y = y;
    return {};
}

WebIDL::ExceptionOr<void> CSSTranslate::set_z(GC::Ref<CSSNumericValue> z)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values.
    if (!z->type().matches_length({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSTranslate z component doesn't match <length>"sv };
    m_z = z;
    return {};
}

}
