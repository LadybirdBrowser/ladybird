/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/AbstractNonMathCalcFunctionStyleValue.h>
#include <LibWeb/CSS/StyleValues/RustStyleValueHandle.h>

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

    virtual bool is_computationally_independent() const override { return false; }

private:
    TreeCountingFunction function() const { return static_cast<TreeCountingFunction>(m_value->tree_counting_function.function); }
    ComputedType computed_type() const { return static_cast<ComputedType>(m_value->tree_counting_function.computed_type); }

    TreeCountingFunctionStyleValue(TreeCountingFunction function, ComputedType computed_type)
        : AbstractNonMathCalcFunctionStyleValue(Type::TreeCountingFunction)
        , m_value(StyleValueFFI::rust_style_value_create_tree_counting_function(to_underlying(function), to_underlying(computed_type)))
    {
    }

    RustStyleValueHandle m_value;
};

}
