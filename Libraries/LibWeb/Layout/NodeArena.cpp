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

u64 NodeArena::formatting_context_run_cache_hit_count() const
{
    return RustFFI::layout_arena_fc_run_cache_hit_count(m_handle);
}

u64 NodeArena::table_cell_measurement_cache_miss_count() const
{
    return RustFFI::layout_arena_table_cell_measurement_cache_miss_count(m_handle);
}

u64 NodeArena::intrinsic_measurement_count() const
{
    return RustFFI::layout_arena_intrinsic_measurement_count(m_handle);
}

void NodeArena::drop_intrinsic_size_cache(RustFFI::NodeData const& node_data) const
{
    RustFFI::layout_arena_drop_intrinsic_size_cache(m_handle, &node_data);
}

bool detach_layout_node_for_destruction(Node& node)
{
    return RustFFI::layout_arena_detach_node_for_destruction(node.arena_handle(), Node::slot_id(&node));
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
        auto* text_node = weak_text_node.ptr();
        if (!text_node)
            continue;
        if (!text_node->parent()) {
            still_detached_text_nodes.append(move(weak_text_node));
            continue;
        }
        // Changed rendered text invalidates cached formatting-context runs regardless of
        // which channel produced the change, including sources with no invalidation of
        // their own (e.g. lang-keyed locale-sensitive casing).
        if (text_node->sync_text_content_to_arena())
            text_node->bump_fragment_cache_epoch_of_self_and_ancestors();
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
        auto* node = weak_node.ptr();
        if (!node) {
            any_enrolled_node_died = true;
            continue;
        }
        RustFFI::FfiReplacedContentFacts facts {};
        if (auto const* box = as_if<Box>(*node))
            facts = box->build_replaced_content_facts_for_arena();
        // Changed facts invalidate cached formatting-context runs regardless of which
        // channel produced the change, including sources with no invalidation of their own.
        if (RustFFI::layout_arena_set_replaced_content_facts(m_handle, Node::slot_id(node), facts))
            node->bump_fragment_cache_epoch_of_self_and_ancestors();
    }
    if (any_enrolled_node_died)
        m_nodes_enrolled_for_replaced_content_facts_sync.remove_all_matching([](auto& weak_node) { return !weak_node.ptr(); });
}

}
