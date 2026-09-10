/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <LibWebView/TabPerformanceStats.h>
#include <QApplication>
#include <QPixmap>
#include <UI/Qt/PerformanceMonitorWidget.h>

int main(int argc, char** argv)
{
    QApplication application(argc, argv);
    Ladybird::PerformanceMonitorWidget widget(nullptr);
    auto initial_size = widget.size();
    auto initial_hint = widget.sizeHint();
    for (u64 value : { 0ULL, 9ULL, 10ULL, 99ULL, 100ULL, 999ULL, 1000ULL, 999999ULL, 1000000ULL, 1000000000ULL }) {
        WebView::TabPerformanceStats stats;
        stats.cpu_percent = value;
        stats.frames_per_second = value;
        stats.memory_bytes = value;
        stats.download_bytes_per_second = value;
        stats.upload_bytes_per_second = value;
        widget.set_stats(stats);
        VERIFY(widget.size() == initial_size);
        VERIFY(widget.sizeHint() == initial_hint);
    }
    widget.set_stats({});
    VERIFY(widget.size() == initial_size);
    VERIFY(widget.toolTip().isEmpty());
    WebView::TabPerformanceStats stats;
    stats.cpu_percent = 37;
    stats.memory_bytes = 412'000'000;
    stats.download_bytes_per_second = 1'800'000;
    stats.upload_bytes_per_second = 12'000;
    stats.frames_per_second = 120;
    stats.cpu_history = { 20, 28, 25, 39, 26, 50, 36, 28, 23, 30, 20, 18, 35, 37 };
    widget.set_stats(stats);
    if (argc == 2)
        VERIFY(widget.grab().save(QString::fromLocal8Bit(argv[1])));
    return 0;
}
