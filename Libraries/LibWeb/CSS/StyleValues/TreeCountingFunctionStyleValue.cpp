/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "TreeCountingFunctionStyleValue.h"
#include <LibWeb/CSS/StyleValues/CalcNodeRef.h>
#include <LibWeb/CSS/StyleValues/CalculatedStyleValue.h>
#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS {

// The function discriminant crosses the style value FFI as a raw code; the Rust serializer
// depends on it.
static_assert(to_underlying(TreeCountingFunctionStyleValue::TreeCountingFunction::SiblingCount) == 0);
static_assert(to_underlying(TreeCountingFunctionStyleValue::TreeCountingFunction::SiblingIndex) == 1);

size_t TreeCountingFunctionStyleValue::resolve(DOM::AbstractElement const& abstract_element) const
{
    const_cast<DOM::Element&>(abstract_element.element()).set_style_uses_tree_counting_function();

    auto tree_counting_function_resolution_context = abstract_element.tree_counting_function_resolution_context();

    switch (function()) {
    case TreeCountingFunction::SiblingCount:
        return tree_counting_function_resolution_context.sibling_count;
    case TreeCountingFunction::SiblingIndex:
        return tree_counting_function_resolution_context.sibling_index;
    }

    VERIFY_NOT_REACHED();
}

Optional<CalcNodeRef> TreeCountingFunctionStyleValue::resolve_to_calculation_node(CalculationContext const&, CalculationResolutionContext const& calculation_resolution_context) const
{
    if (!calculation_resolution_context.abstract_element.has_value())
        return {};

    return CalcNodeRef::numeric(Number { Number::Type::Number, static_cast<double>(resolve(calculation_resolution_context.abstract_element.value())) });
}

ValueComparingNonnullRefPtr<StyleValue const> TreeCountingFunctionStyleValue::absolutized(ComputationContext const& computation_context) const
{
    // FIXME: We should clamp this value in case it falls outside the valid range for the context it is in
    VERIFY(computation_context.abstract_element.has_value());

    size_t value = resolve(computation_context.abstract_element.value());

    switch (computed_type()) {
    case ComputedType::Integer:
        return IntegerStyleValue::create(value);
    case ComputedType::Number:
        return NumberStyleValue::create(static_cast<double>(value));
    }

    VERIFY_NOT_REACHED();
}

}
