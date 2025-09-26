/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSStyleValue.h"
#include <LibWeb/Bindings/CSSStyleValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleValue);

GC::Ref<CSSStyleValue> CSSStyleValue::create(JS::Realm& realm, FlyString associated_property, String constructed_from_string)
{
    return realm.create<CSSStyleValue>(realm, move(associated_property), move(constructed_from_string));
}

CSSStyleValue::CSSStyleValue(JS::Realm& realm)
    : PlatformObject(realm)
{
}

CSSStyleValue::CSSStyleValue(JS::Realm& realm, FlyString associated_property, String constructed_from_string)
    : PlatformObject(realm)
    , m_associated_property(move(associated_property))
    , m_constructed_from_string(move(constructed_from_string))
{
}

void CSSStyleValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleValue);
    Base::initialize(realm);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-parse
WebIDL::ExceptionOr<GC::Ref<CSSStyleValue>> CSSStyleValue::parse(JS::VM& vm, FlyString const& property, String css_text)
{
    // The parse(property, cssText) method, when invoked, must parse a CSSStyleValue with property property, cssText
    // cssText, and parseMultiple set to false, and return the result.
    auto result = parse_a_css_style_value(vm, property, css_text, ParseMultiple::No);
    if (result.is_exception())
        return result.release_error();
    return result.value().get<GC::Ref<CSSStyleValue>>();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssstylevalue-parseall
WebIDL::ExceptionOr<GC::RootVector<GC::Ref<CSSStyleValue>>> CSSStyleValue::parse_all(JS::VM& vm, FlyString const& property, String css_text)
{
    // The parseAll(property, cssText) method, when invoked, must parse a CSSStyleValue with property property, cssText
    // cssText, and parseMultiple set to true, and return the result.
    auto result = parse_a_css_style_value(vm, property, css_text, ParseMultiple::Yes);
    if (result.is_exception())
        return result.release_error();
    return result.value().get<GC::RootVector<GC::Ref<CSSStyleValue>>>();
}

// https://drafts.css-houdini.org/css-typed-om-1/#parse-a-cssstylevalue
WebIDL::ExceptionOr<Variant<GC::Ref<CSSStyleValue>, GC::RootVector<GC::Ref<CSSStyleValue>>>> CSSStyleValue::parse_a_css_style_value(JS::VM& vm, FlyString property_name, String css_text, ParseMultiple parse_multiple)
{
    // 1. If property is not a custom property name string, set property to property ASCII lowercased.
    // 2. If property is not a valid CSS property, throw a TypeError.
    auto property = PropertyNameAndID::from_name(property_name);
    if (!property.has_value())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS property", property_name)) };

    // 3. Attempt to parse cssText according to property’s grammar.
    //    If this fails, throw a TypeError.
    //    Otherwise, let whole value be the parsed result.
    auto whole_value = parse_css_value(Parser::ParsingParams {}, css_text, property->id());
    if (!whole_value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Failed to parse '{}' as a value for '{}' property", css_text, property->name())) };

    // FIXME: 4. Subdivide into iterations whole value, according to property, and let values be the result.

    // 5. For each value in values, replace it with the result of reifying value for property.
    GC::RootVector<GC::Ref<CSSStyleValue>> reified_values { vm.heap() };
    reified_values.append(whole_value->reify(*vm.current_realm(), property->name()));

    // 6. If parseMultiple is false, return values[0]. Otherwise, return values.
    if (parse_multiple == ParseMultiple::No)
        return reified_values.take_first();
    return reified_values;
}

// https://drafts.css-houdini.org/css-typed-om-1/#stylevalue-serialization
WebIDL::ExceptionOr<String> CSSStyleValue::to_string() const
{
    // if the value was constructed from a USVString
    if (m_constructed_from_string.has_value()) {
        // the serialization is the USVString from which the value was constructed.
        return m_constructed_from_string.value();
    }
    // otherwise, if the value was constructed using an IDL constructor
    {
        // the serialization is specified in the sections below.
        // NB: This is handled by subclasses overriding this to_string() method.
    }
    // FIXME: otherwise, if the value was extracted from the CSSOM
    {
        // the serialization is specified in §6.7 Serialization from CSSOM Values below.
    }
    return String {};
}

}
