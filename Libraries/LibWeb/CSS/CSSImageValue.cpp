/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSImageValue.h"
#include <LibWeb/Bindings/CSSImageValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSImageValue);

GC::Ref<CSSImageValue> CSSImageValue::create(JS::Realm& realm, String constructed_from_string)
{
    return realm.create<CSSImageValue>(realm, move(constructed_from_string));
}

CSSImageValue::CSSImageValue(JS::Realm& realm, String constructed_from_string)
    : CSSStyleValue(realm)
    , m_constructed_from_string(move(constructed_from_string))
{
}

void CSSImageValue::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(CSSImageValue);
    Base::initialize(realm);
}

// https://drafts.css-houdini.org/css-typed-om-1/#stylevalue-serialization
WebIDL::ExceptionOr<String> CSSImageValue::to_string() const
{
    // AD-HOC: The spec doesn't say how to serialize this, as it's intentionally a black box.
    //         We just serialize the source string that was used to construct this.
    return m_constructed_from_string;
}

}
