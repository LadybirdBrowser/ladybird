/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/AbstractNonMathCalcFunctionStyleValue.h>

namespace Web::CSS {

class TreeCountingFunctionStyleValue final : public AbstractNonMathCalcFunctionStyleValue {
public:
    enum class TreeCountingFunction : u8 {
        SiblingCount,
        SiblingIndex
    };

    enum class ComputedType : u8 {
        Number,
        Integer
    };

    static ValueComparingNonnullRefPtr<TreeCountingFunctionStyleValue const> create(TreeCountingFunction function, ComputedType computed_type)
    {
        return adopt_ref(*new (nothrow) TreeCountingFunctionStyleValue(function, computed_type));
    }
    virtual ~TreeCountingFunctionStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    size_t resolve(DOM::AbstractElement const&) const;

    virtual RefPtr<CalculationNode const> resolve_to_calculation_node(CalculationContext const&, CalculationResolutionContext const&) const override;
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual bool equals(StyleValue const& other) const override;

private:
    TreeCountingFunctionStyleValue(TreeCountingFunction function, ComputedType computed_type)
        : AbstractNonMathCalcFunctionStyleValue(Type::TreeCountingFunction)
        , m_function(function)
        , m_computed_type(computed_type)
    {
    }

    TreeCountingFunction m_function;
    ComputedType m_computed_type;
};

}
