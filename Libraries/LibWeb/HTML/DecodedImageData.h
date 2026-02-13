/*
 * Copyright (c) 2023-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/RefCounted.h>
#include <LibGfx/ScalingMode.h>
#include <LibGfx/Size.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>
#include <LibWeb/PixelUnits.h>

namespace Web::HTML {

// https://html.spec.whatwg.org/multipage/images.html#img-req-data
class DecodedImageData : public JS::Cell {
    GC_CELL(DecodedImageData, JS::Cell);

public:
    virtual ~DecodedImageData();

    virtual Optional<Gfx::IntRect> frame_rect([[maybe_unused]] size_t frame_index) const = 0;
    virtual void paint([[maybe_unused]] DisplayListRecordingContext&, [[maybe_unused]] size_t frame_index, [[maybe_unused]] Gfx::IntRect dst_rect, [[maybe_unused]] Gfx::IntRect clip_rect, [[maybe_unused]] Gfx::ScalingMode scaling_mode) const = 0;

    virtual RefPtr<Gfx::ImmutableBitmap> bitmap(size_t frame_index, Gfx::IntSize = {}) const = 0;
    virtual int frame_duration(size_t frame_index) const = 0;

    virtual size_t frame_count() const = 0;
    virtual size_t loop_count() const = 0;
    virtual bool is_animated() const = 0;

    virtual size_t notify_frame_advanced(size_t frame_index) { return frame_index; }

    virtual Optional<CSSPixels> intrinsic_width() const = 0;
    virtual Optional<CSSPixels> intrinsic_height() const = 0;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const = 0;

protected:
    DecodedImageData();
};

}
