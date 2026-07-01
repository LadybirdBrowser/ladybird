/*
 * Copyright (c) 2026, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/CalculationResolutionContext.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/CustomPropertyRegistration.h>
#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/Parser/ArbitrarySubstitutionFunctions.h>
#include <LibWeb/CSS/Parser/Parser.h>
#include <LibWeb/CSS/Parser/Syntax.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleValues/GuaranteedInvalidStyleValue.h>
#include <LibWeb/CSS/StyleValues/UnresolvedStyleValue.h>
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

NonnullRefPtr<StyleValue const> initial_custom_property_value(Optional<CustomPropertyRegistration const&> registration, DOM::Document const& document)
{
    if (registration.has_value())
        return compute_registered_custom_property_initial_value(document, registration.value());

    // For non-registered properties, the initial value is the guaranteed-invalid value.
    // See: https://drafts.csswg.org/css-variables/#propdef-
    return GuaranteedInvalidStyleValue::create();
}

NonnullRefPtr<StyleValue const> inherited_custom_property_value(Optional<CustomPropertyRegistration const&> registration, AbstractOrHypotheticalElement const& element, Utf16FlyString const& name, ComputedProperties const* computed_style_for_custom_property_resolution, Optional<Parser::GuardedSubstitutionContexts&> guarded_contexts)
{
    if (auto element_to_inherit_style_from = element.element_to_inherit_style_from(); element_to_inherit_style_from.has_value()) {
        if (auto parent_property = element_to_inherit_style_from->get_custom_property(name)) {
            // NB: With normal style computation we know that ancestors' custom properties are already in their
            //     computed form (since style computation happens in tree order).
            if (element.has<DOM::AbstractElement>())
                return parent_property.release_nonnull();

            VERIFY(element.has<HypotheticalElement*>());

            // NB: Unlike with normal style computation - we don't know that parent's values are in their computed forms
            //     when evaluating a custom function - a property may rely on resolving a custom function which in turn
            //     contains a value which inherits a different, not yet computed, custom property's value.

            // FIXME: We probably need to compute this against the declaring element rather than the parent element.
            auto computed_parent_value = element.document().style_computer().compute_value_of_custom_property(computed_style_for_custom_property_resolution, element_to_inherit_style_from.value(), name, guarded_contexts);

            // https://drafts.csswg.org/css-mixins/#resolve-function-styles
            // inherit
            //   Resolves like an inherit() function with the custom property name as its one and only argument.
            // Note: This ensures that a function parameter defaulted to inherit is reinterpreted using the local parameter type.
            auto inherited_value_tokens = computed_parent_value->tokenize();
            if (contains_guaranteed_invalid_value(inherited_value_tokens))
                return GuaranteedInvalidStyleValue::create();

            return UnresolvedStyleValue::create(move(inherited_value_tokens), {});
        }
    }

    return initial_custom_property_value(registration, element.document());
}

}
