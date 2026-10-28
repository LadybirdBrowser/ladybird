/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/IntrusiveList.h>
#include <AK/Time.h>
#include <LibCore/Forward.h>
#include <LibGfx/DecodedImageFrame.h>
#include <LibGfx/Forward.h>
#include <LibWeb/HTML/DecodedImageData.h>

namespace Web::HTML {

class BitmapDecodedImageData final : public DecodedImageData {
    GC_CELL(BitmapDecodedImageData, DecodedImageData);
    GC_DECLARE_ALLOCATOR(BitmapDecodedImageData);

public:
    struct Frame {
        Gfx::DecodedImageFrame frame;
        int duration { 0 };
    };

    // With the encoded data the image was decoded from, the decoded frame may be discarded while it is not in use and
    // decoded again when it is needed.
    static ErrorOr<GC::Ref<BitmapDecodedImageData>> create(Vector<Frame>&&, size_t loop_count, bool animated, ByteBuffer encoded_data = {});
    virtual ~BitmapDecodedImageData() override;

    virtual Optional<Gfx::DecodedImageFrame> default_frame(Gfx::IntSize = {}) const override;
    virtual Optional<Gfx::DecodedImageFrame> current_frame(Gfx::IntSize = {}) const override;

    virtual Optional<CSSPixels> intrinsic_width() const override;
    virtual Optional<CSSPixels> intrinsic_height() const override;
    virtual Optional<CSSPixelFraction> intrinsic_aspect_ratio() const override;

    virtual Optional<Gfx::Color> color_if_single_pixel_bitmap() const override;

    virtual Optional<Painting::ImagePaint> image_paint(Painting::ImagePaintRequest const&) const override;

    // For tests: discards the frame of every image that can be decoded again, in use or not.
    static size_t discard_all_discardable_frames();

private:
    BitmapDecodedImageData(Gfx::DecodedImageFrame&& frame, ByteBuffer encoded_data);

    virtual void finalize() override;

    virtual size_t external_memory_size() const override;

    Optional<Gfx::DecodedImageFrame> ensure_frame() const;
    void did_decode_frame() const;
    void discard_frame() const;
    void leave_discardable_frames() const;
    void did_fail_to_decode_again() const;
    void give_up_decoding_again() const;

    static void discard_idle_frames();
    static void schedule_discard_of_idle_frames(AK::Duration delay);

    mutable Optional<Gfx::DecodedImageFrame> m_frame;
    Gfx::IntSize m_size;
    mutable ByteBuffer m_encoded_data;
    u64 m_frame_id { 0 };
    mutable MonotonicTime m_last_used { MonotonicTime::now_coarse() };
    mutable u8 m_failed_attempts_to_decode_again { 0 };
    mutable RefPtr<Core::Timer> m_retry_decoding_again_timer;
    mutable IntrusiveListNode<BitmapDecodedImageData> m_discardable_frame_list_node;

public:
    using DiscardableFrameList = IntrusiveList<&BitmapDecodedImageData::m_discardable_frame_list_node>;
};

}
