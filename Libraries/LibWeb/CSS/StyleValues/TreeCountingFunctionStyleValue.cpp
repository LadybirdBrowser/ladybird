/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/StyleValues/IntegerStyleValue.h>
#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/TreeCountingFunctionStyleValue.h>

namespace Web::CSS {

String TreeCountingFunctionStyleValue::to_string(SerializationMode) const
{
    switch (m_function) {
    case TreeCountingFunction::SiblingCount:
        return MUST(String::from_utf8("sibling-count()"sv));
    case TreeCountingFunction::SiblingIndex:
        return MUST(String::from_utf8("sibling-index()"sv));
    }

    VERIFY_NOT_REACHED();
}

ValueComparingNonnullRefPtr<StyleValue const> TreeCountingFunctionStyleValue::absolutized(ComputationContext const& computation_context) const
{
    size_t value;

    switch (m_function) {
    case TreeCountingFunction::SiblingCount:
        value = computation_context.tree_counting_function_resolution_context->sibling_count;
        break;
    case TreeCountingFunction::SiblingIndex:
        value = computation_context.tree_counting_function_resolution_context->sibling_index;
        break;
    }

    switch (m_computed_type) {
    case ComputedType::Integer:
        return IntegerStyleValue::create(value);
    case ComputedType::Number:
        return NumberStyleValue::create(static_cast<double>(value));
    }

    VERIFY_NOT_REACHED();
}

}
