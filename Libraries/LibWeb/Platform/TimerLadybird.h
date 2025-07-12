/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibCore/Forward.h>
#include <LibWeb/Platform/Timer.h>

namespace Web::Platform {

class TimerLadybird final : public Timer {
    GC_CELL(TimerLadybird, Timer);

public:
    static GC::Ref<TimerLadybird> create(GC::Heap&);

    virtual ~TimerLadybird();

    virtual void start() override;
    virtual void start(int interval_ms) override;
    virtual void restart() override;
    virtual void restart(int interval_ms) override;
    virtual void stop() override;

    virtual void set_active(bool) override;

    virtual bool is_active() const override;
    virtual int interval() const override;
    virtual void set_interval(int interval_ms) override;

    virtual bool is_single_shot() const override;
    virtual void set_single_shot(bool) override;

private:
    TimerLadybird();

    NonnullRefPtr<Core::Timer> m_timer;
};

}
