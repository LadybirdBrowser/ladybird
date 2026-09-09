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

namespace Web::Painting {

static bool push_svg_filter_reference(void const* url_value, Layout::NodeWithStyle const& layout_node, void* sink)
{
    auto filter_element = resolve_svg_filter_reference({ .pointer = url_value }, layout_node);
    if (!filter_element)
        return false;
    filter_element->push_primitives(sink);
    return true;
}

bool sync_svg_paint_resources(DOM::Document& document)
{
    return Layout::RustFFI::layout_arena_sync_svg_paint_resources(
        document.layout_node_arena().handle(),
        [](void* layout_node_shell, void const* url_value, void* sink) -> bool {
            auto const& layout_node = *static_cast<Layout::NodeWithStyle const*>(layout_node_shell);
            return push_svg_filter_reference(url_value, layout_node, sink);
        });
}

}
