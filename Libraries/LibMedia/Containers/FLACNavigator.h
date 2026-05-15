/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/OwnPtr.h>
#include <LibMedia/Codecs/FLAC.h>

#include "ScanningContainerNavigator.h"

namespace Media {

struct FLACCachedRange : CachedByteRange {
    using CachedByteRange::CachedByteRange;

    // Empty means "we scanned but didn't find a valid frame at this boundary".
    Optional<AK::Duration> time_start;
    Optional<AK::Duration> time_end;
};

class FLACNavigator final : public ScanningContainerNavigator<FLACNavigator, FLACCachedRange> {
public:
    static OwnPtr<FLACNavigator> create(ReadonlyBytes first_frame, NonnullRefPtr<MediaStreamCursor> cursor, u32 sample_rate);

    void on_cached_range_changed(FLACCachedRange& cached_range, CachedRangeChange change) const;
    static void append_time_range(FLACCachedRange const& cached_range, TimeRanges& to);

    Optional<AK::Duration> scan_forward_for_timestamp(size_t search_start, size_t search_end) const;
    Optional<AK::Duration> scan_backward_for_end_timestamp(size_t search_start, size_t search_end) const;

private:
    FLACNavigator(NonnullRefPtr<MediaStreamCursor> cursor, u32 sample_rate, u16 sync_code, u16 fixed_block_size)
        : m_cursor(move(cursor))
        , m_sample_rate(sample_rate)
        , m_sync_code(sync_code)
        , m_fixed_block_size(fixed_block_size)
    {
    }

    Optional<Codecs::FLAC::FrameInfo> find_first_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const;
    Optional<Codecs::FLAC::FrameInfo> find_last_frame(MediaStreamCursor& cursor, size_t search_start, size_t search_end) const;

    AK::Duration sample_to_duration(u64 sample) const;

    NonnullRefPtr<MediaStreamCursor> m_cursor;
    u32 m_sample_rate;
    u16 m_sync_code;
    u16 m_fixed_block_size;
};

}
