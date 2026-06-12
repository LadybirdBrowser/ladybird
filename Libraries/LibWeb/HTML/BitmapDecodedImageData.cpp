/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/ExternalMemory.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/BitmapDecodedImageData.h>
#include <LibWeb/Painting/DisplayListRecorder.h>
#include <LibWeb/Painting/DisplayListRecordingContext.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(BitmapDecodedImageData);

GC::Ref<BitmapDecodedImageData> BitmapDecodedImageData::create(JS::Realm& realm, Gfx::DecodedImageFrame&& frame)
{
    return realm.create<BitmapDecodedImageData>(move(frame));
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

Optional<Gfx::DecodedImageFrame> BitmapDecodedImageData::frame(size_t, Gfx::IntSize) const
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

Optional<Gfx::IntRect> BitmapDecodedImageData::frame_rect(size_t) const
{
    return m_frame.rect();
}

void BitmapDecodedImageData::paint(DisplayListRecordingContext& context, size_t, Gfx::IntRect dst_rect, Gfx::ScalingMode scaling_mode) const
{
    context.display_list_recorder().draw_scaled_decoded_image_frame(dst_rect, m_frame, scaling_mode);
}

}
