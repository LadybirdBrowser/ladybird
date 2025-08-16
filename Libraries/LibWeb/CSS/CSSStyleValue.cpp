/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSStyleValue.h"
#include <LibWeb/Bindings/CSSStyleValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSStyleValue);

GC::Ref<CSSStyleValue> CSSStyleValue::create(JS::Realm& realm, String associated_property, String constructed_from_string)
{
    return realm.create<CSSStyleValue>(realm, move(associated_property), move(constructed_from_string));
}

CSSStyleValue::CSSStyleValue(JS::Realm& realm, String associated_property, String constructed_from_string)
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

// https://drafts.css-houdini.org/css-typed-om-1/#stylevalue-serialization
String CSSStyleValue::to_string() const
{
    // if the value was constructed from a USVString
    if (m_constructed_from_string.has_value()) {
        // the serialization is the USVString from which the value was constructed.
        return m_constructed_from_string.value();
    }
    // FIXME: otherwise, if the value was constructed using an IDL constructor
    {
        // the serialization is specified in the sections below.
    }
    // FIXME: otherwise, if the value was extracted from the CSSOM
    {
        // the serialization is specified in §6.7 Serialization from CSSOM Values below.
    }
    return {};
}

}
