/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Forward.h>
#include <AK/Function.h>
#include <AK/Time.h>
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

    using Callback = GC::Ref<GC::Function<void()>>;

    static GC::Ref<Timer> create(i32 milliseconds, Callback, i32 id, Repeating, TimerThrottlingClass, double deadline);

    void start();
    void stop();
    void restart(i32 milliseconds);
    bool is_active() const;

    // Runs what the timer runs when it fires, without waiting for the clock.
    void fire_now();

    void set_callback(Callback callback) { m_callback = callback; }
    void set_interval(i32 milliseconds);

    TimerThrottlingClass throttling_class() const { return m_throttling_class; }
    void set_throttling_class(TimerThrottlingClass throttling_class) { m_throttling_class = throttling_class; }

    // When the timer would fire if nothing held it back: a moment on the monotonic clock, in milliseconds.
    double deadline() const { return m_deadline; }
    void set_deadline(double deadline) { m_deadline = deadline; }

    // Claims a firing of the timer for a task to run. A repeating timer keeps to its schedule, so it can fire again
    // before the task its last firing queued has run, and a firing posted before a restart can still arrive after it;
    // neither queues a task. Running the task releases the claim.
    [[nodiscard]] bool claim_firing();
    void release_firing() { m_has_queued_firing = false; }
    bool has_queued_firing() const { return m_has_queued_firing; }

private:
    Timer(i32 milliseconds, Callback, i32 id, Repeating, TimerThrottlingClass, double deadline);

    virtual void visit_edges(Cell::Visitor&) override;
    virtual void finalize() override;

    RefPtr<Core::Timer> m_timer;
    Callback m_callback;
    i32 m_id { 0 };
    TimerThrottlingClass m_throttling_class { TimerThrottlingClass::Delayed };
    double m_deadline { 0 };
    MonotonicTime m_earliest_firing;
    bool m_has_queued_firing { false };
};

}
