/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Text.h>
#include <LibWeb/Editing/EditCommand.h>
#include <LibWeb/Editing/Internal/Algorithms.h>
#include <LibWeb/Editing/ParagraphOperations.h>

namespace Web::Editing {

static DOM::BoundaryPoint boundary_outside_text(DOM::BoundaryPoint boundary)
{
    auto* text = as_if<DOM::Text>(*boundary.node);
    if (!text)
        return boundary;

    auto parent = GC::Ref { *text->parent() };
    if (boundary.offset == 0)
        return { parent, static_cast<WebIDL::UnsignedLong>(text->index()) };
    if (boundary.offset == text->length_in_utf16_code_units())
        return { parent, static_cast<WebIDL::UnsignedLong>(text->index() + 1) };

    auto right_text = MUST(split_text(*text, boundary.offset));
    return { parent, static_cast<WebIDL::UnsignedLong>(right_text->index()) };
}

DOM::BoundaryPoint split_inline_ancestors_at_boundary(DOM::BoundaryPoint boundary, GC::Ref<DOM::Node> containing_block)
{
    VERIFY(boundary.node == containing_block || containing_block->is_ancestor_of(boundary.node));
    boundary = boundary_outside_text(boundary);

    // INTEROP: Blink and WebKit split each inline ancestor at a paragraph-movement boundary. Keeping wrappers on
    //          both sides preserves the inline style of the content without cloning the containing block prematurely.
    while (boundary.node != containing_block) {
        auto container = boundary.node;
        auto parent = GC::Ref { *container->parent() };

        // A boundary at either edge already has an equivalent representation outside the inline ancestor. Hoist it
        // instead of manufacturing an empty clone which replacement completion would immediately remove.
        if (boundary.offset == 0) {
            boundary = { parent, static_cast<WebIDL::UnsignedLong>(container->index()) };
            continue;
        }
        if (boundary.offset == container->child_count()) {
            boundary = { parent, static_cast<WebIDL::UnsignedLong>(container->index() + 1) };
            continue;
        }

        auto container_clone = MUST(clone_node_for_editing(container, false));
        insert_node_before(container_clone, parent, container->next_sibling());

        while (boundary.offset < container->child_count())
            move_node_preserving_ranges(*container->child_at_index(boundary.offset), container_clone, container_clone->child_count());

        boundary = { parent, static_cast<WebIDL::UnsignedLong>(container_clone->index()) };
    }

    return boundary;
}

GC::Ref<DOM::Node> split_containing_block_at_boundary(DOM::BoundaryPoint boundary, GC::Ref<DOM::Node> containing_block)
{
    boundary = split_inline_ancestors_at_boundary(boundary, containing_block);

    auto right_block = MUST(clone_node_for_editing(containing_block, false));
    insert_node_before(right_block, *containing_block->parent(), containing_block->next_sibling());
    while (boundary.offset < containing_block->child_count())
        move_node_preserving_ranges(*containing_block->child_at_index(boundary.offset), right_block, right_block->child_count());
    return right_block;
}

}
