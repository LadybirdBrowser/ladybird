/*
 * Copyright (c) 2026-present, the Ladybird developers
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/DOM/Element.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/TraversalDecision.h>

namespace Web::CSS::Invalidation {

enum class AncestorTraversal {
    ShadowIncluding,
    FlatTree,
};

template<typename Callback>
void for_each_inclusive_ancestor_element(DOM::Node& start, AncestorTraversal traversal, Callback callback)
{
    if (auto* element = as_if<DOM::Element>(start)) {
        if (callback(*element) == TraversalDecision::Break)
            return;
    }

    auto* ancestor = traversal == AncestorTraversal::FlatTree
        ? start.flat_tree_parent_element()
        : start.parent_or_shadow_host_element();
    for (; ancestor; ancestor = traversal == AncestorTraversal::FlatTree ? ancestor->flat_tree_parent_element() : ancestor->parent_or_shadow_host_element()) {
        if (callback(*ancestor) == TraversalDecision::Break)
            return;
    }
}

}
