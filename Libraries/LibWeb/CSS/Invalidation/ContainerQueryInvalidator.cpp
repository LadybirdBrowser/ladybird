/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibWeb/CSS/Invalidation/ContainerQueryInvalidator.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/DOM/Text.h>
#include <LibWeb/HTML/HTMLSlotElement.h>

namespace Web::CSS::Invalidation {

// The children of a node in the flat tree: a host's are its shadow tree's, a slot's are the nodes
// assigned to it, and everything else's are its DOM children. This is the inverse of the walk that
// selects a query container, which is what makes it the right one for finding that container's
// dependents.
static void append_flat_tree_children(DOM::Node& node, Vector<DOM::Node*>& out)
{
    if (auto* element = as_if<DOM::Element>(node)) {
        if (auto shadow_root = element->shadow_root()) {
            for (auto* child = shadow_root->first_child(); child; child = child->next_sibling())
                out.append(child);
            return;
        }
        if (auto* slot = as_if<HTML::HTMLSlotElement>(*element); slot && !slot->assigned_nodes_internal().is_empty()) {
            for (auto const& slottable : slot->assigned_nodes_internal())
                slottable.visit([&](auto const& assigned) { out.append(static_cast<DOM::Node*>(assigned.ptr())); });
            return;
        }
    }

    for (auto* child = node.first_child(); child; child = child->next_sibling())
        out.append(child);
}

void invalidate_descendant_styles_depending_on_size_container_query(DOM::Element& query_container)
{
    // Only an element some size query or container-relative unit resolved against can have a
    // dependent under it, and `container-type` is set far more widely than it is asked about.
    if (!query_container.is_size_query_container())
        return;

    auto& counters = query_container.document().style_invalidation_counters();

    Vector<DOM::Node*> stack;
    append_flat_tree_children(query_container, stack);
    while (!stack.is_empty()) {
        auto* node = stack.take_last();
        if (auto* element = as_if<DOM::Element>(*node)) {
            ++counters.size_query_container_scan_visits;
            if (element->style_depends_on_size_container_query())
                element->set_needs_style_update(true);
        }
        append_flat_tree_children(*node, stack);
    }
}

}
