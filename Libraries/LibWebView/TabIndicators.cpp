/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Timer.h>
#include <LibWebView/TabIndicators.h>

namespace WebView {

TabIndicators::TabIndicators()
    : m_timer(Core::Timer::create_single_shot(AK::Duration::zero(), [this] {
        // The deadlines, not the timer's resolution, decide when an indicator stops showing.
        update_visibility(MonotonicTime::now());
    }))
{
}

TabIndicators::~TabIndicators() = default;

void TabIndicators::set_active(TabIndicator indicator, bool active, MonotonicTime now)
{
    auto& lingering = entry(indicator).lingering;
    lingering.set_active(active, now);

    // Even when the indicator's activity hasn't changed, another's deadline may have passed without the timer having
    // run yet.
    update_visibility(now);
}

bool TabIndicators::is_active(TabIndicator indicator) const
{
    auto const& lingering = entry(indicator).lingering;
    return lingering.is_active();
}

void TabIndicators::update_visibility(MonotonicTime now)
{
    for (auto& entry : m_entries)
        entry.visible = entry.lingering.is_visible(now);

    // Wake before notifying, so a listener that makes an indicator active again schedules from settled state.
    if (auto deadline = next_deadline(now); deadline.has_value())
        m_timer->restart(*deadline - now);
    else
        m_timer->stop();

    // A listener that changes an indicator reports it from a nested update, so an indicator is reported only while what
    // was last reported for it is out of date.
    for (auto& entry : m_entries) {
        if (entry.reported_visible == entry.visible)
            continue;

        entry.reported_visible = entry.visible;
        if (on_visibility_change)
            on_visibility_change(entry.indicator, entry.visible);
    }
}

TabIndicators::Entry& TabIndicators::entry(TabIndicator indicator)
{
    for (auto& entry : m_entries) {
        if (entry.indicator == indicator)
            return entry;
    }
    VERIFY_NOT_REACHED();
}

Optional<MonotonicTime> TabIndicators::next_deadline(MonotonicTime now) const
{
    Optional<MonotonicTime> next_deadline;
    for (auto const& entry : m_entries) {
        auto deadline = entry.lingering.visible_until();
        if (!deadline.has_value() || *deadline <= now)
            continue;
        if (!next_deadline.has_value() || *deadline < *next_deadline)
            next_deadline = deadline;
    }
    return next_deadline;
}

}
