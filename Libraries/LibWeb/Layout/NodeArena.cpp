/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Layout/Box.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

NodeArena::NodeArena()
    : m_handle(RustFFI::layout_arena_create([](void* shell) {
        return as<TextNode>(*static_cast<Node*>(shell)).text_source();
    }))
{
    VERIFY(m_handle);
}

NodeArena::~NodeArena()
{
    RustFFI::layout_arena_destroy(m_handle);
}

RustFFI::NodeSlotId NodeArena::allocate(RustFFI::FfiNodeConstructionFacts const& construction_facts)
{
    return RustFFI::layout_arena_allocate(m_handle, construction_facts);
}

void NodeArena::free_subtree(RustFFI::NodeSlotId root)
{
    RustFFI::layout_arena_free_subtree(m_handle, root);
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

void NodeArena::visit_dom_nodes(GC::Cell::Visitor& visitor) const
{
    RustFFI::layout_arena_visit_dom_nodes(m_handle, &visitor, [](void* visitor_pointer, void* dom_node_pointer) {
        static_cast<GC::Cell::Visitor*>(visitor_pointer)->visit(static_cast<DOM::Node*>(dom_node_pointer));
    });
}

bool destroy_layout_subtree(Node& node)
{
    return RustFFI::layout_arena_detach_and_free_subtree(node.arena_handle(), Node::slot_id(&node));
}

void NodeArena::sync_enrolled_content_for_layout()
{
    if (layout_pass_currently_running())
        return;
    RustFFI::layout_arena_sync_enrolled_content_for_layout(
        m_handle, nullptr,
        [](void*, void* node_shell, RustFFI::FfiReplacedContentFacts* facts) {
            auto const& node = *static_cast<Node const*>(node_shell);
            if (auto const* box = as_if<Box>(node))
                *facts = box->build_replaced_content_facts_for_arena();
        });
}

}
