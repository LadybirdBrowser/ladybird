/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSMathValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathnegate
class CSSMathNegate final : public CSSMathValue {
    WEB_WRAPPABLE(CSSMathNegate, CSSMathValue);
    GC_DECLARE_ALLOCATOR(CSSMathNegate);

public:
    [[nodiscard]] static GC::Ref<CSSMathNegate> create(NumericType, GC::Ref<CSSNumericValue>);
    static GC::Ref<CSSMathNegate> construct_impl(CSSNumberish);

    virtual ~CSSMathNegate() override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<CSSNumericValue> value() const;

    virtual void serialize_math_value(StringBuilder&, Nested, Parens) const override;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const override;
    virtual Optional<SumValue> create_a_sum_value() const override;

    virtual WebIDL::ExceptionOr<NonnullRefPtr<CalculationNode const>> create_calculation_node(CalculationContext const&) const override;

private:
    CSSMathNegate(NumericType, GC::Ref<CSSNumericValue>);
    GC::Ref<CSSNumericValue> m_value;
};

}
