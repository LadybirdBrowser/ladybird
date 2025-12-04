/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Export.h>

namespace Core {

class CORE_API Timer final : public EventReceiver {
    C_OBJECT(Timer);

public:
    static NonnullRefPtr<Timer> create();
    static NonnullRefPtr<Timer> create_repeating(int interval_ms, Function<void()>&& timeout_handler);
    static NonnullRefPtr<Timer> create_single_shot(int interval_ms, Function<void()>&& timeout_handler);

    virtual ~Timer() override;

    void start();
    void start(int interval_ms);
    void restart();
    void restart(int interval_ms);
    void stop();

    void set_active(bool);

    bool is_active() const { return m_active; }
    int interval() const { return m_interval_ms; }
    void set_interval(int interval_ms)
    {
        if (m_interval_ms == interval_ms)
            return;
        m_interval_ms = interval_ms;
        m_interval_dirty = true;
    }

    bool is_single_shot() const { return m_single_shot; }
    void set_single_shot(bool single_shot) { m_single_shot = single_shot; }

    Function<void()> on_timeout;

private:
    Timer();
    Timer(int interval_ms, Function<void()>&& timeout_handler);

    virtual void timer_event(TimerEvent&) override;

    bool m_active { false };
    bool m_single_shot { false };
    bool m_interval_dirty { false };
    int m_interval_ms { 0 };
};

}
