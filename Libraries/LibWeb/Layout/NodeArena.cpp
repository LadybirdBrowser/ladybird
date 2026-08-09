/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
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

void NodeArena::enroll_node_for_replaced_content_facts_sync(Node const& node)
{
    m_nodes_enrolled_for_replaced_content_facts_sync.append(node.make_weak_ptr<Node>());
}

void NodeArena::sync_enrolled_content_for_layout()
{
    if (layout_pass_currently_running())
        return;
    sync_enrolled_text_node_content();
    sync_enrolled_replaced_content_facts();
}

void NodeArena::sync_enrolled_replaced_content_facts()
{
    bool any_enrolled_node_died = false;
    for (auto& weak_node : m_nodes_enrolled_for_replaced_content_facts_sync) {
        auto const* node = weak_node.ptr();
        if (!node) {
            any_enrolled_node_died = true;
            continue;
        }
        RustFFI::FfiReplacedContentFacts facts {};
        if (auto const* box = as_if<Box>(*node))
            facts = box->build_replaced_content_facts_for_arena();
        RustFFI::layout_arena_set_replaced_content_facts(m_handle, Node::slot_id(node), facts);
    }
    if (any_enrolled_node_died)
        m_nodes_enrolled_for_replaced_content_facts_sync.remove_all_matching([](auto& weak_node) { return !weak_node.ptr(); });
}

}
