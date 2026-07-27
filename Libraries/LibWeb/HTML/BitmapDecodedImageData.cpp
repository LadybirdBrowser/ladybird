/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibWeb/CSS/ComputedValues.h>
#include <LibWeb/HTML/BitmapDecodedImageData.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BitmapDecodedImageData);

ErrorOr<GC::Ref<BitmapDecodedImageData>> BitmapDecodedImageData::create(Vector<Frame>&& frames, size_t loop_count, bool animated)
{
    (void)loop_count;
    (void)animated;
    if (frames.is_empty())
        return Error::from_string_literal("Bitmap image has no frames");
    return GC::Heap::the().allocate<BitmapDecodedImageData>(move(frames[0].frame));
}

BitmapDecodedImageData::BitmapDecodedImageData(Gfx::DecodedImageFrame&& frame)
    : m_frame(move(frame))
{
}

BitmapDecodedImageData::~BitmapDecodedImageData() = default;

size_t BitmapDecodedImageData::external_memory_size() const
{
    return m_frame.bitmap().data_size();
}

Optional<Gfx::DecodedImageFrame> BitmapDecodedImageData::current_frame(Gfx::IntSize) const
{
    return m_frame;
}

Optional<Gfx::DecodedImageFrame> BitmapDecodedImageData::default_frame(Gfx::IntSize) const
{
    return m_frame;
}

Optional<CSSPixels> BitmapDecodedImageData::intrinsic_width() const
{
    return m_frame.width();
}

Optional<CSSPixels> BitmapDecodedImageData::intrinsic_height() const
{
    return m_frame.height();
}

Optional<CSSPixelFraction> BitmapDecodedImageData::intrinsic_aspect_ratio() const
{
    return CSSPixels(m_frame.width()) / CSSPixels(m_frame.height());
}

void BitmapDecodedImageData::paint(DisplayListRecordingContext& context, Gfx::IntRect dst_rect, CSS::ImageRendering image_rendering, CSS::PreferredColorScheme) const
{
    auto scaling_mode = CSS::to_gfx_scaling_mode(image_rendering, m_frame.size(), dst_rect.size());

    context.display_list_recorder().draw_scaled_decoded_image_frame(dst_rect, m_frame, scaling_mode);
}

}
