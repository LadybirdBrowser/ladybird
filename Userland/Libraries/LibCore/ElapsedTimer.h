/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>

namespace Core {

enum class TimerType {
    Precise,
    Coarse
};

class ElapsedTimer {
public:
    static ElapsedTimer start_new(TimerType timer_type = TimerType::Coarse);

    ElapsedTimer(TimerType timer_type = TimerType::Coarse)
        : m_timer_type(timer_type)
    {
    }

    bool is_valid() const { return m_valid; }
    void start();
    void reset();

    i64 elapsed_milliseconds() const;
    AK::Duration elapsed_time() const;

    // FIXME: Move callers to elapsed_milliseconds(), remove this.
    i64 elapsed() const // milliseconds
    {
        return elapsed_milliseconds();
    }

    MonotonicTime const& origin_time() const { return m_origin_time; }

private:
    MonotonicTime m_origin_time { MonotonicTime::now() };
    TimerType m_timer_type { TimerType::Coarse };
    bool m_valid { false };
};

}
