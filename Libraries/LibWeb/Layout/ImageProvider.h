/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGC/Cell.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Size.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

// FIXME: Update all ImageProviders to be DecodedImageData::Clients (i.e. support animated images)
class ImageProvider {
public:
    virtual ~ImageProvider() { }

    bool is_image_available() const { return decoded_image_data() != nullptr; }

    // Whether image data is expected to become available later, e.g. a fetch is still in progress
    // or a lazy load is deferred.
    virtual bool is_image_pending() const { return false; }

    // https://html.spec.whatwg.org/multipage/rendering.html#images-3
    // The alt text fallback is only for images that are known not to render (they failed, or there
    // is nothing to load); while an image is still expected to arrive, the element renders as
    // blank space, matching other engines.
    bool renders_as_alt_text() const { return !is_image_available() && !is_image_pending(); }

    virtual GC::Ptr<HTML::DecodedImageData> decoded_image_data() const = 0;

    virtual Optional<CSSPixels> intrinsic_width() const;
    virtual Optional<CSSPixels> intrinsic_height() const;
    Optional<CSSPixelSize> intrinsic_size() const;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const;

    Optional<Gfx::DecodedImageFrame> current_image_frame(Optional<Gfx::IntSize> size = {}) const;
    Optional<Gfx::DecodedImageFrame> default_image_frame(Optional<Gfx::IntSize> size = {}) const;

    virtual void layout_node_was_detached() const { }

protected:
    static void did_update_alt_text(ImageBox&);
};

}
