/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGC/Heap.h>
#include <LibGfx/Bitmap.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/HTML/AnimatedBitmapDecodedImageData.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(AnimatedBitmapDecodedImageData);

ErrorOr<GC::Ref<AnimatedBitmapDecodedImageData>> AnimatedBitmapDecodedImageData::create(JS::Realm& realm, Vector<Frame>&& frames, size_t loop_count, bool animated)
{
    return realm.create<AnimatedBitmapDecodedImageData>(move(frames), loop_count, animated);
}

AnimatedBitmapDecodedImageData::AnimatedBitmapDecodedImageData(Vector<Frame>&& frames, size_t loop_count, bool animated)
    : m_frames(move(frames))
    , m_loop_count(loop_count)
    , m_animated(animated)
{
}

AnimatedBitmapDecodedImageData::~AnimatedBitmapDecodedImageData() = default;

RefPtr<Gfx::ImmutableBitmap> AnimatedBitmapDecodedImageData::bitmap(size_t frame_index, Gfx::IntSize) const
{
    if (frame_index >= m_frames.size())
        return nullptr;
    return m_frames[frame_index].bitmap;
}

int AnimatedBitmapDecodedImageData::frame_duration(size_t frame_index) const
{
    if (frame_index >= m_frames.size())
        return 0;
    return m_frames[frame_index].duration;
}

Optional<CSSPixels> AnimatedBitmapDecodedImageData::intrinsic_width(Gfx::ImageOrientation orientation) const
{
    return m_frames.first().bitmap->width(orientation);
}

Optional<CSSPixels> AnimatedBitmapDecodedImageData::intrinsic_height(Gfx::ImageOrientation orientation) const
{
    return m_frames.first().bitmap->height(orientation);
}

Optional<CSSPixelFraction> AnimatedBitmapDecodedImageData::intrinsic_aspect_ratio(Gfx::ImageOrientation orientation) const
{
    return CSSPixels(m_frames.first().bitmap->width(orientation)) / CSSPixels(m_frames.first().bitmap->height(orientation));
}

}
