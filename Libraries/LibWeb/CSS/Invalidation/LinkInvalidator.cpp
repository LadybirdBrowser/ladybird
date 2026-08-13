/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/LinkInvalidator.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/Selector.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_hyperlink_state_change(DOM::Element& element)
{
    if (!element.is_connected())
        return;

    // Whether the element is a link at all is what moved, and every link pseudo-class is derived
    // from it, so each one is published with the value it now holds.
    auto is_link = element.matches_link_pseudo_class();
    auto is_visited = element.matches_visited_pseudo_class();
    record_element_state_changed(element, PseudoClass::Link, is_link);
    record_element_state_changed(element, PseudoClass::AnyLink, is_link || is_visited);
    record_element_state_changed(element, PseudoClass::LocalLink, element.matches_local_link_pseudo_class());
    record_element_state_changed(element, PseudoClass::Visited, is_visited);
}

void invalidate_style_after_legacy_link_color_change(DOM::Document& document)
{
    document.for_each_shadow_including_inclusive_descendant([&](DOM::Node& node) {
        auto* element = as_if<DOM::Element>(node);
        if (!element || (!element->matches_link_pseudo_class() && !element->matches_visited_pseudo_class()))
            return TraversalDecision::Continue;

        document.style_computer().style_engine().record_element_style_input_change(element->style_node_id());
        return TraversalDecision::Continue;
    });
}

}
