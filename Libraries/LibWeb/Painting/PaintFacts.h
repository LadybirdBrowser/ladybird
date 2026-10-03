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
    LayerImages = 1 << 2,
    // The box's style holds no image, so it has no layer image facts. Noting this replaces a pending LayerImages, as
    // noting LayerImages replaces this.
    NoLayerImages = 1 << 3,
    ReplacedImage = 1 << 4,
    Video = 1 << 5,
};

AK_ENUM_BITWISE_OPERATORS(PaintFactsFamily);

// These note in the invalidation journal that the facts are stale. The journal reads them from the box and its node and
// writes them with apply_paint_facts() when it drains. A box the journal cannot name takes them at once.
WEB_API void push_paint_facts_after_style_attach(Layout::NodeWithStyle&, StyleHoldsImageValues);
WEB_API void push_video_paint_facts(HTML::HTMLVideoElement const&);
WEB_API void push_form_control_paint_facts(HTML::HTMLInputElement&);
WEB_API void push_canvas_paint_facts(HTML::HTMLCanvasElement const&);
// An image a box shows changed, in a task of its own that may run beside a frame in flight. The box is named by its
// layout node, or by the element whose image it shows, and asked nothing until the journal drains.
WEB_API void push_layer_image_paint_facts(Layout::NodeWithStyle&);
WEB_API void push_replaced_image_paint_facts(Layout::Node&);
WEB_API void push_replaced_image_paint_facts(DOM::Element const&);
// Writes the facts of the given families from the box and its node onto the box, where it is a kind of box that paints
// from them. Replaced image and video facts that changed repaint the box.
WEB_API void apply_paint_facts(Layout::Node const&, PaintFactsFamily);
WEB_API void reconcile_navigable_container_paint_facts(Layout::BegunRead const&, DOM::Document const&);
WEB_API void publish_image_map_area_facts_if_needed(Layout::BegunRead const&, DOM::Document&);

}
