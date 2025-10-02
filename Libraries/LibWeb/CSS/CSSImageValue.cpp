/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSImageValue.h"
#include <LibWeb/Bindings/CSSImageValuePrototype.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/CSS/PropertyNameAndID.h>
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

// https://drafts.css-houdini.org/css-typed-om-1/#create-an-internal-representation
WebIDL::ExceptionOr<NonnullRefPtr<StyleValue const>> CSSImageValue::create_an_internal_representation(PropertyNameAndID const& property) const
{
    // If value is a CSSStyleValue subclass,
    //     If value does not match the grammar of a list-valued property iteration of property, throw a TypeError.
    // NB: https://drafts.css-houdini.org/css-typed-om-1/#cssstylevalue-match-a-grammar doesn't list CSSImageValue, but
    //     we should match <image>.
    bool const matches_grammar = [&] {
        if (property.is_custom_property()) {
            // FIXME: If this is a registered custom property, check if that allows <image>.
            return true;
        }
        return property_accepts_type(property.id(), ValueType::Image);
    }();
    if (!matches_grammar) {
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, MUST(String::formatted("Property '{}' does not accept <image>", property.name())) };
    }

    //     FIXME: If any component of property’s CSS grammar has a limited numeric range, and the corresponding part of value
    //            is a CSSUnitValue that is outside of that range, replace that value with the result of wrapping it in a
    //            fresh CSSMathSum whose values internal slot contains only that part of value.

    //     Return the value.
    return NonnullRefPtr { *source_value() };
}

}
