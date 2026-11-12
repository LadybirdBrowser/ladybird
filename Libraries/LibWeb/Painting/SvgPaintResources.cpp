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

bool push_svg_filter_reference(Layout::BegunRead const& read, DOM::Document& document, Compositing::RustFFI::NodeSlotId slot, void const* url_value, void* sink)
{
    auto const& layout_node = as<Layout::NodeWithStyle>(*document.layout_node_arena().node_if_live(read, slot));
    auto filter_element = resolve_svg_filter_reference({ .pointer = url_value }, layout_node);
    if (!filter_element)
        return false;
    auto dest_size = Gfx::enclosing_int_rect(absolute_rect(layout_node).to_type<float>()).size();
    filter_element->push_primitives(sink, dest_size);
    return true;
}

void push_svg_paint_server_description(Layout::BegunRead const& read, DOM::Document& document, Compositing::RustFFI::NodeSlotId slot, bool is_stroke, void* sink)
{
    auto const& layout_node = as<Layout::NodeWithStyle>(*document.layout_node_arena().node_if_live(read, slot));
    auto const* graphics_element = as_if<SVG::SVGGraphicsElement>(layout_node.dom_node());
    if (!graphics_element)
        return;
    auto url = is_stroke ? layout_node.stroke_url() : layout_node.fill_url();
    if (!url.has_value())
        return;
    auto paint_server_element = graphics_element->resolve_url_to_element(*url);
    if (auto const* gradient = as_if<SVG::SVGGradientElement>(paint_server_element.ptr()))
        gradient->push_paint_server_description(sink);
    else if (auto const* pattern = as_if<SVG::SVGPatternElement>(paint_server_element.ptr()))
        pattern->push_paint_server_description(sink, layout_node);
}

}
