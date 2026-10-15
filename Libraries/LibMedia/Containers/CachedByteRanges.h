/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/EnumBits.h>
#include <AK/Vector.h>
#include <LibMedia/MediaStream.h>

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

// Keeps one entry per buffered byte range from one scan to the next, so that a scan only looks again at the ends
// a range grew or shrank at.
template<typename CachedRange>
class CachedByteRanges {
public:
    // Reconciles the incoming byte ranges with the cached ranges in lockstep; both are sorted by position.
    template<typename OnChanged>
    void update(Vector<MediaStream::ByteRange> const& byte_ranges, OnChanged on_changed)
    {
        size_t cached_index = 0;
        size_t byte_index = 0;
        while (byte_index < byte_ranges.size()) {
            auto const& byte_range = byte_ranges[byte_index];
            if (cached_index < m_ranges.size()) {
                auto& cached = m_ranges[cached_index];
                // If the cached range doesn't have the same start byte as the incoming range, then we need to update
                // the start of the cached range if possible. If that's not possible, then we discard the cached range
                // if it's earlier, or insert a new one if it's later.
                if (cached.byte_start != byte_range.start) {
                    if (cached.byte_end == byte_range.end) {
                        cached.byte_start = byte_range.start;
                        on_changed(cached, CachedRangeChange::Start);
                        cached_index++;
                        byte_index++;
                        continue;
                    }
                    if (cached.byte_end < byte_range.end) {
                        cached = CachedRange { byte_range.start, byte_range.end };
                        on_changed(cached, CachedRangeChange::Start | CachedRangeChange::End);
                        cached_index++;
                        byte_index++;
                        continue;
                    }
                    if (cached.byte_start < byte_range.start) {
                        m_ranges.remove(cached_index);
                    } else {
                        m_ranges.insert(cached_index, CachedRange { byte_range.start, byte_range.end });
                        auto& inserted = m_ranges[cached_index];
                        on_changed(inserted, CachedRangeChange::Start | CachedRangeChange::End);
                        cached_index++;
                        byte_index++;
                    }
                    continue;
                }

                // If we're here, the cached range's start matches the incoming one, so we only need to update the end.
                if (cached.byte_end != byte_range.end) {
                    cached.byte_end = byte_range.end;
                    on_changed(cached, CachedRangeChange::End);
                }
                cached_index++;
                byte_index++;
            } else {
                // We have new byte ranges on the end with no equivalent cached ranges, append them.
                m_ranges.append(CachedRange { byte_range.start, byte_range.end });
                auto& appended = m_ranges.last();
                on_changed(appended, CachedRangeChange::Start | CachedRangeChange::End);
                cached_index++;
                byte_index++;
            }
        }

        // Remove any leftover cached ranges past the end.
        if (cached_index < m_ranges.size())
            m_ranges.remove(cached_index, m_ranges.size() - cached_index);
    }

    Vector<CachedRange> const& ranges() const { return m_ranges; }

private:
    Vector<CachedRange> m_ranges;
};

}
