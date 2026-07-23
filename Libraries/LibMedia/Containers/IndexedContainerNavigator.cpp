/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IndexedContainerNavigator.h"

namespace Media {

size_t IndexedContainerNavigator::lower_bound(Vector<IndexEntry> const& entries, size_t target)
{
    size_t lo = 0;
    size_t hi = entries.size();
    while (lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if (entries[mid].position < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

HashMap<u64, BufferedRangesScan> IndexedContainerNavigator::buffered_time_ranges_by_track(Vector<MediaStream::ByteRange> const& byte_ranges) const
{
    HashMap<u64, BufferedRangesScan> result;
    if (byte_ranges.is_empty())
        return result;

    for (auto const& track_index : m_track_indices) {
        auto const& entries = track_index.entries;
        auto entry_count = entries.size();

        BufferedRangesScan scan;

        for (size_t i = 0; i < byte_ranges.size(); i++) {
            auto const& byte_range = byte_ranges[i];

            // Find the first entry at or after the start, and the first entry at or after the end.
            auto first = lower_bound(entries, byte_range.start);
            auto end = lower_bound(entries, byte_range.end);
            if (first >= end)
                continue;

            auto time_start = max(AK::Duration::zero(), entries[first].timestamp);
            auto time_end = (end < entry_count) ? entries[end].timestamp : m_duration;
            scan.time_ranges.add_range(time_start, time_end);

            if (i == byte_ranges.size() - 1)
                scan.last_byte_range_has_samples = true;
        }

        result.set(track_index.track_identifier, move(scan));
    }

    return result;
}

}
