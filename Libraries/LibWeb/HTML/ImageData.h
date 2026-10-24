/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2024-2025, Kenneth Myhra <kennethmyhra@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullRefPtr.h>
#include <LibGfx/Forward.h>
#include <LibJS/Forward.h>
#include <LibWeb/Bindings/ImageData.h>
#include <LibWeb/Bindings/RustInterface.h>
#include <LibWeb/Export.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWebCommon/Bindings/PredefinedColorSpace.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#imagedata
// The C++ side of an ImageData, which is implemented in Rust (Libraries/LibWeb/Bindings/Rust/src/html): what
// createImageBitmap() reads.
class WEB_API ImageData final : public Bindings::RustInterfaceCell {
    WEB_RUST_INTERFACE(ImageData, image_data)

public:
    // A new ImageData of the size, with transparent black pixels and the sRGB color space,
    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#initialize-an-imagedata-object
    static WebIDL::ExceptionOr<GC::Ref<ImageData>> create(JS::Realm&, u32 width, u32 height);

    u32 width() const;
    u32 height() const;

    // https://html.spec.whatwg.org/multipage/imagebitmap-and-animations.html#dom-imagedata-colorspace
    Bindings::PredefinedColorSpace color_space() const;

    // Whether the buffer of the data attribute is detached.
    bool is_detached() const;

    // The pixels, RGBA8888 and unpremultiplied, width() × 4 bytes per row, in the buffer of the data attribute; null if
    // the buffer is detached or the data has no room for them. Script can detach the buffer, so they are valid until
    // script runs.
    u8* pixels() const;

    // A bitmap that views the pixels, valid until script runs.
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> bitmap() const;
};

}
