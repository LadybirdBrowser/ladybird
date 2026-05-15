/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>

#include "ContainerNavigator.h"

namespace Media {

enum class CachedRangeChange : u8 {
    Start = 1 << 0,
    End = 1 << 1,
};
AK_ENUM_BITWISE_OPERATORS(CachedRangeChange);

struct CachedByteRange {
    size_t byte_start { 0 };
    size_t byte_end { 0 };

    constexpr CachedByteRange(size_t start, size_t end)
        : byte_start(start)
        , byte_end(end)
    {
    }
};

template<typename Derived, typename CachedRange = CachedByteRange>
class ScanningContainerNavigator : public ContainerNavigator {
public:
    TimeRanges buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const override
    {
        auto const& self = *static_cast<Derived const*>(this);

        // Reconcile incoming byte ranges with cached ranges in lockstep. Both are sorted by position.
        size_t cached_index = 0;
        size_t byte_index = 0;

        while (byte_index < byte_ranges.size()) {
            auto const& byte_range = byte_ranges[byte_index];

            if (cached_index < m_cached_ranges.size()) {
                auto& cached = m_cached_ranges[cached_index];

                // If the cached range doesn't have the same start byte as the incoming range, then we need to update
                // the start of the cached range if possible. If that's not possible, then we discard the cached range
                // if it's earlier, or insert a new one if it's later.
                if (cached.byte_start != byte_range.start) {
                    if (cached.byte_end == byte_range.end) {
                        cached.byte_start = byte_range.start;
                        self.on_cached_range_changed(cached, CachedRangeChange::Start);
                        cached_index++;
                        byte_index++;
                        continue;
                    }

                    if (cached.byte_end < byte_range.end) {
                        cached = CachedRange { byte_range.start, byte_range.end };
                        self.on_cached_range_changed(cached, CachedRangeChange::Start | CachedRangeChange::End);
                        cached_index++;
                        byte_index++;
                        continue;
                    }

                    if (cached.byte_start < byte_range.start) {
                        m_cached_ranges.remove(cached_index);
                    } else {
                        m_cached_ranges.insert(cached_index, CachedRange { byte_range.start, byte_range.end });
                        auto& inserted = m_cached_ranges[cached_index];
                        self.on_cached_range_changed(inserted, CachedRangeChange::Start | CachedRangeChange::End);
                        cached_index++;
                        byte_index++;
                    }
                    continue;
                }

                // If we're here, the cached range's start matches the incoming one, so we only need to update the end.
                if (cached.byte_end != byte_range.end) {
                    cached.byte_end = byte_range.end;
                    self.on_cached_range_changed(cached, CachedRangeChange::End);
                }

                cached_index++;
                byte_index++;
            } else {
                // We have new byte ranges on the end with no equivalent cached ranges, append them.
                m_cached_ranges.append(CachedRange { byte_range.start, byte_range.end });
                auto& appended = m_cached_ranges.last();
                self.on_cached_range_changed(appended, CachedRangeChange::Start | CachedRangeChange::End);
                cached_index++;
                byte_index++;
            }
        }

        // Remove any leftover cached ranges past the end.
        if (cached_index < m_cached_ranges.size())
            m_cached_ranges.remove(cached_index, m_cached_ranges.size() - cached_index);

        TimeRanges result;
        for (auto const& cached : m_cached_ranges)
            self.append_time_range(cached, result);
        return result;
    }

protected:
    mutable Vector<CachedRange> m_cached_ranges;
};

}
