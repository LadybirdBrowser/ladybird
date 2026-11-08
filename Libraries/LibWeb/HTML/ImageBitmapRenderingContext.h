/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/Bindings/ImageBitmapRenderingContext.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/canvas.html#the-imagebitmaprenderingcontext-interface
// The C++ side of an ImageBitmapRenderingContext, which is implemented in Rust
// (Libraries/LibWeb/Bindings/Rust/src/html): what HTMLCanvasElement creates, and what its painting and readback read.
class WEB_API ImageBitmapRenderingContext final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ImageBitmapRenderingContext, image_bitmap_rendering_context)

public:
    // https://html.spec.whatwg.org/multipage/canvas.html#imagebitmaprenderingcontext-creation-algorithm
    static GC::Ref<ImageBitmapRenderingContext> create(HTMLCanvasElement&, bool alpha);

    // The bitmap data of the output bitmap, which stays valid until the output bitmap changes; null in blank mode, and
    // for an output bitmap without bitmap data.
    Gfx::DecodedImageFrame const* frame() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-imagebitmaprenderingcontext-bitmap-mode
    bool is_blank() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    bool origin_clean() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-imagebitmaprenderingcontext-alpha
    bool alpha() const;
};

}
