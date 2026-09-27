/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibMedia/AudioBlock.h>
#include <LibMedia/CodedFrame.h>
#include <LibMedia/Export.h>

namespace Media {

// Tracks the decoded output of coded frames that must not be presented, so that audio decoders can drop it. Decoders
// limit each block to end at the next discard boundary, so that every block is either wholly discarded or kept.
class MEDIA_API AudioDiscardIntervals {
public:
    void add_intervals_for_frame(CodedFrame const&);

    size_t frames_until_next_boundary(AK::Duration block_start, u32 sample_rate, size_t max_frame_count) const;
    bool should_discard(AudioBlock const&);

    void clear() { m_intervals.clear(); }

private:
    struct Interval {
        AK::Duration start;
        AK::Duration end;
    };

    Vector<Interval> m_intervals;
};

}
