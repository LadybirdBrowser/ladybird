/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Error.h>
#include <AK/FixedArray.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <LibGfx/ColorSpace.h>
#include <LibGfx/Size.h>
#include <LibGfx/YUVData.h>
#include <LibIPC/Forward.h>
#include <LibMedia/Export.h>

namespace Media {

class PooledVideoFrameSlot;
class ResolvedVideoFrameSlot;

class MEDIA_API VideoFrame final : public AtomicRefCounted<VideoFrame> {

public:
    using BackingStorage = Variant<FixedArray<u8>, NonnullRefPtr<PooledVideoFrameSlot>, NonnullRefPtr<ResolvedVideoFrameSlot>>;

    VideoFrame(
        AK::Duration timestamp,
        AK::Duration duration,
        Gfx::Size<u32> size,
        u8 bit_depth,
        Gfx::ColorSpace color_space,
        Gfx::YUVData yuv_data,
        BackingStorage backing_storage);
    ~VideoFrame();

    AK::Duration timestamp() const { return m_timestamp; }
    AK::Duration duration() const { return m_duration; }

    Gfx::Size<u32> size() const { return m_size; }
    u32 width() const { return size().width(); }
    u32 height() const { return size().height(); }

    u8 bit_depth() const { return m_bit_depth; }

    Gfx::ColorSpace const& color_space() const { return m_color_space; }
    Gfx::YUVData const& yuv_data() const { return m_yuv_data; }

    PooledVideoFrameSlot const* pool_slot() const;

    // Confirms the viewed pixels were not recycled while being consumed. Only frames resolved
    // from a VideoFrameHandle can fail this.
    bool revalidate_backing() const;

private:
    AK::Duration m_timestamp;
    AK::Duration m_duration;
    Gfx::Size<u32> m_size;
    u8 m_bit_depth;
    Gfx::ColorSpace m_color_space;
    Gfx::YUVData m_yuv_data;
    BackingStorage m_backing_storage;
};

}

namespace IPC {

template<>
MEDIA_API ErrorOr<void> encode(Encoder&, Media::VideoFrame const&);
template<>
MEDIA_API ErrorOr<void> encode(Encoder&, NonnullRefPtr<Media::VideoFrame const> const&);

template<>
MEDIA_API ErrorOr<NonnullRefPtr<Media::VideoFrame const>> decode(Decoder&);

}
