/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Bindings/CSSNumericValue.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue
class WEB_API CSSNumericValue : public CSSStyleValue {
    WEB_RUST_INTERFACE(CSSNumericValue, css_numeric_value)

public:
    // A new CSSMathSum of the values, whose types can be added.
    static GC::Ref<CSSNumericValue> create_sum(GC::Ref<CSSNumericValue>, GC::Ref<CSSNumericValue>);

    // https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-type
    NumericType type() const;

    // https://drafts.css-houdini.org/css-typed-om-1/#numeric-value-serialization
    Utf16String to_string() const;

    Optional<CalcNodeRef> create_calculation_node() const;
};

}
