/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Math.h>
#include <Compositor/VSyncScheduler.h>
#include <LibCore/Timer.h>

namespace Compositor {

class TimerVSyncScheduler final : public VSyncScheduler {
public:
    explicit TimerVSyncScheduler(Function<void()>&& tick_callback)
        : m_tick_callback(move(tick_callback))
        , m_timer(Core::Timer::create_single_shot(fallback_interval_ms(), [this] {
            fire();
        }))
    {
    }

    virtual ~TimerVSyncScheduler() override
    {
        m_timer->on_timeout = {};
        m_timer->stop();
    }

    virtual void schedule(double refresh_rate) override
    {
        VERIFY(refresh_rate == refresh_rate);
        VERIFY(refresh_rate > 0);
        VERIFY(refresh_rate < AK::Infinity<double>);

        m_refresh_rate = refresh_rate;
        if (m_timer->is_active())
            return;

        m_timer->restart(fallback_interval_ms());
    }

private:
    void fire()
    {
        m_tick_callback();
    }

    int fallback_interval_ms() const
    {
        auto interval = static_cast<int>((1000.0 / m_refresh_rate) + 0.5);
        return interval > 0 ? interval : 1;
    }

    Function<void()> m_tick_callback;
    double m_refresh_rate { 60.0 };
    RefPtr<Core::Timer> m_timer;
};

OwnPtr<VSyncScheduler> create_vsync_scheduler(Optional<u64>, Function<void()>&& tick_callback)
{
    return make<TimerVSyncScheduler>(move(tick_callback));
}

}
