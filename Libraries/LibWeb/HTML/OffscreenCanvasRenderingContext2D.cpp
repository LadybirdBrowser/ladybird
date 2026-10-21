/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/OffscreenCanvas.h>
#include <LibWeb/HTML/OffscreenCanvasRenderingContext2D.h>

namespace Web::HTML {

// Defined in Bindings/Rust/src/canvas_2d/exports.rs.
extern "C" void web_canvas_2d_context_replace_bitmap_with_cleared_bitmap(void* context);

GC::Ref<OffscreenCanvasRenderingContext2D> OffscreenCanvasRenderingContext2D::create(OffscreenCanvas& offscreen_canvas, CanvasRenderingContext2DSettings const& context_attributes)
{
    return static_cast<OffscreenCanvasRenderingContext2D&>(Canvas2DContextBase::create(offscreen_canvas, offscreen_canvas, true, context_attributes));
}

void OffscreenCanvasRenderingContext2D::replace_bitmap_with_cleared_bitmap()
{
    web_canvas_2d_context_replace_bitmap_with_cleared_bitmap(this);
}

}
