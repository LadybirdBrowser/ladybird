/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TreeCountingFunctionStyleValue.h"
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>

namespace Web::CSS {

String TreeCountingFunctionStyleValue::to_string(SerializationMode) const
{
    switch (m_function) {
    case TreeCountingFunction::SiblingCount:
        return "sibling-count()"_string;
    case TreeCountingFunction::SiblingIndex:
        return "sibling-index()"_string;
    }

    VERIFY_NOT_REACHED();
}

size_t TreeCountingFunctionStyleValue::resolve(TreeCountingFunctionResolutionContext const& tree_counting_function_resolution_context, PropertyComputationDependencies& property_computation_dependencies) const
{
    property_computation_dependencies.tree_counting_function = true;

    switch (m_function) {
    case TreeCountingFunction::SiblingCount:
        return tree_counting_function_resolution_context.sibling_count;
    case TreeCountingFunction::SiblingIndex:
        return tree_counting_function_resolution_context.sibling_index;
    }

    VERIFY_NOT_REACHED();
}

RefPtr<CalculationNode const> TreeCountingFunctionStyleValue::resolve_to_calculation_node(CalculationContext const& calculation_context, CalculationResolutionContext const& calculation_resolution_context, PropertyComputationDependencies* property_computation_dependencies) const
{
    if (!calculation_resolution_context.tree_counting_function_resolution_context.has_value())
        return nullptr;

    VERIFY(property_computation_dependencies);

    return NumericCalculationNode::create(Number { Number::Type::Number, static_cast<double>(resolve(calculation_resolution_context.tree_counting_function_resolution_context.value(), *property_computation_dependencies)) }, calculation_context);
}

ValueComparingNonnullRefPtr<StyleValue const> TreeCountingFunctionStyleValue::absolutized(ComputationContext const& computation_context, PropertyComputationDependencies& property_computation_dependencies) const
{
    // FIXME: We should clamp this value in case it falls outside the valid range for the context it is in
    VERIFY(computation_context.tree_counting_function_resolution_context.has_value());

    size_t value = resolve(computation_context.tree_counting_function_resolution_context.value(), property_computation_dependencies);

    switch (m_computed_type) {
    case ComputedType::Integer:
        return IntegerStyleValue::create(value);
    case ComputedType::Number:
        return NumberStyleValue::create(static_cast<double>(value));
    }

    VERIFY_NOT_REACHED();
}

bool TreeCountingFunctionStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;

    auto const& other_tree_counting_function = other.as_tree_counting_function();

    return m_function == other_tree_counting_function.m_function && m_computed_type == other_tree_counting_function.m_computed_type;
}

}
