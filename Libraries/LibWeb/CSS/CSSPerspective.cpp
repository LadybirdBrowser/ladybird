/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSPerspective.h"
#include <LibWeb/Bindings/CSSPerspectivePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSNumericValue.h>
#include <LibWeb/CSS/CSSUnitValue.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSPerspective);

static WebIDL::ExceptionOr<CSSPerspectiveValueInternal> to_internal(JS::Realm& realm, CSSPerspectiveValue const& value)
{
    // Steps 1 and 2 of The CSSPerspective(length) constructor:
    // https://drafts.css-houdini.org/css-typed-om-1/#dom-cssperspective-cssperspective
    return value.visit(
        // 1. If length is a CSSNumericValue:
        [](GC::Root<CSSNumericValue> const& numeric_value) -> WebIDL::ExceptionOr<CSSPerspectiveValueInternal> {
            // 1. If length does not match <length>, throw a TypeError.
            if (!numeric_value->type().matches_length({})) {
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSPerspective length component doesn't match <length>"sv };
            }
            return { GC::Ref { *numeric_value } };
        },
        // 2. Otherwise (that is, if length is not a CSSNumericValue):
        [&realm](CSSKeywordish const& keywordish) -> WebIDL::ExceptionOr<CSSPerspectiveValueInternal> {
            // 1. Rectify a keywordish value from length, then set length to the result’s value.
            auto rectified_length = rectify_a_keywordish_value(realm, keywordish);

            // 2. If length does not represent a value that is an ASCII case-insensitive match for the keyword none,
            //    throw a TypeError.
            if (!rectified_length->value().equals_ignoring_ascii_case("none"_fly_string)) {
                return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSPerspective length component is a keyword other than `none`"sv };
            }

            return { rectified_length };
        });
}

GC::Ref<CSSPerspective> CSSPerspective::create(JS::Realm& realm, CSSPerspectiveValueInternal length)
{
    return realm.create<CSSPerspective>(realm, length);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssperspective-cssperspective
WebIDL::ExceptionOr<GC::Ref<CSSPerspective>> CSSPerspective::construct_impl(JS::Realm& realm, CSSPerspectiveValue length)
{
    // The CSSPerspective(length) constructor must, when invoked, perform the following steps:
    // NB: Steps 1 and 2 are implemented in to_internal().
    auto internal_length = TRY(to_internal(realm, length));

    // 3. Return a new CSSPerspective object with its length internal slot set to length, and its is2D internal slot
    //    set to false.
    return CSSPerspective::create(realm, internal_length);
}

CSSPerspective::CSSPerspective(JS::Realm& realm, CSSPerspectiveValueInternal length)
    : CSSTransformComponent(realm, Is2D::No)
    , m_length(length)
{
}

CSSPerspective::~CSSPerspective() = default;

void CSSPerspective::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSPerspective);
    Base::initialize(realm);
}

void CSSPerspective::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    m_length.visit([&visitor](auto const& it) { visitor.visit(it); });
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssperspective
WebIDL::ExceptionOr<Utf16String> CSSPerspective::to_string() const
{
    // 1. Let s initially be "perspective(".
    StringBuilder builder { StringBuilder::Mode::UTF16 };
    builder.append("perspective("sv);

    // 2. Serialize this’s length internal slot, with a minimum of 0px, and append it to s.
    auto serialized_length = TRY(m_length.visit(
        [](GC::Ref<CSSNumericValue> const& numeric_value) -> WebIDL::ExceptionOr<String> {
            return numeric_value->to_string({ .minimum = 0 });
        },
        [](GC::Ref<CSSKeywordValue> const& keyword_value) -> WebIDL::ExceptionOr<String> {
            return keyword_value->to_string();
        }));
    builder.append(serialized_length);

    // 3. Append ")" to s, and return s.
    builder.append(")"sv);
    return builder.to_utf16_string();
}

WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSPerspective::to_matrix() const
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

    TRY(m_length.visit(
        [&matrix](GC::Ref<CSSNumericValue> const& numeric_value) -> WebIDL::ExceptionOr<void> {
            // NB: to() throws a TypeError if the conversion can't be done.
            auto distance = TRY(numeric_value->to("px"_fly_string))->value();
            matrix->set_m34(-1 / max(distance, 1));
            return {};
        },
        [](GC::Ref<CSSKeywordValue> const&) -> WebIDL::ExceptionOr<void> {
            // NB: This is `none`, so do nothing.
            return {};
        }));

    // 2. Return matrix.
    return matrix;
}

CSSPerspectiveValue CSSPerspective::length() const
{
    return m_length.visit(
        [](GC::Ref<CSSNumericValue> const& numeric_value) -> CSSPerspectiveValue {
            return GC::Root { numeric_value };
        },
        [](GC::Ref<CSSKeywordValue> const& keyword_value) -> CSSPerspectiveValue {
            return CSSKeywordish { keyword_value };
        });
}

WebIDL::ExceptionOr<void> CSSPerspective::set_length(CSSPerspectiveValue value)
{
    // AD-HOC: Not specced. https://github.com/w3c/css-houdini-drafts/issues/1153
    //         WPT expects this to throw for invalid values, so just reuse the constructor code.
    auto length = TRY(to_internal(realm(), value));
    m_length = length;
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssperspective-is2d
void CSSPerspective::set_is_2d(bool)
{
    // The is2D attribute of a CSSPerspective object must, on setting, do nothing.
}

WebIDL::ExceptionOr<NonnullRefPtr<TransformationStyleValue const>> CSSPerspective::create_style_value(PropertyNameAndID const& property) const
{
    auto length = TRY(m_length.visit([&](auto const& value) {
        return value->create_an_internal_representation(property, CSSStyleValue::PerformTypeCheck::No);
    }));
    return TransformationStyleValue::create(property.id(), TransformFunction::Perspective, { move(length) });
}

}
