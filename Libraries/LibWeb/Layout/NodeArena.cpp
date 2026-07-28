/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

NodeArena::NodeArena()
    : m_handle(RustFFI::layout_arena_create())
{
    VERIFY(m_handle);
}

NodeArena::~NodeArena()
{
    RustFFI::layout_arena_destroy(m_handle);
}

RustFFI::NodeAllocation NodeArena::allocate()
{
    auto allocation = RustFFI::layout_arena_allocate(m_handle);
    VERIFY(allocation.data);
    return allocation;
}

void NodeArena::free(RustFFI::NodeSlotId slot, u32 generation)
{
    RustFFI::layout_arena_free(m_handle, slot, generation);
}

void NodeArena::enroll_text_node_for_content_sync(TextNode const& text_node)
{
    m_text_nodes_enrolled_for_content_sync.append(text_node.make_weak_ptr<TextNode>());
}

void NodeArena::sync_enrolled_text_node_content()
{
    if (m_text_nodes_enrolled_for_content_sync.is_empty())
        return;
    // A node that is alive but detached keeps its enrollment: it cannot
    // resolve style-dependent text without a parent, and it may be reinserted
    // by a later tree update without another enrollment trigger.
    Vector<WeakPtr<TextNode>> still_detached_text_nodes;
    for (auto& weak_text_node : m_text_nodes_enrolled_for_content_sync) {
        auto const* text_node = weak_text_node.ptr();
        if (!text_node)
            continue;
        if (!text_node->parent()) {
            still_detached_text_nodes.append(move(weak_text_node));
            continue;
        }
        text_node->sync_text_content_to_arena();
    }
    m_text_nodes_enrolled_for_content_sync = move(still_detached_text_nodes);
}

}
