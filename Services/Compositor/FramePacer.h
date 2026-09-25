/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>

namespace Compositor {

// Paces rendering opportunities delivered on display ticks to a maximum frame rate.
// Keeps the interval calculation and last-delivery time together for reuse.
class FramePacer {
public:
    void set_maximum_frames_per_second(double);
    double maximum_frames_per_second() const { return m_maximum_frames_per_second; }

    // The whole number of display ticks between two frames, in milliseconds.
    double frame_interval(double display_refresh_rate) const;
    bool is_due(MonotonicTime frame_time, double display_refresh_rate) const;
    void did_deliver(MonotonicTime frame_time) { m_last_frame_time_nanoseconds = frame_time.nanoseconds(); }

private:
    double m_maximum_frames_per_second { 60.0 };
    Optional<i64> m_last_frame_time_nanoseconds;
};

}
