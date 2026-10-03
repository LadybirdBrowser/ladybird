/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/Layout/LayoutRustBridge.h>
#include <LibWeb/Layout/Node.h>
#include <LibWeb/Painting/BoxViews.h>
#include <LibWeb/Painting/SvgPaintResources.h>
#include <LibWeb/SVG/SVGFilterElement.h>
#include <LibWeb/SVG/SVGGradientElement.h>
#include <LibWeb/SVG/SVGGraphicsElement.h>
#include <LibWeb/SVG/SVGPatternElement.h>

namespace Web::Painting {

static bool push_svg_filter_reference(void const* url_value, Layout::NodeWithStyle const& layout_node, void* sink)
{
    auto filter_element = resolve_svg_filter_reference({ .pointer = url_value }, layout_node);
    if (!filter_element)
        return false;
    filter_element->push_primitives(sink);
    return true;
}

static void push_svg_paint_server_description(Layout::NodeWithStyle const& layout_node, bool is_stroke, void* sink)
{
    auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(layout_node.dom_node());
    if (!graphics_element)
        return;
    auto paint_server_element = graphics_element->paint_server_element(is_stroke ? layout_node.stroke() : layout_node.fill());
    if (auto const* gradient = as_if<SVG::SVGGradientElement>(paint_server_element.ptr()))
        gradient->push_paint_server_description(sink);
    else if (auto const* pattern = as_if<SVG::SVGPatternElement>(paint_server_element.ptr()))
        pattern->push_paint_server_description(sink, layout_node);
}

bool sync_svg_paint_resources(Layout::BegunRead const& read, DOM::Document& document)
{
    // The boxes the resources are drawn from are found in the read the sync is made in.
    struct Context {
        Layout::NodeArena& arena;
        Layout::BegunRead const& read;
    } context { document.layout_node_arena(), read };
    return Layout::RustFFI::render_state_sync_svg_paint_resources(
        context.arena.host(),
        &read, &context,
        [](void* context_pointer, Compositing::RustFFI::NodeSlotId slot, void const* url_value, void* sink) -> bool {
            auto& context = *static_cast<Context*>(context_pointer);
            auto const& layout_node = as<Layout::NodeWithStyle>(*context.arena.node_if_live(context.read, slot));
            return push_svg_filter_reference(url_value, layout_node, sink);
        },
        [](void* context_pointer, Compositing::RustFFI::NodeSlotId slot, bool is_stroke, void* sink) {
            auto& context = *static_cast<Context*>(context_pointer);
            auto const& layout_node = as<Layout::NodeWithStyle>(*context.arena.node_if_live(context.read, slot));
            push_svg_paint_server_description(layout_node, is_stroke, sink);
        });
}

}
