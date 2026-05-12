/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Containers/ContainerNavigator.h>
#include <LibSync/Mutex.h>

namespace Media {

class MP3Navigator final : public ContainerNavigator {
public:
    MP3Navigator(NonnullRefPtr<MediaStream> stream, size_t first_frame_position, AK::Duration total_duration);

    struct CachedRange {
        size_t byte_start { 0 };
        AK::Duration time_start { AK::Duration::zero() };
        size_t last_scanned_byte { 0 };
        u64 duration_in_ticks { 0 };
    };

    TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const override;
    DecoderErrorOr<SeekResult> seek_to_timestamp(AK::Duration timestamp) const override;

private:
    void update_cached_ranges(Vector<MediaStream::ByteRange> const& byte_ranges, MediaStreamCursor&) const;
    void reproject_cached_range_times() const;

    NonnullRefPtr<MediaStream> m_stream;
    size_t m_first_frame_position;
    AK::Duration m_total_duration;

    NonnullRefPtr<MediaStreamCursor> m_buffered_range_scanning_cursor;
    NonnullRefPtr<MediaStreamCursor> m_seek_range_scanning_cursor;
    NonnullRefPtr<MediaStreamCursor> m_seek_cursor;

    mutable Sync::Mutex m_mutex;
    mutable Vector<CachedRange> m_cached_ranges;
};

}
