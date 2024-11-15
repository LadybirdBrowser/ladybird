/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGC/Ptr.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

ErrorOr<GC::Ref<HTML::HTMLCanvasElement>, WebDriver::Error> draw_bounding_box_from_the_framebuffer(HTML::BrowsingContext&, DOM::Element&, Gfx::IntRect);
Response encode_canvas_element(HTML::HTMLCanvasElement&);

}
