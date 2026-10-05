/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWeb/CSS/StyleComputer.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Node.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Layout/NodeArena.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/PaintingRustBridge.h>

namespace Web::Layout {

NodeArena::NodeArena(RenderDocument& render_document)
    : m_render_document(render_document)
{
    Painting::register_geometry_host(*this);
}

NodeArena::~NodeArena() = default;

void NodeArena::free_subtree(Layout::BegunRead const& read, Compositing::RustFFI::NodeSlotId root)
{
    RustFFI::render_state_drop_subtree(host(), &read, root);
}

Node* NodeArena::node_if_live(Layout::BegunRead const& read, Compositing::RustFFI::NodeSlotId slot) const
{
    return static_cast<Node*>(RustFFI::render_state_node_shell_if_live(host(), &read, slot));
}

u64 NodeArena::table_cell_measurement_cache_miss_count() const
{
    return RustFFI::render_state_layout_counts(host()).table_cell_measurement_cache_misses;
}

u64 NodeArena::intrinsic_measurement_count() const
{
    return RustFFI::render_state_layout_counts(host()).intrinsic_measurements;
}

u64 NodeArena::intrinsic_inline_measurement_count() const
{
    return RustFFI::render_state_layout_counts(host()).intrinsic_inline_measurements;
}

void NodeArena::start_reporting_box_presence(Badge<DOM::Document>)
{
    RustFFI::render_state_set_box_presence_host(host(), this, [](void* context, u32 style_node, u8 bits) {
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
    RustFFI::render_state_clear_box_presence_host(host());
}

void NodeArena::commit_box_presence(DOM::Node& node)
{
    Layout::ForcedReadScope read { node.document() };
    auto const* layout_node = node.unsafe_layout_node(read);
    node.set_box_presence({}, layout_node, layout_node && Painting::has_committed_box(*layout_node));
}

void NodeArena::sync_enrolled_content_for_layout()
{
    RustFFI::render_state_sync_enrolled_content_for_layout(host());
}

}
