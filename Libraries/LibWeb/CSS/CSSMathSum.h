/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSMathValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathsum
class CSSMathSum final : public CSSMathValue {
    WEB_PLATFORM_OBJECT(CSSMathSum, CSSMathValue);
    GC_DECLARE_ALLOCATOR(CSSMathSum);

public:
    [[nodiscard]] static GC::Ref<CSSMathSum> create(JS::Realm&, NumericType, GC::Ref<CSSNumericArray>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathSum>> construct_impl(JS::Realm&, Vector<CSSNumberish>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathSum>> add_all_types_into_math_sum(JS::Realm&, GC::RootVector<GC::Ref<CSSNumericValue>> const&);

    virtual ~CSSMathSum() override;

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(Visitor&) override;

    GC::Ref<CSSNumericArray> values() const;

    virtual void serialize_math_value(StringBuilder&, Nested, Parens) const override;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const override;
    virtual Optional<SumValue> create_a_sum_value() const override;

    virtual WebIDL::ExceptionOr<NonnullRefPtr<CalculationNode const>> create_calculation_node(CalculationContext const&) const override;

private:
    CSSMathSum(JS::Realm&, NumericType, GC::Ref<CSSNumericArray>);
    GC::Ref<CSSNumericArray> m_values;
};

}
