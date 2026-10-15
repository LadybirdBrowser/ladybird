/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Optional.h>
#include <AK/StdLibExtras.h>
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

// A display tick that was asked for and not delivered yet, paced to the rate it was last asked for at.
class PacedTickRequest {
public:
    // Returns whether the tick had not been asked for already.
    bool request(double maximum_frames_per_second)
    {
        m_pacer.set_maximum_frames_per_second(maximum_frames_per_second);
        return !exchange(m_requested, true);
    }
    bool is_requested() const { return m_requested; }
    double frame_interval(double display_refresh_rate) const { return m_pacer.frame_interval(display_refresh_rate); }
    bool is_due(MonotonicTime frame_time, double display_refresh_rate) const { return m_pacer.is_due(frame_time, display_refresh_rate); }
    void did_deliver(MonotonicTime frame_time)
    {
        VERIFY(m_requested);
        m_requested = false;
        m_pacer.did_deliver(frame_time);
    }

private:
    bool m_requested { false };
    FramePacer m_pacer;
};

}
