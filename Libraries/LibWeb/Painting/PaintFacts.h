/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::Painting {

enum class StyleHoldsImageValues : u8 {
    No,
    Yes,
};

WEB_API void push_paint_facts_after_style_attach(Layout::NodeWithStyle&, StyleHoldsImageValues);
WEB_API void push_layer_image_paint_facts(Layout::NodeWithStyle const&);
WEB_API void push_form_control_paint_facts(HTML::HTMLInputElement&);
WEB_API void push_canvas_paint_facts(HTML::HTMLCanvasElement const&);
WEB_API void reconcile_navigable_container_paint_facts(DOM::Document const&);
WEB_API bool push_replaced_image_paint_facts(Layout::ImageProvider const&, Layout::Node const&);
WEB_API void push_video_paint_facts(HTML::HTMLVideoElement const&);

}
