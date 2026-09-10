/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <LibWebView/TabPerformanceStats.h>

namespace WebView {

String format_performance_bytes(u64 bytes)
{
    if (bytes >= 1'000'000'000)
        return MUST(String::formatted("{:.1f} GB", bytes / 1'000'000'000.0));
    if (bytes >= 1'000'000)
        return MUST(String::formatted("{:.1f} MB", bytes / 1'000'000.0));
    if (bytes >= 1'000)
        return MUST(String::formatted("{:.0f} KB", bytes / 1'000.0));
    return MUST(String::formatted("{} B", bytes));
}

void TabPerformanceAccumulator::add_network_bytes(MonotonicTime now, u64 interval_microseconds, u64 download, u64 upload)
{
    if (interval_microseconds > 0)
        m_network.append({ now, interval_microseconds, download, upload });
}

void TabPerformanceAccumulator::did_present(MonotonicTime now)
{
    if (!m_started_at.has_value())
        m_started_at = now;
    m_presentations.append(now);
    m_presentations.remove_all_matching([&](auto time) { return now - time > AK::Duration::from_seconds(1); });
}

TabPerformanceStats TabPerformanceAccumulator::sample(MonotonicTime now, HashMap<pid_t, Core::Platform::ProcessResourceUsage> const& processes)
{
    TabPerformanceStats stats;
    if (!m_started_at.has_value())
        m_started_at = now;
    auto elapsed = m_previous_sample.has_value() ? (now - *m_previous_sample).to_microseconds() : 0;
    Checked<u64> memory = 0;
    double cpu = 0;
    bool has_baseline = false;
    for (auto const& process : processes) {
        memory += process.value.attributed_memory_bytes;
        if (auto previous = m_previous_processes.get(process.key); previous.has_value() && elapsed > 0
            && process.value.cpu_time_microseconds >= previous->cpu_time_microseconds) {
            cpu += 100.0 * static_cast<double>(process.value.cpu_time_microseconds - previous->cpu_time_microseconds) / elapsed;
            has_baseline = true;
        }
    }
    if (!processes.is_empty() && !memory.has_overflow())
        stats.memory_bytes = memory.value();
    if (has_baseline) {
        stats.cpu_percent = cpu;
        m_cpu_history.append({ now, cpu });
    }
    m_cpu_history.remove_all_matching([&](auto const& sample) { return now - sample.time >= AK::Duration::from_seconds(10); });
    for (auto const& sample : m_cpu_history)
        stats.cpu_history.append(sample.percent);

    m_network.remove_all_matching([&](auto const& sample) { return now - sample.time >= AK::Duration::from_seconds(1); });
    Checked<u64> download = 0;
    Checked<u64> upload = 0;
    for (auto const& sample : m_network) {
        auto age = max<i64>(0, (now - sample.time).to_microseconds());
        auto overlap = min<u64>(sample.interval_microseconds, 1'000'000 - age);
        auto scale = [&](u64 bytes) -> u64 {
            Checked<u64> product = bytes;
            product *= overlap;
            if (!product.has_overflow())
                return product.value() / sample.interval_microseconds;
            // NB: Avoid overflow even for malformed counters, at the cost of fractional-byte precision.
            return (bytes / sample.interval_microseconds) * overlap;
        };
        download += scale(sample.download);
        upload += scale(sample.upload);
    }
    if (!download.has_overflow())
        stats.download_bytes_per_second = download.value();
    if (!upload.has_overflow())
        stats.upload_bytes_per_second = upload.value();

    m_presentations.remove_all_matching([&](auto time) { return now - time >= AK::Duration::from_seconds(1); });
    auto window_microseconds = min<i64>(1'000'000, (now - *m_started_at).to_microseconds());
    if (!m_presentations.is_empty() && window_microseconds > 0)
        stats.frames_per_second = static_cast<double>(m_presentations.size()) * 1'000'000 / window_microseconds;
    m_previous_sample = now;
    m_previous_processes = processes;
    return stats;
}

}
