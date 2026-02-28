/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSTransformValue.h"
#include <LibWeb/Bindings/CSSTransformValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSTransformComponent.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/CSS/StyleValues/TransformationStyleValue.h>
#include <LibWeb/Geometry/DOMMatrix.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSTransformValue);

GC::Ref<CSSTransformValue> CSSTransformValue::create(JS::Realm& realm, Vector<GC::Ref<CSSTransformComponent>> transforms)
{
    return realm.create<CSSTransformValue>(realm, move(transforms));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-csstransformvalue
WebIDL::ExceptionOr<GC::Ref<CSSTransformValue>> CSSTransformValue::construct_impl(JS::Realm& realm, GC::RootVector<GC::Root<CSSTransformComponent>> transforms)
{
    // The CSSTransformValue(transforms) constructor must, when called, perform the following steps:

    // 1. If transforms is empty, throw a TypeError.
    if (transforms.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "CSSTransformValue's transforms list cannot be empty."sv };

    // 2. Return a new CSSTransformValue whose values to iterate over is transforms.
    Vector<GC::Ref<CSSTransformComponent>> converted_transforms;
    converted_transforms.ensure_capacity(transforms.size());
    for (auto const& transform : transforms)
        converted_transforms.append(*transform);
    return CSSTransformValue::create(realm, move(converted_transforms));
}

CSSTransformValue::CSSTransformValue(JS::Realm& realm, Vector<GC::Ref<CSSTransformComponent>> transforms)
    : CSSStyleValue(realm)
    , m_transforms(move(transforms))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
        .has_indexed_property_setter = true,
    };
}

CSSTransformValue::~CSSTransformValue() = default;

void CSSTransformValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSTransformValue);
    Base::initialize(realm);
}

void CSSTransformValue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_transforms);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-length
WebIDL::UnsignedLong CSSTransformValue::length() const
{
    // The length attribute indicates how many transform components are contained within the CSSTransformValue.
    return m_transforms.size();
}

// https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-determine-the-value-of-an-indexed-property%E2%91%A0
Optional<JS::Value> CSSTransformValue::item_value(size_t index) const
{
    // To determine the value of an indexed property of a CSSTransformValue this and an index n, let values be this’s
    // [[values]] internal slot, and return values[n].
    if (index >= m_transforms.size())
        return {};
    return m_transforms[index];
}

static WebIDL::ExceptionOr<GC::Ref<CSSTransformComponent>> transform_component_from_js_value(JS::Value& value)
{
    if (auto transform_component = value.as_if<CSSTransformComponent>())
        return GC::Ref { *transform_component };
    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Value must be a CSSTransformComponent"sv };
}

// https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-an-existing-indexed-property%E2%91%A0
WebIDL::ExceptionOr<void> CSSTransformValue::set_value_of_existing_indexed_property(u32 n, JS::Value new_value)
{
    // To set the value of an existing indexed property of a CSSTransformValue this, an index n, and a value new value,
    // let values be this’s [[values]] internal slot, and set values[n] to new value.
    m_transforms[n] = TRY(transform_component_from_js_value(new_value));
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-a-new-indexed-property①
WebIDL::ExceptionOr<void> CSSTransformValue::set_value_of_new_indexed_property(u32 n, JS::Value new_value)
{
    // To set the value of a new indexed property of a CSSTransformValue this, an index n, and a value new value, let
    // values be this’s [[values]] internal slot. If n is not equal to the size of values, throw a RangeError.
    // Otherwise, append new value to values.
    if (n != m_transforms.size())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Index out of range"sv };

    m_transforms.append(TRY(transform_component_from_js_value(new_value)));
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-is2d
bool CSSTransformValue::is_2d() const
{
    // The is2D attribute of a CSSTransformValue this must, on getting, return true if, for each func in this’s values
    // to iterate over, the func’s is2D attribute would return true; otherwise, the attribute returns false.
    return all_of(m_transforms, [](auto& transform) { return transform->is_2d(); });
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csstransformvalue-tomatrix
WebIDL::ExceptionOr<GC::Ref<Geometry::DOMMatrix>> CSSTransformValue::to_matrix() const
{
    // The toMatrix() method of a CSSTransformValue this must, when called, perform the following steps:

    // 1. Let matrix be a new DOMMatrix, initialized to the identity matrix, with its is2D internal slot set to true.
    auto matrix = Geometry::DOMMatrix::create(realm());

    // 2. For each func in this’s values to iterate over:
    for (auto const& function : m_transforms) {
        // 1. Let funcMatrix be the DOMMatrix returned by calling toMatrix() on func.
        // AD-HOC: This can throw exceptions.
        auto function_matrix = TRY(function->to_matrix());

        // 2. Set matrix to the result of multiplying matrix and the matrix represented by funcMatrix.
        TRY(matrix->multiply_self(*function_matrix));
    }

    // 3. Return matrix.
    return matrix;
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-csstransformvalue
WebIDL::ExceptionOr<String> CSSTransformValue::to_string() const
{
    // 1. Return the result of serializing each item in this’s values to iterate over, then concatenating them
    //    separated by " ".
    StringBuilder builder;
    bool first = true;
    for (auto const& transform : m_transforms) {
        if (!first)
            builder.append(" "sv);
        first = false;
        builder.append(TRY(transform->to_string()));
    }
    return builder.to_string_without_validation();
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> CSSTransformValue::create_an_internal_representation(PropertyNameAndID const& property, PerformTypeCheck perform_type_check) const
{
    // NB: This can become <transform-function> or <transform-list>, and we don't know which is wanted without performing the type checking.
    //     We can worry about that if and when we ever do have a CSSTransformValue that isn't top-level.
    VERIFY(perform_type_check == PerformTypeCheck::Yes);

    // If value is a CSSStyleValue subclass,
    //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
    //
    //     If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of value
    //     is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it in a
    //     fresh CSSMathSum whose values internal slot contains only that part of value.
    //
    //     Return the value.

    // NB: We match <transform-function> if we have 1 transform. We match <transform-list> always.
    if (m_transforms.size() == 1 && property_accepts_type(property.id(), ValueType::TransformFunction))
        return TRY(m_transforms.first()->create_style_value(property));

    if (property_accepts_type(property.id(), ValueType::TransformList)) {
        StyleValueVector transforms;
        transforms.ensure_capacity(m_transforms.size());
        for (auto const transform : m_transforms)
            transforms.unchecked_append(TRY(transform->create_style_value(property)));
        return StyleValueList::create(move(transforms), StyleValueList::Separator::Space);
    }

    return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Property does not accept values of this type."sv };
}

}
