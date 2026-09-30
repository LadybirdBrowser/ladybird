/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Layout {

NodeArena::NodeArena()
    : m_handle(RustFFI::layout_arena_create([](void* shell) {
        return as<TextNode>(*static_cast<Node*>(shell)).text_source();
    }))
{
    VERIFY(m_handle);
    Painting::register_geometry_host(*this);
}

NodeArena::~NodeArena()
{
    RustFFI::layout_arena_destroy(m_handle);
}

Compositing::RustFFI::NodeSlotId NodeArena::allocate(RustFFI::FfiNodeConstructionFacts const& construction_facts)
{
    return RustFFI::layout_arena_allocate(m_handle, construction_facts);
}

void NodeArena::free_subtree(Compositing::RustFFI::NodeSlotId root)
{
    RustFFI::layout_arena_free_subtree(m_handle, root);
}

Node* NodeArena::node_if_live(Compositing::RustFFI::NodeSlotId slot) const
{
    return static_cast<Node*>(RustFFI::layout_arena_node_shell_if_live(m_handle, slot));
}

u64 NodeArena::table_cell_measurement_cache_miss_count() const
{
    return RustFFI::layout_arena_table_cell_measurement_cache_miss_count(m_handle);
}

u64 NodeArena::intrinsic_measurement_count() const
{
    return RustFFI::layout_arena_intrinsic_measurement_count(m_handle);
}

u64 NodeArena::intrinsic_inline_measurement_count() const
{
    return RustFFI::layout_arena_intrinsic_inline_measurement_count(m_handle);
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

void NodeArena::start_reporting_box_presence(Badge<DOM::Document>)
{
    RustFFI::layout_arena_set_box_presence_host(m_handle, this, [](void* context, u32 style_node, u8 bits) {
        auto& document = *static_cast<NodeArena*>(context)->m_document;
        // The document has no StyleNodeID; it is named by 0.
        GC::Ptr<DOM::Node> node = &document;
        if (style_node != 0)
            node = document.style_computer().node_for_style_node(CSS::StyleNodeID { style_node });
        if (node)
            node->set_box_presence({}, bits & RustFFI::BOX_PRESENCE_HAS_LAYOUT_BOX, bits & RustFFI::BOX_PRESENCE_HAS_COMMITTED_BOX);
    });
}

void NodeArena::stop_reporting_box_presence(Badge<DOM::Document>)
{
    RustFFI::layout_arena_clear_box_presence_host(m_handle);
}

void NodeArena::commit_box_presence(DOM::Node& node)
{
    auto const* layout_node = node.unsafe_layout_node();
    node.set_box_presence({}, layout_node, layout_node && Painting::has_committed_box(*layout_node));
}

void NodeArena::sync_enrolled_content_for_layout()
{
    RustFFI::layout_arena_sync_enrolled_content_for_layout(m_handle);
}

}
