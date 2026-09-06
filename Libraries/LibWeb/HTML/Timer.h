/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/WeakPtr.h>
#include <LibCore/Forward.h>
#include <LibGC/Function.h>
#include <LibGC/Ptr.h>
#include <LibJS/Heap/Cell.h>
#include <LibWeb/Forward.h>

namespace Web::HTML {

// How a hidden document treats the timer: it leaves an immediate one alone, holds a delayed one back until the next of
// the wake-ups it lets its timers run at, and holds a chained one — a timer the page re-armed from a timer callback,
// or a timeout another specification waits on — back to a much rarer wake-up once it has been hidden for a while.
enum class TimerThrottlingClass : u8 {
    Immediate,
    Delayed,
    Chained,
};

class Timer final : public JS::Cell {
    GC_CELL(Timer, JS::Cell);
    GC_DECLARE_ALLOCATOR(Timer);

public:
    enum class Repeating {
        No,
        Yes,
    };

    static constexpr bool OVERRIDES_FINALIZE = true;

    static GC::Ref<Timer> create(i32 milliseconds, Function<void()> callback, i32 id, Repeating, TimerThrottlingClass, double deadline);

    void start();
    void stop();
    void restart(i32 milliseconds);
    bool is_active() const;

    void set_callback(Function<void()>);
    void set_interval(i32 milliseconds);

    TimerThrottlingClass throttling_class() const { return m_throttling_class; }
    void set_throttling_class(TimerThrottlingClass throttling_class) { m_throttling_class = throttling_class; }

    // When the timer would fire if nothing held it back: a moment on the monotonic clock, in milliseconds.
    double deadline() const { return m_deadline; }
    void set_deadline(double deadline) { m_deadline = deadline; }

private:
    Timer(i32 milliseconds, Function<void()> callback, i32 id, Repeating, TimerThrottlingClass, double deadline);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    RefPtr<Core::Timer> m_timer;
    i32 m_id { 0 };
    TimerThrottlingClass m_throttling_class { TimerThrottlingClass::Delayed };
    double m_deadline { 0 };
};

}
