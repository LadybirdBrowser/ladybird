/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibGfx/Bitmap.h>
#include <LibMediaCodecs/VideoFramePool.h>
#include <LibMediaCodecs/VideoSurface.h>

#include "VideoFrame.h"

namespace Media {

static u64 next_frame_id()
{
    static Atomic<u64> s_next_id { 1 };
    return s_next_id.fetch_add(1, AK::MemoryOrder::memory_order_relaxed);
}

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth,
    Subsampling subsampling,
    CodingIndependentCodePoints cicp,
    NonnullRefPtr<VideoFrameStorage> storage)
    : m_id(next_frame_id())
    , m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_subsampling(subsampling)
    , m_cicp(cicp)
    , m_storage(move(storage))
{
}

VideoFrame::~VideoFrame() = default;

RefPtr<VideoSurface> VideoFrame::surface() const
{
    return m_storage->surface();
}

Core::AnonymousBuffer VideoFrame::pixel_buffer() const
{
    return m_storage->pixel_buffer();
}

Optional<Gfx::YUVData> VideoFrame::yuv_data() const
{
    auto yuv_data = yuv_data_in_buffer(pixel_buffer(), m_size.to_type<int>(), m_bit_depth, m_subsampling, m_cicp);
    if (yuv_data.is_error())
        return {};
    return yuv_data.release_value();
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> VideoFrame::to_bitmap() const
{
    if (auto surface = this->surface())
        return surface->read_back_bitmap(m_cicp);
    auto yuv_data = this->yuv_data();
    if (!yuv_data.has_value())
        return Error::from_string_literal("Video frame has no pixels to read");
    return yuv_data->to_bitmap();
}

}
