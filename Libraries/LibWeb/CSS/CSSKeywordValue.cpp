/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSKeywordValue.h"
#include <LibWeb/Bindings/CSSKeywordValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSKeywordValue);

GC::Ref<CSSKeywordValue> CSSKeywordValue::create(JS::Realm& realm, FlyString value)
{
    return realm.create<CSSKeywordValue>(realm, move(value));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csskeywordvalue-csskeywordvalue
WebIDL::ExceptionOr<GC::Ref<CSSKeywordValue>> CSSKeywordValue::construct_impl(JS::Realm& realm, FlyString value)
{
    // 1. If value is an empty string, throw a TypeError.
    if (value.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot create a CSSKeywordValue with an empty string as the value"sv };

    // 2. Otherwise, return a new CSSKeywordValue with its value internal slot set to value.
    return CSSKeywordValue::create(realm, move(value));
}

CSSKeywordValue::CSSKeywordValue(JS::Realm& realm, FlyString value)
    : CSSStyleValue(realm)
    , m_value(move(value))
{
}

void CSSKeywordValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSKeywordValue);
    Base::initialize(realm);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-csskeywordvalue-value
WebIDL::ExceptionOr<void> CSSKeywordValue::set_value(FlyString value)
{
    // 1. If value is an empty string, throw a TypeError.
    if (value.is_empty())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "Cannot set CSSKeywordValue.value to an empty string"sv };

    // 2. Otherwise, set this’s value internal slot, to value.
    m_value = move(value);
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#keywordvalue-serialization
WebIDL::ExceptionOr<String> CSSKeywordValue::to_string() const
{
    // To serialize a CSSKeywordValue this:
    // 1. Return this’s value internal slot.
    // AD-HOC: Serialize it as an identifier. Spec issue: https://github.com/w3c/csswg-drafts/issues/12545
    return serialize_an_identifier(m_value);
}

// https://drafts.css-houdini.org/css-typed-om-1/#rectify-a-keywordish-value
GC::Ref<CSSKeywordValue> rectify_a_keywordish_value(JS::Realm& realm, CSSKeywordish const& keywordish)
{
    // To rectify a keywordish value val, perform the following steps:
    return keywordish.visit(
        // 1. If val is a CSSKeywordValue, return val.
        [](GC::Root<CSSKeywordValue> const& value) -> GC::Ref<CSSKeywordValue> {
            return *value;
        },

        // 2. If val is a DOMString, return a new CSSKeywordValue with its value internal slot set to val.
        [&realm](FlyString const& value) -> GC::Ref<CSSKeywordValue> {
            return CSSKeywordValue::create(realm, value);
        });
}

}
