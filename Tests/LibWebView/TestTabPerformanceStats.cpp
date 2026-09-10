/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/TabPerformanceStats.h>

using Core::Platform::ProcessResourceUsage;
using WebView::TabPerformanceAccumulator;

TEST_CASE(cpu_uses_elapsed_time_and_can_exceed_one_hundred_percent)
{
    TabPerformanceAccumulator accumulator;
    auto now = MonotonicTime::now();
    HashMap<pid_t, ProcessResourceUsage> processes;
    processes.set(1, { 10'000'000, 100 });
    processes.set(2, { 20'000'000, 200 });
    auto first = accumulator.sample(now, processes);
    EXPECT(!first.cpu_percent.has_value());
    EXPECT_EQ(*first.memory_bytes, 300u);
    processes.set(1, { 12'000'000, 100 });
    processes.set(2, { 21'400'000, 200 });
    auto second = accumulator.sample(now + AK::Duration::from_milliseconds(500), processes);
    EXPECT_EQ(*second.cpu_percent, 680.0);
    EXPECT_EQ(second.cpu_history.size(), 1u);
}

TEST_CASE(new_removed_and_reset_processes_do_not_create_cpu_spikes)
{
    TabPerformanceAccumulator accumulator;
    auto now = MonotonicTime::now();
    HashMap<pid_t, ProcessResourceUsage> processes;
    processes.set(1, { 10'000'000, 100 });
    accumulator.sample(now, processes);
    processes.remove(1);
    processes.set(2, { 200'000'000, 200 });
    auto second = accumulator.sample(now + AK::Duration::from_seconds(1), processes);
    EXPECT(!second.cpu_percent.has_value());
    EXPECT_EQ(*second.memory_bytes, 200u);
    processes.set(2, { 100, 200 });
    auto reset = accumulator.sample(now + AK::Duration::from_seconds(2), processes);
    EXPECT(!reset.cpu_percent.has_value());
    auto missing = accumulator.sample(now + AK::Duration::from_seconds(3), {});
    EXPECT(!missing.cpu_percent.has_value());
    EXPECT(!missing.memory_bytes.has_value());
}

TEST_CASE(network_rates_use_a_rolling_second_and_actual_push_intervals)
{
    TabPerformanceAccumulator accumulator;
    auto now = MonotonicTime::now();
    accumulator.add_network_bytes(now, 500'000, 1000, 100);
    auto first = accumulator.sample(now, {});
    EXPECT_EQ(first.download_bytes_per_second, 1000u);
    accumulator.add_network_bytes(now + AK::Duration::from_milliseconds(500), 500'000, 1000, 100);
    auto second = accumulator.sample(now + AK::Duration::from_milliseconds(500), {});
    EXPECT_EQ(second.download_bytes_per_second, 2000u);
    EXPECT_EQ(second.upload_bytes_per_second, 200u);
    auto idle = accumulator.sample(now + AK::Duration::from_seconds(2), {});
    EXPECT_EQ(idle.download_bytes_per_second, 0u);
    accumulator.add_network_bytes(now + AK::Duration::from_seconds(7), 5'000'000, 10'000, 1000);
    auto delayed = accumulator.sample(now + AK::Duration::from_seconds(7), {});
    EXPECT_EQ(delayed.download_bytes_per_second, 2000u);
    EXPECT_EQ(delayed.upload_bytes_per_second, 200u);
}

TEST_CASE(fps_counts_presentations_and_becomes_unavailable_when_idle)
{
    TabPerformanceAccumulator accumulator;
    auto now = MonotonicTime::now();
    EXPECT(!accumulator.sample(now, {}).frames_per_second.has_value());
    for (int i = 1; i <= 60; ++i)
        accumulator.did_present(now + AK::Duration::from_microseconds(i * 1'000'000 / 60));
    auto active = accumulator.sample(now + AK::Duration::from_seconds(1), {});
    EXPECT_EQ(*active.frames_per_second, 60.0);
    EXPECT(!accumulator.sample(now + AK::Duration::from_seconds(2), {}).frames_per_second.has_value());
}

TEST_CASE(cpu_history_expires_after_ten_seconds)
{
    TabPerformanceAccumulator accumulator;
    auto now = MonotonicTime::now();
    HashMap<pid_t, ProcessResourceUsage> processes;
    for (u64 i = 0; i <= 30; ++i) {
        processes.set(1, { i * 500'000, 100 });
        auto stats = accumulator.sample(now + AK::Duration::from_milliseconds(i * 500), processes);
        EXPECT(stats.cpu_history.size() <= 20);
    }
    auto expired = accumulator.sample(now + AK::Duration::from_seconds(30), {});
    EXPECT(expired.cpu_history.is_empty());
}

TEST_CASE(memory_overflow_is_unavailable)
{
    TabPerformanceAccumulator accumulator;
    HashMap<pid_t, ProcessResourceUsage> processes;
    processes.set(1, { 0, NumericLimits<u64>::max() });
    processes.set(2, { 0, 1 });
    EXPECT(!accumulator.sample(MonotonicTime::now(), processes).memory_bytes.has_value());
}

TEST_CASE(automatic_byte_units)
{
    EXPECT_EQ(WebView::format_performance_bytes(12), "12 B"sv);
    EXPECT_EQ(WebView::format_performance_bytes(12'000), "12 KB"sv);
    EXPECT_EQ(WebView::format_performance_bytes(1'800'000), "1.8 MB"sv);
    EXPECT_EQ(WebView::format_performance_bytes(1'800'000'000), "1.8 GB"sv);
}

TEST_CASE(fps_window_starts_with_presentations_before_the_first_sample)
{
    TabPerformanceAccumulator accumulator;
    auto now = MonotonicTime::now();
    accumulator.did_present(now);
    accumulator.did_present(now + AK::Duration::from_milliseconds(250));
    auto first = accumulator.sample(now + AK::Duration::from_milliseconds(500), {});
    EXPECT(first.frames_per_second.has_value());
    EXPECT_EQ(first.frames_per_second.value_or(0), 4.0);
    auto second = accumulator.sample(now + AK::Duration::from_milliseconds(750), {});
    EXPECT_EQ(second.frames_per_second.value_or(0), 2.0 / 0.75);
}
