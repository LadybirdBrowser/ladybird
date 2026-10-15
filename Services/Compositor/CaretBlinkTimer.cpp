/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <Compositor/CaretBlinkTimer.h>
#include <LibCompositing/DisplayList/DisplayList.h>
#include <LibCore/Timer.h>

namespace Compositor {

CaretBlinkTimer::CaretBlinkTimer(Function<Gfx::IntRect()> damage_rect_of_blinking_carets, Function<void(Gfx::IntRect)> schedule_repaint)
    : m_damage_rect_of_blinking_carets(move(damage_rect_of_blinking_carets))
    , m_schedule_repaint(move(schedule_repaint))
{
}

CaretBlinkTimer::~CaretBlinkTimer()
{
    if (m_timer) {
        m_timer->on_timeout = {};
        m_timer->stop();
    }
}

void CaretBlinkTimer::did_install_display_list(Compositing::DisplayList const& display_list)
{
    Optional<i64> blink_cycle_start_time_ns;
    display_list.for_each_caret([&](auto, auto, auto const& caret) {
        if (caret.should_blink)
            blink_cycle_start_time_ns = caret.blink_cycle_start_time_ns;
    });

    if (blink_cycle_start_time_ns == m_blink_cycle_start_time_ns)
        return;
    m_blink_cycle_start_time_ns = blink_cycle_start_time_ns;

    if (m_timer)
        m_timer->stop();
    if (!m_blink_cycle_start_time_ns.has_value() || !m_schedule_repaint)
        return;

    if (!m_timer) {
        m_timer = Core::Timer::create_single_shot(500, [this] {
            auto damage_rect = m_damage_rect_of_blinking_carets();
            if (!damage_rect.is_empty())
                m_schedule_repaint(damage_rect);
            schedule_next_blink();
        });
    }
    schedule_next_blink();
}

void CaretBlinkTimer::schedule_next_blink()
{
    if (!m_blink_cycle_start_time_ns.has_value() || !m_timer)
        return;

    auto now_ns = MonotonicTime::now().nanoseconds();
    auto elapsed_ns = now_ns > *m_blink_cycle_start_time_ns
        ? now_ns - *m_blink_cycle_start_time_ns
        : 0;
    auto delay_ns = Compositing::caret_blink_interval_ns - elapsed_ns % Compositing::caret_blink_interval_ns;
    auto delay_ms = max(static_cast<int>((delay_ns + 999'999) / 1'000'000), 1);
    m_timer->restart(delay_ms);
}

}
