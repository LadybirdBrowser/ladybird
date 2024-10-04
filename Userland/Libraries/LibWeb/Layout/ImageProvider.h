/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Size.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::Layout {

class ImageProvider {
public:
    virtual ~ImageProvider() { }

    virtual bool is_image_available() const = 0;

    virtual Optional<CSSPixels> intrinsic_width() const = 0;
    virtual Optional<CSSPixels> intrinsic_height() const = 0;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const = 0;

    virtual RefPtr<Gfx::ImmutableBitmap> current_image_bitmap(Gfx::IntSize) const = 0;
    virtual void set_visible_in_viewport(bool) = 0;

    virtual JS::NonnullGCPtr<DOM::Element const> to_html_element() const = 0;

protected:
    static void did_update_alt_text(ImageBox&);
};

}
