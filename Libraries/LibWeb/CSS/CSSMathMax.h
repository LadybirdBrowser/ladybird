/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibJS/Forward.h>
#include <LibWeb/CSS/CSSMathValue.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-typed-om-1/#cssmathmax
class CSSMathMax final : public CSSMathValue {
    WEB_WRAPPABLE(CSSMathMax, CSSMathValue);
    GC_DECLARE_ALLOCATOR(CSSMathMax);

public:
    [[nodiscard]] static GC::Ref<CSSMathMax> create(NumericType, GC::Ref<CSSNumericArray>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathMax>> construct_impl(Vector<CSSNumberish>);
    static WebIDL::ExceptionOr<GC::Ref<CSSMathMax>> add_all_types_into_math_max(GC::RootVector<GC::Ref<CSSNumericValue>> const&);

    virtual ~CSSMathMax() override;
    virtual void visit_edges(GC::Cell::Visitor&) override;

    GC::Ref<CSSNumericArray> values() const;

    virtual void serialize_math_value(StringBuilder&, Nested, Parens) const override;
    virtual bool is_equal_numeric_value(GC::Ref<CSSNumericValue> other) const override;
    virtual Optional<SumValue> create_a_sum_value() const override;

    virtual WebIDL::ExceptionOr<NonnullRefPtr<CalculationNode const>> create_calculation_node(CalculationContext const&) const override;

private:
    CSSMathMax(NumericType, GC::Ref<CSSNumericArray>);
    GC::Ref<CSSNumericArray> m_values;
};

}
