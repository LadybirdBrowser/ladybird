/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGfx/Size.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/PixelUnits.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/images.html#img-req-data
class DecodedImageData : public JS::Cell {
    JS_CELL(DecodedImageData, JS::Cell);

public:
    virtual ~DecodedImageData();

    virtual RefPtr<Gfx::ImmutableBitmap> bitmap(size_t frame_index, Gfx::IntSize = {}) const = 0;
    virtual int frame_duration(size_t frame_index) const = 0;

    virtual size_t frame_count() const = 0;
    virtual size_t loop_count() const = 0;
    virtual bool is_animated() const = 0;

    virtual Optional<CSSPixels> intrinsic_width() const = 0;
    virtual Optional<CSSPixels> intrinsic_height() const = 0;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const = 0;

protected:
    DecodedImageData();
};

}
