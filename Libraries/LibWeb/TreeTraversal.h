/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/TraversalDecision.h>

namespace Web {

enum class IncludeTraversalRoot {
    No,
    Yes,
};

template<typename T, typename Callback>
TraversalDecision traverse_preorder(T& root, IncludeTraversalRoot include_root, Callback callback)
{
    auto* current = include_root == IncludeTraversalRoot::Yes ? &root : root.first_child_ptr();
    while (current) {
        TraversalDecision decision = callback(*current);
        if (decision == TraversalDecision::Break)
            return TraversalDecision::Break;

        if (decision != TraversalDecision::SkipChildrenAndContinue) {
            if (auto* first_child = current->first_child_ptr()) {
                current = first_child;
                continue;
            }
        }
        if (current == &root)
            break;

        if (auto* next_sibling = current->next_sibling_ptr()) {
            current = next_sibling;
            continue;
        }

        T* ancestor_next_sibling = nullptr;
        do {
            current = current->parent_ptr();
        } while (current != &root && !(ancestor_next_sibling = current->next_sibling_ptr()));
        if (current == &root)
            break;

        current = ancestor_next_sibling;
    }
    return TraversalDecision::Continue;
}

}
