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
    Subsampling subsampling,
    CodingIndependentCodePoints cicp,
    BackingStorage backing_storage)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_subsampling(subsampling)
    , m_cicp(cicp)
    , m_backing_storage(move(backing_storage))
{
}

Optional<Gfx::YUVData> VideoFrame::yuv_data() const
{
    auto slot_buffer = m_backing_storage.visit(
        [](NonnullRefPtr<PooledVideoFrameSlot> const& slot) { return slot->ledger().slot_buffer(slot->slot_index()); },
        [](NonnullRefPtr<ResolvedVideoFrameSlot> const& slot) { return slot->slot_buffer(); });

    auto yuv_data = yuv_data_in_slot_buffer(slot_buffer, m_size.to_type<int>(), m_bit_depth, m_subsampling, m_cicp);
    if (yuv_data.is_error())
        return {};
    return yuv_data.release_value();
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
