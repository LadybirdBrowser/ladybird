/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/Platform/MemoryPressure.h>
#include <LibCore/Timer.h>
#include <LibWebView/TabDiscarder.h>
#include <LibWebView/ViewImplementation.h>

namespace WebView {

static constexpr auto check_interval = AK::Duration::from_seconds(5);

// A tab that the user saw a short while ago is likely to be one they come back to soon.
static constexpr auto minimum_time_hidden_under_memory_pressure = AK::Duration::from_seconds(10 * 60);

TabDiscarder::TabDiscarder()
    : m_timer(Core::Timer::create_repeating(check_interval, [this] { check_tabs(); }))
{
    m_timer->start();
}

TabDiscarder::~TabDiscarder() = default;

void TabDiscarder::check_tabs()
{
    auto now = MonotonicTime::now();

    // NB: The memory of a discarded tab comes back as its processes exit, so one tab is discarded at a time, and the
    //     next check sees whether the system still needs more. The tab the user saw least recently goes first, but a
    //     tab whose processes all host other tabs too frees little, so one that lets a process exit goes before it.
    if (Core::Platform::system_memory_pressure() == Core::Platform::MemoryPressure::Critical) {
        ViewImplementation* least_recently_seen = nullptr;
        ViewImplementation* least_recently_seen_ending_a_process = nullptr;
        auto seen_less_recently = [](ViewImplementation& view, ViewImplementation* other) {
            return !other || *view.hidden_since() < *other->hidden_since();
        };
        ViewImplementation::for_each_view([&](ViewImplementation& view) {
            if (!view.may_be_discarded(ViewImplementation::DiscardReason::MemoryPressure, minimum_time_hidden_under_memory_pressure, now))
                return IterationDecision::Continue;
            if (seen_less_recently(view, least_recently_seen))
                least_recently_seen = &view;
            if (seen_less_recently(view, least_recently_seen_ending_a_process) && view.discarding_ends_a_process())
                least_recently_seen_ending_a_process = &view;
            return IterationDecision::Continue;
        });
        if (auto* view = least_recently_seen_ending_a_process ? least_recently_seen_ending_a_process : least_recently_seen) {
            dbgln("Discarding tab {}, as the system is about to run out of memory", view->url());
            view->request_discard();
        }
    }
}

}
