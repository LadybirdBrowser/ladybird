/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

enum class StyleHoldsImageValues : u8 {
    No,
    Yes,
};

// The kinds of node state a box paints from that a change to its node can leave stale.
enum class PaintFactsFamily : u8 {
    None = 0,
    Canvas = 1 << 0,
    FormControl = 1 << 1,
};

AK_ENUM_BITWISE_OPERATORS(PaintFactsFamily);

WEB_API void push_paint_facts_after_style_attach(Layout::NodeWithStyle&, StyleHoldsImageValues);
WEB_API void push_layer_image_paint_facts(Layout::NodeWithStyle const&);
// These note in the invalidation journal that the element's facts are stale. The journal reads them from the element and
// writes them with apply_paint_facts() when it drains.
WEB_API void push_form_control_paint_facts(HTML::HTMLInputElement&);
WEB_API void push_canvas_paint_facts(HTML::HTMLCanvasElement const&);
// Writes the facts of the given families from the box's node onto the box, where it is a kind of box that paints from
// them.
WEB_API void apply_paint_facts(Layout::Node const&, PaintFactsFamily);
WEB_API void reconcile_navigable_container_paint_facts(DOM::Document const&);
WEB_API bool push_replaced_image_paint_facts(Layout::ImageProvider const&, Layout::Node const&);
WEB_API void push_video_paint_facts(HTML::HTMLVideoElement const&);

}
