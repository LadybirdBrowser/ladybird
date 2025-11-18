/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSScale.h"
#include <LibWeb/Bindings/CSSScalePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSScale);

GC::Ref<CSSScale> CSSScale::create(JS::Realm& realm, Is2D is_2d, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z)
{
    return realm.create<CSSScale>(realm, is_2d, x, y, z);
}

WebIDL::ExceptionOr<GC::Ref<CSSScale>> CSSScale::construct_impl(JS::Realm& realm, CSSNumberish x, CSSNumberish y, Optional<CSSNumberish> z)
{
    // The CSSScale(x, y, z) constructor must, when invoked, perform the following steps:

    // 1. Let x, y, and z (if passed) be replaced by the result of rectifying a numberish value.
    auto rectified_x = rectify_a_numberish_value(realm, x);
    auto rectified_y = rectify_a_numberish_value(realm, y);
    auto rectified_z = z.map([&](auto& it) { return rectify_a_numberish_value(realm, it); });

    // 2. If x, y, or z (if passed) don’t match <number>, throw a TypeError.
    if (!rectified_x->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSScale x component doesn't match <number>"sv };
    if (!rectified_y->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSScale y component doesn't match <number>"sv };
    if (rectified_z.has_value() && !rectified_z.value()->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSScale z component doesn't match <number>"sv };

    // 3. Let this be a new CSSScale object, with its x and y internal slots set to x and y.
    // 4. If z was passed, set this’s z internal slot to z, and set this’s is2D internal slot to false.
    // 5. If z was not passed, set this’s z internal slot to a new unit value of (1, "number"), and set this’s is2D internal slot to true.
    Is2D is_2d = Is2D::No;
    if (!rectified_z.has_value()) {
        rectified_z = CSSUnitValue::create(realm, 1, "number"_fly_string);
        is_2d = Is2D::Yes;
    }
    auto this_ = CSSScale::create(realm, is_2d, rectified_x, rectified_y, rectified_z.release_value());

    // 6. Return this.
    return this_;
}

CSSScale::CSSScale(JS::Realm& realm, Is2D is_2d, GC::Ref<CSSNumericValue> x, GC::Ref<CSSNumericValue> y, GC::Ref<CSSNumericValue> z)
    : CSSTransformComponent(realm, is_2d)
    , m_x(x)
    , m_y(y)
    , m_z(z)
{
}

CSSScale::~CSSScale() = default;

void CSSScale::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSScale);
    Base::initialize(realm);
}

void CSSScale::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_x);
    visitor.visit(m_y);
    visitor.visit(m_z);
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssscale
WebIDL::ExceptionOr<Utf16String> CSSScale::to_string() const
{
    // 1. Let s initially be the empty string.
    StringBuilder builder { StringBuilder::Mode::UTF16 };

    // 2. If this’s is2D internal slot is false:
    if (!is_2d()) {

        // 1. Append "scale3d(" to s.
        builder.append("scale3d("sv);

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
        // 1. Append "scale(" to s.
        builder.append("scale("sv);

        // 2. Serialize this’s x internal slot, and append it to s.
        builder.append(TRY(m_x->to_string()));

        // 3. If this’s x and y internal slots are equal numeric values, append ")" to s and return s.
        // AD-HOC: Don't do this - neither Chrome nor Safari show this behavior.
        //         Upstream issue: https://github.com/w3c/css-houdini-drafts/issues/1161

        // 4. Otherwise, append ", " to s.
        builder.append(", "sv);

        // 5. Serialize this’s y internal slot, and append it to s.
        builder.append(TRY(m_y->to_string()));

        // 6. Append ")" to s, and return s.
        builder.append(")"sv);
        return builder.to_utf16_string();
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformcomponent-tomatrix
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSScale::to_matrix() const
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
    auto x = TRY(m_x->to("number"_fly_string))->value();
    auto y = TRY(m_y->to("number"_fly_string))->value();

    if (is_2d())
        return matrix->scale_self(x, y, {}, {}, {}, {});

    auto z = TRY(m_z->to("number"_fly_string))->value();
    return matrix->scale_self(x, y, z, {}, {}, {});
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssscale-x
WebIDL::ExceptionOr<void> CSSScale::set_x(CSSNumberish value)
{
    // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set the
    // corresponding internal slot to the result of that.
    // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
    auto rectified_x = rectify_a_numberish_value(realm(), value);
    if (!rectified_x->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSScale x component doesn't match <number>"sv };
    m_x = rectified_x;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssscale-y
WebIDL::ExceptionOr<void> CSSScale::set_y(CSSNumberish value)
{
    // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set the
    // corresponding internal slot to the result of that.
    // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
    auto rectified_y = rectify_a_numberish_value(realm(), value);
    if (!rectified_y->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSScale y component doesn't match <number>"sv };
    m_y = rectified_y;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssscale-z
WebIDL::ExceptionOr<void> CSSScale::set_z(CSSNumberish value)
{
    // The x, y, and z attributes must, on setting to a new value val, rectify a numberish value from val and set the
    // corresponding internal slot to the result of that.
    // AD-HOC: WPT expects this to throw for invalid values. https://github.com/w3c/css-houdini-drafts/issues/1153
    auto rectified_z = rectify_a_numberish_value(realm(), value);
    if (!rectified_z->type().matches_number({}))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSScale z component doesn't match <number>"sv };
    m_z = rectified_z;
    return {};
}

WebIDL::ExceptionOr<NonnullRefPtr<TransformationStyleValue const>> CSSScale::create_style_value(PropertyNameAndID const& property) const
{
    if (is_2d()) {
        return TransformationStyleValue::create(property.id(), TransformFunction::Scale,
            {
                TRY(m_x->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::No)),
                TRY(m_y->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::No)),
            });
    }

    return TransformationStyleValue::create(property.id(), TransformFunction::Scale3d,
        {
            TRY(m_x->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::No)),
            TRY(m_y->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::No)),
            TRY(m_z->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::No)),
        });
}

}
