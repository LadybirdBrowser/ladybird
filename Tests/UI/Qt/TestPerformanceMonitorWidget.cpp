/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibTest/TestCase.h>
#include <LibWebView/TabPerformanceStats.h>
#include <UI/Qt/PerformanceMonitorWidget.h>

TEST_CASE(performance_monitor_stats_do_not_change_widget_size)
{
    Ladybird::PerformanceMonitorWidget widget(nullptr);
    auto initial_size = widget.size();
    auto initial_hint = widget.sizeHint();
    for (u64 value : { 0ULL, 9ULL, 10ULL, 99ULL, 100ULL, 999ULL, 1000ULL, 999999ULL, 1000000ULL, 1000000000ULL }) {
        WebView::TabPerformanceStats stats;
        stats.cpu_percent = value;
        stats.frames_per_second = value;
        stats.memory_bytes = value;
        stats.peak_memory_bytes = value;
        stats.download_bytes_per_second = value;
        stats.upload_bytes_per_second = value;
        widget.set_stats(stats);
        EXPECT_EQ(widget.size(), initial_size);
        EXPECT_EQ(widget.sizeHint(), initial_hint);
    }
    widget.set_stats({});
    EXPECT_EQ(widget.size(), initial_size);
    EXPECT(widget.toolTip().isEmpty());
}
