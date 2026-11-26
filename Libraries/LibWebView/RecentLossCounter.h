/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibWebView/Export.h>

namespace WebView {

// Counts the recent losses of helpers that share a cause, such as a GPU driver: each loss adds one, and the count drops
// by one every decay interval. A helper that was already running when a loss was counted may have been lost to the
// same cause, so its loss within the coalescing window counts as part of that one. Each counted loss starts a new
// epoch, which helpers are launched in.
class WEBVIEW_API RecentLossCounter {
public:
    using Epoch = u64;

    RecentLossCounter(u32 maximum_count, AK::Duration decay_interval, AK::Duration coalescing_window);

    Epoch current_epoch() const { return m_epoch; }
    void record_loss(MonotonicTime now, Epoch launch_epoch);
    bool is_at_maximum(MonotonicTime now);

private:
    void decay(MonotonicTime now);

    u32 m_maximum_count { 0 };
    AK::Duration m_decay_interval;
    AK::Duration m_coalescing_window;

    u32 m_count { 0 };
    MonotonicTime m_last_decay_time { MonotonicTime::now_coarse() };
    Optional<MonotonicTime> m_last_counted_loss_time;
    Epoch m_epoch { 0 };
};

}
