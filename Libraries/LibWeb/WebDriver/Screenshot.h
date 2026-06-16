/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibGfx/Rect.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/WebDriver/Response.h>

namespace Web::WebDriver {

using DrawBoundingBoxCallback = Function<void(ErrorOr<GC::Root<HTML::HTMLCanvasElement>, WebDriver::Error>)>;

WEB_API void draw_bounding_box_from_the_framebuffer(HTML::BrowsingContext&, DOM::Element&, Gfx::IntRect, DrawBoundingBoxCallback&&);
WEB_API Response encode_canvas_element(HTML::HTMLCanvasElement&);

}
