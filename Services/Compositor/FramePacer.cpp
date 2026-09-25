/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <Compositor/FramePacer.h>

namespace Compositor {

void FramePacer::set_maximum_frames_per_second(double maximum_frames_per_second)
{
    VERIFY(maximum_frames_per_second == maximum_frames_per_second);
    VERIFY(maximum_frames_per_second > 0);
    VERIFY(maximum_frames_per_second < AK::Infinity<double>);
    m_maximum_frames_per_second = maximum_frames_per_second;
}

double FramePacer::frame_interval(double display_refresh_rate) const
{
    auto ticks_per_frame = max(1.0, AK::ceil(display_refresh_rate / m_maximum_frames_per_second));
    return ticks_per_frame * 1000.0 / display_refresh_rate;
}

bool FramePacer::is_due(MonotonicTime frame_time, double display_refresh_rate) const
{
    if (!m_last_frame_time_nanoseconds.has_value())
        return true;

    auto interval = frame_interval(display_refresh_rate);
    auto display_interval = 1000.0 / display_refresh_rate;
    auto elapsed = static_cast<double>(frame_time.nanoseconds() - *m_last_frame_time_nanoseconds) / 1'000'000.0;
    return elapsed + display_interval / 2.0 >= interval;
}

}
