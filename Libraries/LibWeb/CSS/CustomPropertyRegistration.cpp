/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/DOM/Document.h>

namespace Web::CSS {

// https://drafts.css-houdini.org/css-properties-values-api/#calculation-of-computed-values
NonnullRefPtr<StyleValue const> compute_registered_custom_property_value(CustomPropertyRegistration const& registration, NonnullRefPtr<StyleValue const> value, ComputationContext const& computation_context)
{
    // If the registration’s syntax is the universal syntax definition, the computed value is the same as for
    // unregistered custom properties (either the specified value with variables substituted, or the guaranteed-invalid
    // value).
    if (registration.syntax->type() == Parser::SyntaxNode::NodeType::Universal)
        return value;

    // Otherwise...
    // NB: Our regular computed-value computation already behaves how this wants.
    return value->absolutized(computation_context);
}

NonnullRefPtr<StyleValue const> compute_registered_custom_property_initial_value(DOM::Document const& document, CustomPropertyRegistration const& registration)
{
    if (registration.computed_initial_value)
        return *registration.computed_initial_value;

    NonnullRefPtr<StyleValue const> computed_initial_value = GuaranteedInvalidStyleValue::create();
    if (registration.initial_value) {
        ComputationContext computation_context {
            .length_resolution_context = Length::ResolutionContext::for_document(document),
        };
        computed_initial_value = compute_registered_custom_property_value(registration, *registration.initial_value, computation_context);
    }

    const_cast<CustomPropertyRegistration&>(registration).computed_initial_value = computed_initial_value;
    return computed_initial_value;
}

}
