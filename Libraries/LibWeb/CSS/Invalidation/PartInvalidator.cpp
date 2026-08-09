/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/CSS/Invalidation/PartInvalidator.h>
#include <LibWeb/CSS/PseudoClass.h>
#include <LibWeb/CSS/StyleEngineInput.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/TraversalDecision.h>

namespace Web::CSS::Invalidation {

// What an `exportparts` change moves is how far out of the tree each part reaches, which is a fact
// about the elements exposing parts rather than about the host. Nested hosts forward outwards in
// turn, so the whole hosted tree is walked and each of them republishes its own reach.
void invalidate_style_after_exportparts_attribute_change(DOM::Element& element)
{
    auto shadow_root = element.shadow_root();
    if (!shadow_root)
        return;

    shadow_root->for_each_shadow_including_descendant([](DOM::Node& node) {
        if (auto* descendant = as_if<DOM::Element>(node); descendant && !descendant->part_names().is_empty())
            record_element_parts_changed(*descendant);
        return TraversalDecision::Continue;
    });
}

}
