/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StringBuilder.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/Export.h>

namespace Media {

// A sorted, non-overlapping collection of time ranges. Ranges are automatically merged
// when they overlap or are adjacent. This is the backing store for track buffer ranges
// in the Media Source Extensions spec.
class MEDIA_API TimeRanges {
public:
    struct Range {
        AK::Duration start;
        AK::Duration end;

        bool operator==(Range const&) const = default;
    };

    TimeRanges() = default;
    TimeRanges(std::initializer_list<Range> ranges)
    {
        for (auto const& range : ranges)
            add_range(range.start, range.end);
    }

    void add_range(AK::Duration start, AK::Duration end);
    void remove_range(AK::Duration start, AK::Duration end);

    size_t size() const { return m_ranges.size(); }
    bool is_empty() const { return m_ranges.is_empty(); }

    Range const& operator[](size_t index) const { return m_ranges[index]; }

    AK::Duration highest_end_time() const;

    // Returns a copy with ranges separated by gaps smaller than the given threshold merged.
    [[nodiscard]] TimeRanges coalesced(AK::Duration threshold) const;

    // Returns the intersection of this set of ranges with another.
    TimeRanges intersection(TimeRanges const& other) const;

    // Returns the range containing the given point, or the nearest range after it.
    Optional<Range> range_at_or_after(AK::Duration point) const;

    bool operator==(TimeRanges const& other) const
    {
        return m_ranges == other.m_ranges;
    }

    auto begin() const { return m_ranges.begin(); }
    auto end() const { return m_ranges.end(); }

private:
    // Sorted by start time, non-overlapping.
    Vector<Range> m_ranges;
};

}

template<>
struct AK::Formatter<Media::TimeRanges::Range> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, Media::TimeRanges::Range const& range)
    {
        Formatter<AK::Duration> value_formatter;
        TRY(value_formatter.format(builder, range.start));
        TRY(builder.put_literal("-"sv));
        TRY(value_formatter.format(builder, range.end));
        return {};
    }
};

template<>
struct AK::Formatter<Media::TimeRanges> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, Media::TimeRanges const& ranges)
    {
        TRY(builder.put_literal("["sv));
        for (size_t i = 0; i < ranges.size(); i++) {
            if (i > 0)
                TRY(builder.put_literal(", "sv));
            Formatter<Media::TimeRanges::Range> value_formatter;
            TRY(value_formatter.format(builder, ranges[i]));
        }
        return builder.put_literal("]"sv);
    }
};
