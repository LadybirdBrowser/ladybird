/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Node.h>
#include <LibWeb/Editing/InlineInsertion.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/ParagraphOperations.h>
#include <LibWeb/HTML/HTMLLIElement.h>
#include <LibWeb/HTML/HTMLOListElement.h>
#include <LibWeb/HTML/HTMLUListElement.h>

namespace Web::Editing {

InlineInsertionBoundary prepare_inline_insertion_boundary(DOM::BoundaryPoint boundary, DOM::Node& containing_block)
{
    VERIFY(boundary.node == &containing_block || containing_block.is_ancestor_of(boundary.node));

    // INTEROP: Blink first advances through preceding inline nodes while the resulting DOM boundary represents the
    //          same caret. Inserting after a styled run then occurs beside that run instead of creating an empty style
    //          clone on its trailing edge.
    while (boundary.node != &containing_block) {
        if (boundary.offset != boundary.node->length() || !boundary.node->parent())
            break;
        auto node = boundary.node;
        boundary = { *node->parent(), static_cast<WebIDL::UnsignedLong>(node->index() + 1) };
    }

    GC::Ptr<DOM::Node> highest_styled_inline_ancestor;
    for (auto* ancestor = boundary.node.ptr(); ancestor && ancestor != &containing_block; ancestor = ancestor->parent()) {
        // INTEROP: Blink leaves list insertion to its dedicated list-item path instead of splitting the list's inline
        //          destination style here.
        if (is<HTML::HTMLLIElement>(*ancestor) || is<HTML::HTMLUListElement>(*ancestor) || is<HTML::HTMLOListElement>(*ancestor))
            return { boundary, false };
        if (is_inline_node(*ancestor) && is_modifiable_element(*ancestor))
            highest_styled_inline_ancestor = *ancestor;
    }
    if (!highest_styled_inline_ancestor || !highest_styled_inline_ancestor->parent())
        return { boundary, false };

    // INTEROP: Blink splits the tree to the parent of the highest styled inline ancestor before rich insertion. This
    //          keeps pasted source styling independent from formatting supplied only by the destination caret.
    boundary = split_inline_ancestors_at_boundary(boundary, *highest_styled_inline_ancestor->parent());
    return { boundary, true };
}

}
