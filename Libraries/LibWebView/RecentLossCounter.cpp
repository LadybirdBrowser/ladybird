/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWebView/RecentLossCounter.h>

namespace WebView {

RecentLossCounter::RecentLossCounter(u32 maximum_count, AK::Duration decay_interval, AK::Duration coalescing_window)
    : m_maximum_count(maximum_count)
    , m_decay_interval(decay_interval)
    , m_coalescing_window(coalescing_window)
{
    VERIFY(m_maximum_count > 0);
    VERIFY(m_decay_interval > AK::Duration::zero());
}

void RecentLossCounter::decay(MonotonicTime now)
{
    if (m_count == 0) {
        m_last_decay_time = now;
        return;
    }
    while (m_count > 0 && now - m_last_decay_time >= m_decay_interval) {
        m_count--;
        m_last_decay_time = m_last_decay_time + m_decay_interval;
    }
    if (m_count == 0)
        m_last_decay_time = now;
}

void RecentLossCounter::record_loss(MonotonicTime now, Epoch launch_epoch)
{
    VERIFY(launch_epoch <= m_epoch);
    if (launch_epoch < m_epoch && now - *m_last_counted_loss_time < m_coalescing_window)
        return;
    decay(now);
    m_last_counted_loss_time = now;
    m_epoch++;
    if (m_count < m_maximum_count)
        m_count++;
}

bool RecentLossCounter::is_at_maximum(MonotonicTime now)
{
    decay(now);
    return m_count >= m_maximum_count;
}

}
