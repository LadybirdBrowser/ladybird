/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "IndexedContainerNavigator.h"

namespace Media {

size_t IndexedContainerNavigator::lower_bound(size_t target) const
{
    size_t lo = 0;
    size_t hi = m_entries.size();
    while (lo < hi) {
        auto mid = lo + (hi - lo) / 2;
        if (m_entries[mid].position < target)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo;
}

TimeRanges IndexedContainerNavigator::buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const
{
    if (byte_ranges.is_empty())
        return {};

    auto entry_count = m_entries.size();

    TimeRanges ranges;

    for (auto const& byte_range : byte_ranges) {
        // Find the first entry at or after the start, and the first entry at or after the end.
        auto first = lower_bound(byte_range.start);
        auto end = lower_bound(byte_range.end);
        if (first >= end)
            continue;

        auto time_start = max(AK::Duration::zero(), m_entries[first].timestamp);
        auto time_end = (end < entry_count) ? m_entries[end].timestamp : m_duration;
        ranges.add_range(time_start, time_end);
    }

    return ranges;
}

}
