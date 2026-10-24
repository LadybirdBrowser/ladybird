/*
 * Copyright (c) 2020-2024, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2022, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibCompositing/DisplayList/DisplayListResourceIds.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibGfx/Size.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Canvas/CanvasSettings.h>
#include <LibWebCommon/Bindings/PredefinedColorSpace.h>

namespace Web::HTML {

// The C++ side of a CanvasRenderingContext2D or an OffscreenCanvasRenderingContext2D, which are implemented in Rust
// (Libraries/LibWeb/Bindings/Rust/src/canvas_2d and the modules of their IDL files in .../src/html): what the rest of
// LibWeb calls, and what the Rust code calls back.
class WEB_API Canvas2DContextBase : public Bindings::RustInterfaceCell {
public:
    CanvasHost& canvas_host() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    bool origin_clean() const;

    void set_size(Gfx::IntSize const&);
    void reset_to_default_state();
    void prepare_for_compositing();

    void ensure_backing_storage();
    void discard_backing_storage();

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-context-lost
    void notify_backing_storage_lost();

    Optional<Compositing::CanvasId> canvas_id() const;

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-color-space
    Bindings::PredefinedColorSpace color_space() const;

    // The pixels of the rectangle of the output bitmap, converted to the color space.
    RefPtr<Gfx::Bitmap> read_pixels(Gfx::IntRect const&, Gfx::AlphaType = Gfx::AlphaType::Premultiplied, Bindings::PredefinedColorSpace = Bindings::PredefinedColorSpace::Srgb);

    // Draws the ImageBitmap at the point as drawImage() does, and returns whether that threw nothing.
    bool draw_image(ImageBitmap&, float x, float y);

protected:
    static Canvas2DContextBase& create(CanvasHost&, GC::Cell& canvas, bool canvas_is_offscreen, CanvasRenderingContext2DSettings const&);
};

}
