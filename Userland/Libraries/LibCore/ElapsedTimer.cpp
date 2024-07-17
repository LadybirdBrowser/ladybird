/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>

namespace Core {

ElapsedTimer ElapsedTimer::start_new(TimerType timer_type)
{
    ElapsedTimer timer(timer_type);
    timer.start();
    return timer;
}

void ElapsedTimer::start()
{
    m_valid = true;
    m_origin_time = m_timer_type == TimerType::Precise ? MonotonicTime::now() : MonotonicTime::now_coarse();
}

void ElapsedTimer::reset()
{
    m_valid = false;
}

i64 ElapsedTimer::elapsed_milliseconds() const
{
    return elapsed_time().to_milliseconds();
}

AK::Duration ElapsedTimer::elapsed_time() const
{
    VERIFY(is_valid());
    auto now = m_timer_type == TimerType::Precise ? MonotonicTime::now() : MonotonicTime::now_coarse();
    return now - m_origin_time;
}

}
