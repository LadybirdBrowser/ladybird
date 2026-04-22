/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>

#include "ConstantBitrateContainerNavigator.h"

namespace Media {

TimeRanges ConstantBitrateContainerNavigator::buffered_time_ranges(Vector<MediaStream::ByteRange> const& byte_ranges) const
{
    if (byte_ranges.is_empty())
        return {};

    TimeRanges ranges;

    for (auto const& byte_range : byte_ranges) {
        if (byte_range.end <= m_first_sample_position)
            continue;

        auto data_start = (byte_range.start > m_first_sample_position) ? byte_range.start - m_first_sample_position : 0;
        auto data_end = byte_range.end - m_first_sample_position;

        auto time_start = AK::Duration::from_time_units(AK::clamp_to<i64>(data_start), 1, m_bytes_per_second);
        auto time_end = AK::Duration::from_time_units(AK::clamp_to<i64>(data_end), 1, m_bytes_per_second);

        ranges.add_range(max(AK::Duration::zero(), time_start), time_end);
    }

    return ranges;
}

DecoderErrorOr<SeekResult> ConstantBitrateContainerNavigator::seek_to_timestamp(AK::Duration timestamp) const
{
    timestamp = max(timestamp, AK::Duration::zero());
    auto byte_offset = timestamp.to_time_units(1, m_bytes_per_second);
    byte_offset -= byte_offset % static_cast<i64>(m_block_align);
    auto byte_position = static_cast<i64>(m_first_sample_position) + byte_offset;
    auto resolved_timestamp = AK::Duration::from_time_units(byte_offset, 1, m_bytes_per_second);
    return SeekedPosition { byte_position, resolved_timestamp };
}

}
