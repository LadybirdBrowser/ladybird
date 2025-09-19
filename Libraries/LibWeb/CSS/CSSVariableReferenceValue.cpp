/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSVariableReferenceValue.h"
#include <LibWeb/Bindings/CSSVariableReferenceValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/CSSUnparsedValue.h>
#include <LibWeb/CSS/PropertyName.h>
#include <LibWeb/HTML/CustomElements/CustomElementName.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSVariableReferenceValue);
GC::Ref<CSSVariableReferenceValue> CSSVariableReferenceValue::create(JS::Realm& realm, FlyString variable, GC::Ptr<CSSUnparsedValue> fallback)
{
    return realm.create<CSSVariableReferenceValue>(realm, move(variable), move(fallback));
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-cssvariablereferencevalue
WebIDL::ExceptionOr<GC::Ref<CSSVariableReferenceValue>> CSSVariableReferenceValue::construct_impl(JS::Realm& realm, FlyString variable, GC::Ptr<CSSUnparsedValue> fallback)
{
    // The CSSVariableReferenceValue(variable, fallback) constructor must, when called, perform the following steps:
    // 1. If variable is not a custom property name string, throw a TypeError.
    if (!is_a_custom_property_name_string(variable))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS custom property name", variable)) };

    // 2. Return a new CSSVariableReferenceValue with its variable internal slot set to variable and its fallback internal slot set to fallback.
    return CSSVariableReferenceValue::create(realm, move(variable), move(fallback));
}

CSSVariableReferenceValue::CSSVariableReferenceValue(JS::Realm& realm, FlyString variable, GC::Ptr<CSSUnparsedValue> fallback)
    : Bindings::PlatformObject(realm)
    , m_variable(move(variable))
    , m_fallback(move(fallback))
{
}

CSSVariableReferenceValue::~CSSVariableReferenceValue() = default;

void CSSVariableReferenceValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSVariableReferenceValue);
    Base::initialize(realm);
}

void CSSVariableReferenceValue::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_fallback);
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-variable
String CSSVariableReferenceValue::variable() const
{
    // The getter for the variable attribute of a CSSVariableReferenceValue this must return its variable internal slot.
    return m_variable.to_string();
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-variable
WebIDL::ExceptionOr<void> CSSVariableReferenceValue::set_variable(FlyString variable)
{
    // The variable attribute of a CSSVariableReferenceValue this must, on setting a variable variable, perform the following steps:
    // 1. If variable is not a custom property name string, throw a TypeError.
    if (!is_a_custom_property_name_string(variable))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("'{}' is not a valid CSS custom property name", variable)) };

    // 2. Otherwise, set this’s variable internal slot to variable.
    m_variable = move(variable);
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-fallback
GC::Ptr<CSSUnparsedValue> CSSVariableReferenceValue::fallback() const
{
    // AD-HOC: No spec algorithm, see https://github.com/w3c/css-houdini-drafts/issues/1146#issuecomment-3188550133
    return m_fallback;
}

// https://drafts.css-houdini.org/css-typed-om-1/#dom-cssvariablereferencevalue-fallback
WebIDL::ExceptionOr<void> CSSVariableReferenceValue::set_fallback(GC::Ptr<CSSUnparsedValue> fallback)
{
    // AD-HOC: No spec algorithm, see https://github.com/w3c/css-houdini-drafts/issues/1146#issuecomment-3188550133
    m_fallback = move(fallback);
    return {};
}

// https://drafts.css-houdini.org/css-typed-om-1/#serialize-a-cssvariablereferencevalue
WebIDL::ExceptionOr<String> CSSVariableReferenceValue::to_string() const
{
    // To serialize a CSSVariableReferenceValue this:
    // 1. Let s initially be "var(".
    StringBuilder s;
    s.append("var("sv);

    // 2. Append this’s variable internal slot to s.
    s.append(m_variable);

    // 3. If this’s fallback internal slot is not null, append ", " to s, then serialize the fallback internal slot and append it to s.
    if (m_fallback) {
        // AD-HOC: Tested behaviour requires we append "," without the space. https://github.com/w3c/css-houdini-drafts/issues/1148
        s.append(","sv);
        s.append(TRY(m_fallback->to_string()));
    }

    // 4. Append ")" to s and return s.
    s.append(")"sv);
    return s.to_string_without_validation();
}

}
