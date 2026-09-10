/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/HashMap.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibCore/Platform/ProcessResourceUsage.h>
#include <LibWebView/Export.h>

namespace WebView {

WEBVIEW_API String format_performance_bytes(u64);

struct TabPerformanceStats {
    Optional<double> cpu_percent;
    Optional<u64> memory_bytes;
    u64 download_bytes_per_second { 0 };
    u64 upload_bytes_per_second { 0 };
    Optional<double> frames_per_second;
    Vector<double> cpu_history;
};

// Pure accounting state: callers supply monotonic timestamps and cumulative process counters.
class WEBVIEW_API TabPerformanceAccumulator {
public:
    void add_network_bytes(MonotonicTime, u64 interval_microseconds, u64 download, u64 upload);
    void did_present(MonotonicTime);
    TabPerformanceStats sample(MonotonicTime, HashMap<pid_t, Core::Platform::ProcessResourceUsage> const&);

private:
    struct NetworkSample {
        MonotonicTime time;
        u64 interval_microseconds;
        u64 download;
        u64 upload;
    };
    struct CPUSample {
        MonotonicTime time;
        double percent;
    };
    HashMap<pid_t, Core::Platform::ProcessResourceUsage> m_previous_processes;
    Optional<MonotonicTime> m_previous_sample;
    Optional<MonotonicTime> m_started_at;
    Vector<NetworkSample> m_network;
    Vector<MonotonicTime> m_presentations;
    Vector<CPUSample> m_cpu_history;
};

}
