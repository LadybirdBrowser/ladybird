/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibAudio/SpscAudioFrameRing.h>

namespace Audio {

SpscAudioFrameRing::SpscAudioFrameRing(size_t frame_capacity, u32 channel_count)
    : m_owned_samples(MUST(FixedArray<float>::create(round_up_to_a_power_of_two(frame_capacity) * channel_count)))
{
    VERIFY(frame_capacity > 0);
    VERIFY(channel_count > 0);
    m_owned_header.channel_count = channel_count;
    m_owned_header.frame_capacity = round_up_to_a_power_of_two(frame_capacity);
    set_storage(m_owned_header, channel_count, m_owned_header.frame_capacity, m_owned_samples.span());
}

}
