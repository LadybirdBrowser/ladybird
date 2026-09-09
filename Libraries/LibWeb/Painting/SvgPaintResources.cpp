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

bool sync_svg_paint_resources(DOM::Document& document)
{
    return Layout::RustFFI::layout_arena_sync_svg_paint_resources(
        document.layout_node_arena().handle(),
        [](void* layout_node_shell, void const* url_value, void* sink) -> bool {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            return push_svg_filter_reference(url_value, layout_node, sink);
        },
        [](void* layout_node_shell, bool is_stroke, void* sink) {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            push_svg_paint_server_description(layout_node, is_stroke, sink);
        });
}

}
