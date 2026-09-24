/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/BinarySearch.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibMedia/TimeRanges.h>

namespace Media {

void TimeRanges::add_range(AK::Duration start, AK::Duration end)
{
    if (start >= end)
        return;
    if (m_ranges.is_empty()) {
        m_ranges.append({ start, end });
        return;
    }

    // Find the index we should insert at. This index would point to either the range overlapping the start of the new
    // range, or the range following it if there is no overlap.
    size_t insert_index = 0;
    binary_search(m_ranges, start, &insert_index, [](AK::Duration needle, Range const& range) -> int {
        return needle <=> range.start;
    });
    if (m_ranges[insert_index].end < start)
        insert_index++;

    // Determine the range of ranges that will be made contiguous by this new range.
    AK::Duration merged_start = start;
    AK::Duration merged_end = end;
    size_t merge_end_index = insert_index;

    if (merge_end_index < m_ranges.size()) {
        merged_start = min(merged_start, m_ranges[merge_end_index].start);

        while (merge_end_index < m_ranges.size() && m_ranges[merge_end_index].start <= merged_end) {
            merged_end = max(merged_end, m_ranges[merge_end_index].end);
            merge_end_index++;
        }
    }

    // Replace the found range with our new merged range if applicable.
    if (merge_end_index > insert_index) {
        m_ranges[insert_index] = { merged_start, merged_end };
        m_ranges.remove(insert_index + 1, merge_end_index - insert_index - 1);
    } else {
        m_ranges.insert(insert_index, { merged_start, merged_end });
    }
}

void TimeRanges::remove_range(AK::Duration start, AK::Duration end)
{
    if (start >= end)
        return;

    Vector<Range> new_ranges;
    for (auto const& range : m_ranges) {
        if (range.end <= start || range.start >= end) {
            new_ranges.append(range);
            continue;
        }

        if (range.start < start)
            new_ranges.append({ range.start, start });
        if (range.end > end)
            new_ranges.append({ end, range.end });
    }
    m_ranges = move(new_ranges);
}

AK::Duration TimeRanges::highest_end_time() const
{
    if (m_ranges.is_empty())
        return AK::Duration::zero();
    return m_ranges.last().end;
}

TimeRanges TimeRanges::coalesced(AK::Duration threshold) const
{
    TimeRanges result;
    if (m_ranges.is_empty())
        return result;

    result.m_ranges.append(m_ranges[0]);
    for (size_t i = 1; i < m_ranges.size(); i++) {
        auto& last = result.m_ranges.last();
        if (m_ranges[i].start - last.end <= threshold) {
            last.end = max(last.end, m_ranges[i].end);
        } else {
            result.m_ranges.append(m_ranges[i]);
        }
    }
    return result;
}

TimeRanges TimeRanges::intersection(TimeRanges const& other) const
{
    TimeRanges result;
    size_t i = 0;
    size_t j = 0;

    while (i < m_ranges.size() && j < other.m_ranges.size()) {
        auto const& a = m_ranges[i];
        auto const& b = other.m_ranges[j];

        auto inter_start = max(a.start, b.start);
        auto inter_end = min(a.end, b.end);

        if (inter_start < inter_end)
            result.m_ranges.append({ inter_start, inter_end });

        // Advance the range that ends first.
        if (a.end < b.end)
            i++;
        else
            j++;
    }

    return result;
}

Optional<TimeRanges::Range> TimeRanges::range_at_or_after(AK::Duration point) const
{
    size_t index = 0;
    binary_search(m_ranges, point, &index, [](AK::Duration needle, Range const& range) -> int {
        return needle <=> range.start;
    });
    if (index >= m_ranges.size())
        return {};
    if (m_ranges[index].end <= point)
        index++;
    if (index >= m_ranges.size())
        return {};
    return m_ranges[index];
}

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Media::TimeRanges const& ranges)
{
    TRY(encoder.encode(static_cast<u64>(ranges.size())));
    for (auto const& range : ranges) {
        TRY(encoder.encode(range.start));
        TRY(encoder.encode(range.end));
    }
    return {};
}

template<>
ErrorOr<Media::TimeRanges> decode(Decoder& decoder)
{
    auto range_count = TRY(decoder.decode<u64>());
    Media::TimeRanges ranges;
    for (u64 i = 0; i < range_count; i++) {
        auto start = TRY(decoder.decode<AK::Duration>());
        auto end = TRY(decoder.decode<AK::Duration>());
        if (end < start)
            return Error::from_string_literal("IPC: Invalid time range");
        ranges.add_range(start, end);
    }
    return ranges;
}

}
