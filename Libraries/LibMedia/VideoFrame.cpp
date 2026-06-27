/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/VideoFramePool.h>

#include "VideoFrame.h"

namespace Media {

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth,
    Gfx::YUVData yuv_data,
    BackingStorage backing_storage)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_yuv_data(yuv_data)
    , m_backing_storage(move(backing_storage))
{
}

VideoFrame::~VideoFrame() = default;

PooledVideoFrameSlot const* VideoFrame::pool_slot() const
{
    if (!m_backing_storage.has<NonnullRefPtr<PooledVideoFrameSlot>>())
        return nullptr;
    return m_backing_storage.get<NonnullRefPtr<PooledVideoFrameSlot>>().ptr();
}

ResolvedVideoFrameSlot const* VideoFrame::resolved_slot() const
{
    if (auto const* resolved = m_backing_storage.get_pointer<NonnullRefPtr<ResolvedVideoFrameSlot>>())
        return resolved->ptr();
    return nullptr;
}

bool VideoFrame::revalidate_backing() const
{
    if (auto const* resolved_slot = m_backing_storage.get_pointer<NonnullRefPtr<ResolvedVideoFrameSlot>>())
        return (*resolved_slot)->revalidate();
    return true;
}

}
