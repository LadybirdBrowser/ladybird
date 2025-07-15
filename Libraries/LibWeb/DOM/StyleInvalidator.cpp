/*
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/StyleInvalidator.h>

namespace Web::DOM {

GC_DEFINE_ALLOCATOR(StyleInvalidator);

// This function makes a full pass over the entire DOM and converts "entire subtree needs style update"
// into "needs style update" for each inclusive descendant where it's found.
void StyleInvalidator::perform_pending_style_invalidations(Node& node, bool invalidate_entire_subtree)
{
    invalidate_entire_subtree |= node.entire_subtree_needs_style_update();

    if (invalidate_entire_subtree) {
        node.set_needs_style_update_internal(true);
        if (node.has_child_nodes())
            node.set_child_needs_style_update(true);
    }

    for (auto* child = node.first_child(); child; child = child->next_sibling()) {
        perform_pending_style_invalidations(*child, invalidate_entire_subtree);
    }

    if (node.is_element()) {
        auto& element = static_cast<Element&>(node);
        if (auto shadow_root = element.shadow_root()) {
            perform_pending_style_invalidations(*shadow_root, invalidate_entire_subtree);
            if (invalidate_entire_subtree)
                node.set_child_needs_style_update(true);
        }
    }

    node.set_entire_subtree_needs_style_update(false);
}

}
