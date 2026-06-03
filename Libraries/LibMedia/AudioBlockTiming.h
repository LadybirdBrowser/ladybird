/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Math.h>
#include <AK/SaturatingMath.h>
#include <AK/Time.h>

namespace Media {

class AudioBlockTiming {
public:
    AK::Duration media_time_start() const { return m_media_time_start; }
    AK::Duration media_time_duration() const { return m_media_time_duration; }
    AK::Duration media_time_end() const { return m_media_time_start + m_media_time_duration; }

    i64 first_frame_index() const { return m_first_frame_index; }
    i64 frame_count() const { return m_frame_count; }
    i64 end_frame_index() const { return saturating_add(m_first_frame_index, m_frame_count); }

    bool contains_frame_index(i64 frame_index) const
    {
        return frame_index >= first_frame_index() && frame_index < end_frame_index();
    }

    AK::Duration media_time_at_frame_index(i64 frame_index) const
    {
        VERIFY(frame_count() > 0);
        if (frame_index < first_frame_index()) {
            auto frame_offset = AK::clamp_to<u32>(first_frame_index() - frame_index);
            auto block_frame_count = AK::clamp_to<u32>(frame_count());
            return media_time_start() - media_time_duration().scaled_by(frame_offset, block_frame_count);
        }
        if (frame_index == first_frame_index())
            return media_time_start();
        if (frame_index >= end_frame_index())
            return media_time_end();

        auto frame_offset = AK::clamp_to<u32>(frame_index - first_frame_index());
        auto block_frame_count = AK::clamp_to<u32>(frame_count());
        return media_time_start() + media_time_duration().scaled_by(frame_offset, block_frame_count);
    }

    void clear()
    {
        m_media_time_start = {};
        m_media_time_duration = {};
        m_first_frame_index = 0;
        m_frame_count = 0;
    }

    void initialize(AK::Duration media_time_start, u32 sample_rate, i64 frame_count)
    {
        m_media_time_start = media_time_start;
        m_first_frame_index = media_time_start.to_time_units(1, sample_rate);
        m_frame_count = frame_count;
        recalculate_media_duration_from_frame_count(sample_rate);
    }

    void initialize(i64 first_frame_index, u32 sample_rate, i64 frame_count)
    {
        m_first_frame_index = first_frame_index;
        m_media_time_start = AK::Duration::from_time_units(first_frame_index, 1, sample_rate);
        m_frame_count = frame_count;
        recalculate_media_duration_from_frame_count(sample_rate);
    }

    void set_frame_count(u32 sample_rate, i64 frame_count)
    {
        m_frame_count = frame_count;
        recalculate_media_duration_from_frame_count(sample_rate);
    }

    void set_first_frame_index(i64 first_frame_index, u32 sample_rate)
    {
        m_first_frame_index = first_frame_index;
        m_media_time_start = AK::Duration::from_time_units(first_frame_index, 1, sample_rate);
    }

    void set_media_time_start(AK::Duration media_time_start) { m_media_time_start = media_time_start; }
    void set_media_time_duration(AK::Duration media_time_duration)
    {
        VERIFY(!media_time_duration.is_negative());
        m_media_time_duration = media_time_duration;
    }

private:
    void recalculate_media_duration_from_frame_count(u32 sample_rate)
    {
        m_media_time_duration = AK::Duration::from_time_units(m_frame_count, 1, sample_rate);
    }

    AK::Duration m_media_time_start;
    AK::Duration m_media_time_duration;
    i64 m_first_frame_index { 0 };
    i64 m_frame_count { 0 };
};

}
