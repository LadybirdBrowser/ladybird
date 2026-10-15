/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include "CachedByteRanges.h"
#include "ContainerNavigator.h"

namespace Media {

template<typename Derived, typename CachedRange = CachedByteRange>
class ScanningContainerNavigator : public SingleTrackContainerNavigator {
public:
    BufferedRangesScan buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const override
    {
        auto const& self = *static_cast<Derived const*>(this);
        m_cached_ranges.update(byte_ranges, [&](CachedRange& range, CachedRangeChange change) {
            self.on_cached_range_changed(range, change);
        });

        BufferedRangesScan scan;
        auto const& cached_ranges = m_cached_ranges.ranges();
        if (cached_ranges.is_empty())
            return scan;

        for (size_t i = 0; i + 1 < cached_ranges.size(); i++)
            self.append_time_range(cached_ranges[i], scan.time_ranges);

        TimeRanges last_byte_range_time_ranges;
        self.append_time_range(cached_ranges.last(), last_byte_range_time_ranges);
        scan.last_byte_range_has_samples = !last_byte_range_time_ranges.is_empty();
        for (auto const& range : last_byte_range_time_ranges)
            scan.time_ranges.add_range(range.start, range.end);

        return scan;
    }

protected:
    mutable CachedByteRanges<CachedRange> m_cached_ranges;
};

}
