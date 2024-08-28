/*
 * Copyright (c) 2023, kleines Filmröllchen <filmroellchen@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "GenericTypes.h"
#include <AK/BinarySearch.h>
#include <AK/Error.h>
#include <AK/Optional.h>

namespace Audio {

size_t SeekTable::size() const
{
    return m_seek_points.size();
}

ReadonlySpan<SeekPoint> SeekTable::seek_points() const
{
    return m_seek_points.span();
}

Vector<SeekPoint>& SeekTable::seek_points()
{
    return m_seek_points;
}

Optional<SeekPoint const&> SeekTable::seek_point_before(u64 sample_index) const
{
    if (m_seek_points.is_empty())
        return {};
    size_t nearby_seek_point_index = 0;
    AK::binary_search(m_seek_points, sample_index, &nearby_seek_point_index, [](auto const& sample_index, auto const& seekpoint_candidate) {
        // Subtraction with i64 cast may cause overflow.
        if (sample_index > seekpoint_candidate.sample_index)
            return 1;
        if (sample_index == seekpoint_candidate.sample_index)
            return 0;
        return -1;
    });
    // Binary search will always give us a close index, but it may be too large or too small.
    // By doing the index adjustment in this order, we will always find a seek point before the given sample.
    while (nearby_seek_point_index < m_seek_points.size() - 1 && m_seek_points[nearby_seek_point_index].sample_index < sample_index)
        ++nearby_seek_point_index;
    while (nearby_seek_point_index > 0 && m_seek_points[nearby_seek_point_index].sample_index > sample_index)
        --nearby_seek_point_index;
    if (m_seek_points[nearby_seek_point_index].sample_index > sample_index)
        return {};
    return m_seek_points[nearby_seek_point_index];
}

Optional<u64> SeekTable::seek_point_sample_distance_around(u64 sample_index) const
{
    if (m_seek_points.is_empty())
        return {};
    size_t nearby_seek_point_index = 0;
    AK::binary_search(m_seek_points, sample_index, &nearby_seek_point_index, [](auto const& sample_index, auto const& seekpoint_candidate) {
        // Subtraction with i64 cast may cause overflow.
        if (sample_index > seekpoint_candidate.sample_index)
            return 1;
        if (sample_index == seekpoint_candidate.sample_index)
            return 0;
        return -1;
    });

    while (nearby_seek_point_index < m_seek_points.size() && m_seek_points[nearby_seek_point_index].sample_index <= sample_index)
        ++nearby_seek_point_index;
    // There is no seek point beyond the sample index.
    if (nearby_seek_point_index >= m_seek_points.size())
        return {};
    auto upper_seek_point_index = nearby_seek_point_index;

    while (nearby_seek_point_index > 0 && m_seek_points[nearby_seek_point_index].sample_index > sample_index)
        --nearby_seek_point_index;
    auto lower_seek_point_index = nearby_seek_point_index;

    VERIFY(upper_seek_point_index >= lower_seek_point_index);
    return m_seek_points[upper_seek_point_index].sample_index - m_seek_points[lower_seek_point_index].sample_index;
}

ErrorOr<void> SeekTable::insert_seek_point(SeekPoint seek_point)
{
    if (auto previous_seek_point = seek_point_before(seek_point.sample_index); previous_seek_point.has_value() && previous_seek_point->sample_index == seek_point.sample_index) {
        // Do not insert a duplicate seek point.
        return {};
    }

    // FIXME: This could be even faster if we used binary search while finding the insertion point.
    return m_seek_points.try_insert_before_matching(seek_point, [&](auto const& other_seek_point) {
        return seek_point.sample_index < other_seek_point.sample_index;
    });
}

}
