/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/AudioDiscardIntervals.h>

namespace Media {

void AudioDiscardIntervals::add_intervals_for_frame(CodedFrame const& frame)
{
    if (frame.leading_discard() > AK::Duration::zero())
        m_intervals.append({ frame.decoded_start_timestamp(), frame.presentation_timestamp() });
    if (frame.trailing_discard() > AK::Duration::zero()) {
        auto presentation_end = frame.presentation_timestamp() + frame.duration();
        m_intervals.append({ presentation_end, presentation_end + frame.trailing_discard() });
    }
}

size_t AudioDiscardIntervals::frames_until_next_boundary(AK::Duration block_start, u32 sample_rate, size_t max_frame_count) const
{
    auto frame_count = max_frame_count;
    auto limit_to_boundary = [&](AK::Duration boundary) {
        if (boundary <= block_start)
            return;
        // A boundary within half a frame of the block's start is at its start.
        auto frames_until_boundary = (boundary - block_start).to_time_units(1, sample_rate);
        if (frames_until_boundary <= 0)
            return;
        frame_count = min(frame_count, static_cast<size_t>(frames_until_boundary));
    };
    for (auto const& interval : m_intervals) {
        limit_to_boundary(interval.start);
        limit_to_boundary(interval.end);
    }
    return frame_count;
}

bool AudioDiscardIntervals::should_discard(AudioBlock const& block)
{
    auto block_start = block.media_time_start();
    m_intervals.remove_all_matching([&](auto const& interval) { return interval.end <= block_start; });

    // Blocks don't cross boundaries, so the middle of their first frame decides which side of one they are on.
    auto first_frame_middle = block_start + AK::Duration::from_nanoseconds(500'000'000 / block.sample_rate());
    for (auto const& interval : m_intervals) {
        if (interval.start <= first_frame_middle && first_frame_middle < interval.end)
            return true;
    }
    return false;
}

}
