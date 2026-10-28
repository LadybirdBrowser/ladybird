/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <AK/Variant.h>
#include <LibCore/AnonymousBuffer.h>
#include <LibGfx/Size.h>
#include <LibGfx/YUVData.h>
#include <LibMediaCodecs/Color/CodingIndependentCodePoints.h>
#include <LibMediaCodecs/Export.h>
#include <LibMediaCodecs/Subsampling.h>

namespace Media {

class PooledVideoFrameSlot;
class ReceivedVideoFrameStorage;
class VideoSurface;

class MEDIACODECS_API VideoFrame final : public AtomicRefCounted<VideoFrame> {

public:
    using BackingStorage = Variant<NonnullRefPtr<PooledVideoFrameSlot>, NonnullRefPtr<ReceivedVideoFrameStorage>>;

    VideoFrame(
        AK::Duration timestamp,
        AK::Duration duration,
        Gfx::Size<u32> size,
        u8 bit_depth,
        Subsampling subsampling,
        CodingIndependentCodePoints cicp,
        BackingStorage backing_storage);
    ~VideoFrame();

    // Tells this frame apart from every other frame in this process, including ones whose storage it shares.
    u64 id() const { return m_id; }

    AK::Duration timestamp() const { return m_timestamp; }
    AK::Duration duration() const { return m_duration; }
    AK::Duration end() const { return m_timestamp + m_duration; }
    bool contains_timestamp(AK::Duration timestamp) { return timestamp >= m_timestamp && timestamp < end(); }
    static AK::Duration conservative_end_of(AK::Duration timestamp, AK::Duration duration) { return timestamp + duration.scaled_by(3, 2); }
    AK::Duration conservative_end() const { return conservative_end_of(m_timestamp, m_duration); }

    Gfx::Size<u32> size() const { return m_size; }
    u32 width() const { return size().width(); }
    u32 height() const { return size().height(); }

    u8 bit_depth() const { return m_bit_depth; }
    Subsampling subsampling() const { return m_subsampling; }
    CodingIndependentCodePoints const& cicp() const { return m_cicp; }

    // A view of the planes in the backing slot's buffer, absent when a platform surface holds them instead.
    Optional<Gfx::YUVData> yuv_data() const;

    // The platform surface holding the frame's pixels, null when they live in the backing slot's buffer.
    RefPtr<VideoSurface> surface() const;

    // The shared memory holding the frame's pixels, invalid when a surface holds them instead.
    Core::AnonymousBuffer pixel_buffer() const;

    // Converts the pixels to RGBA in memory, wherever they are, if this process can read them.
    ErrorOr<NonnullRefPtr<Gfx::Bitmap>> to_bitmap() const;

    PooledVideoFrameSlot const* pool_slot() const;
    ReceivedVideoFrameStorage const* received_storage() const;

private:
    u64 m_id { 0 };
    AK::Duration m_timestamp;
    AK::Duration m_duration;
    Gfx::Size<u32> m_size;
    u8 m_bit_depth;
    Subsampling m_subsampling;
    CodingIndependentCodePoints m_cicp;
    BackingStorage m_backing_storage;
};

}
