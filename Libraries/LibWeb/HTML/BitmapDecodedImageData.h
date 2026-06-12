/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::HTML {

class BitmapDecodedImageData final : public DecodedImageData {
    GC_CELL(BitmapDecodedImageData, DecodedImageData);
    GC_DECLARE_ALLOCATOR(BitmapDecodedImageData);

public:
    static GC::Ref<BitmapDecodedImageData> create(JS::Realm&, Gfx::DecodedImageFrame&& frame);
    virtual ~BitmapDecodedImageData() override;

    virtual Optional<Gfx::DecodedImageFrame> frame(size_t frame_index, Gfx::IntSize = {}) const override;

    virtual int frame_duration(size_t) const override { return 0; }
    virtual size_t frame_count() const override { return 1; }
    virtual size_t loop_count() const override { return 0; }
    virtual bool is_animated() const override { return false; }

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;

    virtual Optional<Gfx::IntRect> frame_rect(size_t frame_index) const override;
    virtual void paint(DisplayListRecordingContext&, size_t frame_index, Gfx::IntRect dst_rect, Gfx::ScalingMode scaling_mode) const override;

private:
    BitmapDecodedImageData(Gfx::DecodedImageFrame&& frame);

    virtual size_t external_memory_size() const override;

    Gfx::DecodedImageFrame m_frame;
};

}
