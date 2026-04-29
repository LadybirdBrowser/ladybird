/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/PartInvalidator.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/TraversalDecision.h>

namespace Web::CSS::Invalidation {

void invalidate_style_after_part_attribute_change(DOM::Element& element)
{
    // ::part(...) rules in the outer scope target this element by part name, so the element's computed style must
    // be recomputed when its part tokens change.
    element.set_needs_style_update(true);
}

void invalidate_style_after_exportparts_attribute_change(DOM::Element& element)
{
    // When exportparts changes on a shadow host, elements with part tokens inside its shadow tree may newly become
    // or stop being targets of ::part() rules in the outer scope.
    if (auto shadow_root = element.shadow_root()) {
        shadow_root->for_each_in_subtree_of_type<DOM::Element>([](DOM::Element& element) {
            if (!element.part_names().is_empty())
                element.set_needs_style_update(true);
            return TraversalDecision::Continue;
        });
    }
}

}
