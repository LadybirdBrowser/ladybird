/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16String.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PaintTiming/DocumentPaintTimingState.h>
#include <LibWeb/PaintTiming/PaintTimingMixin.h>
#include <LibWeb/Painting/BoxViews.h>

namespace Web::PaintTiming {

// https://w3c.github.io/paint-timing/#process-an-image-that-finished-loading
void process_an_image_that_finished_loading(DOM::Element&, ImageRequestInfo);

ImageRequestInfo image_request_info(Utf16String url, GC::Ptr<HTML::DecodedImageData const>);

// NB: Lets paint timing know of a connected text node that may be painted for the first time, as it was inserted or
//     its data changed from white space to something else.
void text_node_may_be_painted_for_the_first_time(DOM::Text&);

// https://w3c.github.io/paint-timing/#mark-paint-timing
void mark_paint_timing(DOM::Document&);

// https://w3c.github.io/paint-timing/#report-paint-timing
void report_paint_timing(DOM::Document&, Utf16String const& paint_type, PaintTimingInfo const&);

// NB: The client rect of the given box, which for a text box is the union of the rects of its fragments.
CSSPixelRect client_rect_of_box(Layout::Node const&, Layout::RustFFI::FfiRectToViewportTransform const&);

// https://w3c.github.io/paint-timing/#exposed-for-paint-timing
bool is_exposed_for_paint_timing(DOM::Element const&, GC::Ptr<DOM::Document const>);

}
