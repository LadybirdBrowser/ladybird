/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSImageValue.h"
#include <LibWeb/Bindings/CSSImageValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::CSS {

GC_DEFINE_ALLOCATOR(CSSImageValue);

GC::Ref<CSSImageValue> CSSImageValue::create(JS::Realm& realm, NonnullRefPtr<StyleValue const> source_value)
{
    return realm.create<CSSImageValue>(realm, move(source_value));
}

CSSImageValue::CSSImageValue(JS::Realm& realm, NonnullRefPtr<StyleValue const> source_value)
    : CSSStyleValue(realm, move(source_value))
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
    //         We just rely on CSSStyleValue serializing its held StyleValue.
    return Base::to_string();
}

}
