/*
 * Copyright (c) 2025-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/Bindings/OffscreenCanvasRenderingContext2D.h>
#include <LibWeb/HTML/Canvas/Canvas2DContextBase.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#offscreencanvasrenderingcontext2d
class WEB_API OffscreenCanvasRenderingContext2D final : public Canvas2DContextBase {
    WEB_RUST_INTERFACE(OffscreenCanvasRenderingContext2D, offscreen_canvas_rendering_context2d)

public:
    static GC::Ref<OffscreenCanvasRenderingContext2D> create(OffscreenCanvas&, CanvasRenderingContext2DSettings const&);

    void replace_bitmap_with_cleared_bitmap();
};

}
