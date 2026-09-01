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

bool detach_layout_node_for_destruction(Node& node)
{
    return RustFFI::layout_arena_detach_node_for_destruction(node.arena_handle(), Node::slot_id(&node));
}

void NodeArena::sync_enrolled_content_for_layout()
{
    if (layout_pass_currently_running())
        return;
    RustFFI::layout_arena_sync_enrolled_content_for_layout(
        m_handle, nullptr,
        [](void*, void* text_node_shell) -> bool {
            return static_cast<TextNode const&>(*static_cast<Node const*>(text_node_shell)).sync_text_content_to_arena();
        },
        [](void*, void* node_shell, RustFFI::FfiReplacedContentFacts* facts) {
            auto const& node = *static_cast<Node const*>(node_shell);
            if (auto const* box = as_if<Box>(node))
                *facts = box->build_replaced_content_facts_for_arena();
        });
}

}
