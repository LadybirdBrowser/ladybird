/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ScopeGuard.h>
#include <AK/Time.h>
#include <LibCore/ElapsedTimer.h>

namespace Core {

struct TimingInfo {
    u64 cumulative_time_nanoseconds { 0 };
    u64 call_count { 0 };
};

void log_timing_info(ByteString const& name, AK::Duration const& elapsed_time, u64 print_every_n_calls);

#define CONCAT(a, b) a##b
#define UNIQUE_NAME(base) CONCAT(base, __LINE__)

#define REPORT_TIME_EVERY(name, print_every_n_calls)                                                \
    auto UNIQUE_NAME(report_time_timer_) = Core::ElapsedTimer::start_new(Core::TimerType::Precise); \
    ScopeGuard UNIQUE_NAME(report_time_guard_) = [&] {                                              \
        auto elapsed_time = UNIQUE_NAME(report_time_timer_).elapsed_time();                         \
        Core::log_timing_info(#name, elapsed_time, print_every_n_calls);                            \
    };

#define REPORT_TIME(name) \
    REPORT_TIME_EVERY(name, 1)

}
