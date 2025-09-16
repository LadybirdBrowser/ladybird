/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSUnparsedValue.h"
#include <LibWeb/Bindings/CSSUnparsedValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSVariableReferenceValue.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSUnparsedValue);

GC::Ref<CSSUnparsedValue> CSSUnparsedValue::create(JS::Realm& realm, Vector<GCRootCSSUnparsedSegment> value)
{
    // NB: Convert our GC::Roots into GC::Refs.
    Vector<CSSUnparsedSegment> converted_value;
    for (auto const& variant : value) {
        variant.visit(
            [&](GC::Root<CSSVariableReferenceValue> const& it) { converted_value.append(GC::Ref { *it }); },
            [&](String const& it) { converted_value.append(it); });
    }

    return realm.create<CSSUnparsedValue>(realm, move(converted_value));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunparsedvalue-cssunparsedvalue
WebIDL::ExceptionOr<GC::Ref<CSSUnparsedValue>> CSSUnparsedValue::construct_impl(JS::Realm& realm, Vector<GCRootCSSUnparsedSegment> value)
{
    // AD-HOC: There is no spec for this, see https://github.com/w3c/css-houdini-drafts/issues/1146

    return CSSUnparsedValue::create(realm, move(value));
}

CSSUnparsedValue::CSSUnparsedValue(JS::Realm& realm, Vector<CSSUnparsedSegment> value)
    : CSSStyleValue(realm)
    , m_tokens(move(value))
{
    m_legacy_platform_object_flags = LegacyPlatformObjectFlags {
        .supports_indexed_properties = true,
        .has_indexed_property_setter = true,
    };
}

CSSUnparsedValue::~CSSUnparsedValue() = default;

void CSSUnparsedValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSUnparsedValue);
    Base::initialize(realm);
}

void CSSUnparsedValue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    for (auto const& token : m_tokens) {
        if (auto* variable = token.get_pointer<GC::Ref<CSSVariableReferenceValue>>()) {
            visitor.visit(*variable);
        }
    }
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssunparsedvalue-length
WebIDL::UnsignedLong CSSUnparsedValue::length() const
{
    // The length attribute returns the size of the [[tokens]] internal slot.
    return m_tokens.size();
}

// https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-determine-the-value-of-an-indexed-property
Optional<JS::Value> CSSUnparsedValue::item_value(size_t index) const
{
    // To determine the value of an indexed property of a CSSUnparsedValue this and an index n, let tokens be this’s
    // [[tokens]] internal slot, and return tokens[n].
    if (index >= m_tokens.size())
        return {};
    auto value = m_tokens[index];
    return value.visit(
        [&](GC::Ref<CSSVariableReferenceValue> const& variable) -> JS::Value { return variable; },
        [&](String const& string) -> JS::Value { return JS::PrimitiveString::create(vm(), string); });
}

static WebIDL::ExceptionOr<CSSUnparsedSegment> unparsed_segment_from_js_value(JS::VM& vm, JS::Value& value)
{
    if (value.is_object()) {
        if (auto* variable_reference = as_if<CSSVariableReferenceValue>(value.as_object())) {
            return GC::Ref { *variable_reference };
        }
    }
    return TRY(value.to_string(vm));
}

// https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-an-existing-indexed-property
WebIDL::ExceptionOr<void> CSSUnparsedValue::set_value_of_existing_indexed_property(u32 n, JS::Value value)
{
    // To set the value of an existing indexed property of a CSSUnparsedValue this, an index n, and a value new value,
    // let tokens be this’s [[tokens]] internal slot, and set tokens[n] to new value.
    m_tokens[n] = TRY(unparsed_segment_from_js_value(vm(), value));
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#ref-for-dfn-set-the-value-of-a-new-indexed-property
WebIDL::ExceptionOr<void> CSSUnparsedValue::set_value_of_new_indexed_property(u32 n, JS::Value value)
{
    // To set the value of a new indexed property of a CSSUnparsedValue this, an index n, and a value new value,
    // let tokens be this’s [[tokens]] internal slot. If n is not equal to the size of tokens, throw a RangeError.
    // Otherwise, append new value to tokens.
    if (n != m_tokens.size())
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::RangeError, "Index out of range"sv };

    m_tokens.append(TRY(unparsed_segment_from_js_value(vm(), value)));
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssunparsedvalue
WebIDL::ExceptionOr<String> CSSUnparsedValue::to_string() const
{
    // To serialize a CSSUnparsedValue this:
    // 1. Let s initially be the empty string.
    StringBuilder s;

    // 2. For each item in this’s [[tokens]] internal slot:
    for (auto const& item : m_tokens) {
        // FIXME: In order to match the expected test behaviour, this should insert comments, with the same rules as
        //        serialize_a_series_of_component_values(). See https://github.com/w3c/css-houdini-drafts/issues/1148
        TRY(item.visit(
            // 1. If item is a USVString, append it to s.
            [&](String const& string) -> WebIDL::ExceptionOr<void> {
                s.append(string);
                return {};
            },
            // 2. Otherwise, item is a CSSVariableReferenceValue. Serialize it, then append the result to s.
            [&](GC::Ref<CSSVariableReferenceValue> const& variable) -> WebIDL::ExceptionOr<void> {
                s.append(TRY(variable->to_string()));
                return {};
            }));
    }

    // 3. Return s.
    return s.to_string_without_validation();
}

}
