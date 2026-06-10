/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/FlyString.h>
#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/NumericType.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::Bindings {

struct CSSNumericType;

}

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue-sum-value
struct SumValueItem {
    double value;
    UnitMap unit_map;
};
using SumValue = Vector<SumValueItem>;

// https://drafts.css-houdini.org/css-typed-om-1/#cssnumericvalue
class CSSNumericValue : public CSSStyleValue {
    WEB_WRAPPABLE(CSSNumericValue, CSSStyleValue);
    GC_DECLARE_ALLOCATOR(CSSNumericValue);

public:
    struct SerializationParams {
        Optional<double> minimum {};
        Optional<double> maximum {};
        bool nested { false };
        bool parenless { false };
    };
    virtual ~CSSNumericValue() override = default;

    WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> add(ReadonlySpan<CSSNumberish>);
    WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> sub(ReadonlySpan<CSSNumberish>);
    WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> mul(ReadonlySpan<CSSNumberish>);
    WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> div(ReadonlySpan<CSSNumberish>);
    WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> min(ReadonlySpan<CSSNumberish>);
    WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> max(ReadonlySpan<CSSNumberish>);

    bool equals_for_bindings(ReadonlySpan<CSSNumberish>) const;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const = 0;

    WebIDL::ExceptionOr<GC::Ref<CSSUnitValue>> to(FlyString const& unit) const;
    Bindings::CSSNumericType type_for_bindings() const;

    CSSNumberish negate();
    WebIDL::ExceptionOr<CSSNumberish> invert();

    virtual Optional<SumValue> create_a_sum_value() const = 0;

    NumericType const& type() const { return m_type; }

    virtual WebIDL::ExceptionOr<String> to_string() const final override { return to_string({}); }
    void serialize(StringBuilder&, SerializationParams const&) const;
    String to_string(SerializationParams const&) const;

    static WebIDL::ExceptionOr<GC::Ref<CSSNumericValue>> parse(String const& css_text);

    virtual WebIDL::ExceptionOr<NonnullRefPtr<CalculationNode const>> create_calculation_node(CalculationContext const&) const = 0;

protected:
    explicit CSSNumericValue(NumericType);

    NumericType m_type;
};

GC::Ref<CSSNumericValue> rectify_a_numberish_value(CSSNumberish const&, Optional<FlyString> unit = {});

}
