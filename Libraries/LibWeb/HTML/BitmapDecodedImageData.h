/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibGfx/Forward.h>
#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::HTML {

class BitmapDecodedImageData final : public DecodedImageData {
    GC_CELL(BitmapDecodedImageData, DecodedImageData);
    GC_DECLARE_ALLOCATOR(BitmapDecodedImageData);

public:
    struct Frame {
        RefPtr<Gfx::ImmutableBitmap> bitmap;
        int duration { 0 };
    };

    static ErrorOr<GC::Ref<BitmapDecodedImageData>> create(JS::Realm&, Vector<Frame>&&, size_t loop_count, bool animated);
    virtual ~BitmapDecodedImageData() override;

    virtual RefPtr<Gfx::ImmutableBitmap> bitmap(size_t frame_index, Gfx::IntSize = {}) const override;
    virtual int frame_duration(size_t frame_index) const override;

    virtual size_t frame_count() const override { return m_frames.size(); }
    virtual size_t loop_count() const override { return m_loop_count; }
    virtual bool is_animated() const override { return m_animated; }

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;

    virtual Optional<Gfx::IntRect> frame_rect(size_t frame_index) const override;
    virtual void paint(DisplayListRecordingContext&, size_t frame_index, Gfx::IntRect dst_rect, Gfx::IntRect clip_rect, Gfx::ScalingMode scaling_mode) const override;

private:
    BitmapDecodedImageData(Vector<Frame>&&, size_t loop_count, bool animated);

    Vector<Frame> m_frames;
    size_t m_loop_count { 0 };
    bool m_animated { false };
};

}
