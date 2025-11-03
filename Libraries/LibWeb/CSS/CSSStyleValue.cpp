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
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValueList.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleValue);

GC::Ref<CSSStyleValue> CSSStyleValue::create(JS::Realm& realm, FlyString associated_property, NonnullRefPtr<StyleValue const> source_value)
{
    return realm.create<CSSStyleValue>(realm, move(associated_property), move(source_value));
}

CSSStyleValue::CSSStyleValue(JS::Realm& realm)
    : PlatformObject(realm)
{
}

CSSStyleValue::CSSStyleValue(JS::Realm& realm, NonnullRefPtr<StyleValue const> source_value)
    : PlatformObject(realm)
    , m_source_value(move(source_value))
{
}

CSSStyleValue::CSSStyleValue(JS::Realm& realm, FlyString associated_property, NonnullRefPtr<StyleValue const> source_value)
    : PlatformObject(realm)
    , m_associated_property(move(associated_property))
    , m_source_value(move(source_value))
{
}

void CSSStyleValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSStyleValue);
    Base::initialize(realm);
}

CSSStyleValue::~CSSStyleValue() = default;

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

    // 4. Subdivide into iterations whole value, according to property, and let values be the result.
    auto values = whole_value->subdivide_into_iterations(property.value());

    // 5. For each value in values, replace it with the result of reifying value for property.
    GC::RootVector<GC::Ref<CSSStyleValue>> reified_values { vm.heap() };
    for (auto const& value : values) {
        reified_values.append(value->reify(*vm.current_realm(), property->name()));
    }

    // 6. If parseMultiple is false, return values[0]. Otherwise, return values.
    // FIXME: We need to somehow store the source css_text on the returned CSSStyleValue.
    //        https://github.com/w3c/css-houdini-drafts/issues/1156
    if (parse_multiple == ParseMultiple::No)
        return reified_values.take_first();
    return reified_values;
}

// https://drafts.css-houdini.org/css-typed-om-1/#stylevalue-serialization
WebIDL::ExceptionOr<String> CSSStyleValue::to_string() const
{
    // FIXME: if the value was constructed from a USVString
    // NB: Basically, if this was constructed with "parse a CSSStyleValue", regardless of what CSSStyleValue type it is now.
    {
        // the serialization is the USVString from which the value was constructed.
    }
    // otherwise, if the value was constructed using an IDL constructor
    {
        // the serialization is specified in the sections below.
        // NB: This is handled by subclasses overriding this to_string() method.
    }
    // FIXME: otherwise, if the value was extracted from the CSSOM
    // NB: For CSSStyleValue itself, we use the source value we were created from.
    if (m_source_value)
        return m_source_value->to_string(SerializationMode::Normal);
    {
        // the serialization is specified in §6.7 Serialization from CSSOM Values below.
    }
    return String {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> CSSStyleValue::create_an_internal_representation(PropertyNameAndID const&, PerformTypeCheck) const
{
    // If value is a direct CSSStyleValue,
    //     Return value’s associated value.
    if (!m_source_value)
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Missing {}::create_an_internal_representation() overload", class_name())) };
    return NonnullRefPtr { *m_source_value };
}

}
