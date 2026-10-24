/*
 * Copyright (c) 2024, Lucas Chollet <lucas.chollet@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <AK/Types.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibWeb/Bindings/ImageBitmap.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWebCommon/WebIDL/Types.h>

namespace Web::HTML {

using ImageBitmapSource = FlattenVariant<CanvasImageSource, Variant<GC::Ref<FileAPI::Blob>, GC::Ref<ImageData>>>;

using ResizeQuality = Bindings::ResizeQuality;

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#imagebitmapoptions
using ImageBitmapOptions = Bindings::ImageBitmapOptions;

// AD-HOC: The pixels of every ImageBitmap that one message transfers share one buffer of shared memory, which the
//         message carries alongside the memory of its SharedArrayBuffers. However many bitmaps a message transfers,
//         their pixels then take one attachment, and the receiver uses them without copying.
struct ImageBitmapSharedPixels {
    Core::AnonymousBuffer buffer;
    u32 buffer_index { 0 };
    size_t used_size { 0 };
};

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#imagebitmap
// The C++ side of an ImageBitmap, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/html): what
// createImageBitmap(), OffscreenCanvas and WebDriver create, and createImageBitmap() and StructuredSerialize read.
// ImageBitmap.cpp encodes and decodes the bitmap data for the serialization and transfer steps of the Rust
// implementation.
class WEB_API ImageBitmap final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ImageBitmap, image_bitmap)

public:
    // A new ImageBitmap whose bitmap data is the bitmap, or that has none.
    static GC::Ref<ImageBitmap> create(RefPtr<Gfx::Bitmap const>);
    // A new ImageBitmap whose bitmap data is the frame, with its color space, or that has none.
    static GC::Ref<ImageBitmap> create(Optional<Gfx::DecodedImageFrame>);

    WebIDL::UnsignedLong width() const;
    WebIDL::UnsignedLong height() const;

    // https://html.spec.whatwg.org/multipage/structured-data.html#detached
    bool is_detached() const;

    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagebitmap-close
    void close();

    // https://html.spec.whatwg.org/multipage/canvas.html#concept-canvas-origin-clean
    bool is_origin_clean() const;
    void set_origin_clean(bool);

    // The bitmap of the bitmap data, which the ImageBitmap keeps until it is closed or transferred.
    Gfx::Bitmap const* bitmap() const;
    // The bitmap data with its color space.
    Gfx::DecodedImageFrame const* frame() const;

    static size_t shared_pixels_size(Gfx::Bitmap const&);
};

}
