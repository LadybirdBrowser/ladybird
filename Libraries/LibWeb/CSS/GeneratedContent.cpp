/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/CSS/GeneratedContent.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>

namespace Web::CSS {

static bool style_affects_generated_content_state(ComputedValues const& style)
{
    return style.generated_content_facts() & (ComputedValuesFFI::GENERATED_CONTENT_DEFINES_COUNTERS | ComputedValuesFFI::GENERATED_CONTENT_HAS_QUOTES);
}

bool subtree_affects_generated_content_state(DOM::Node const& node)
{
    auto style_affects_state = [](auto const& style) {
        return style && style_affects_generated_content_state(*style);
    };
    bool affects_generated_content_state = false;
    node.for_each_in_inclusive_subtree([&](DOM::Node const& descendant) {
        auto const* element = as_if<DOM::Element>(descendant);
        if (!element)
            return TraversalDecision::Continue;

        if (!style_affects_state(element->computed_style())
            && !style_affects_state(element->computed_style(PseudoElement::Before))
            && !style_affects_state(element->computed_style(PseudoElement::After))
            && !style_affects_state(element->computed_style(PseudoElement::Marker))) {
            return TraversalDecision::Continue;
        }

        affects_generated_content_state = true;
        return TraversalDecision::Break;
    });
    return affects_generated_content_state;
}

}
