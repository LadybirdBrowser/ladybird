/*
 * Copyright (c) 2026, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Math.h>
#include <AK/Platform.h>
#include <Compositor/VSyncScheduler.h>
#include <LibCore/Timer.h>

#if defined(AK_OS_MACOS)
#    include <AK/Atomic.h>
#    include <AK/AtomicRefCounted.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/RefPtr.h>
#    include <CoreGraphics/CoreGraphics.h>
#    include <CoreVideo/CoreVideo.h>
#    include <LibCore/EventLoop.h>
#endif

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

#if defined(AK_OS_MACOS)

static constexpr int display_link_idle_stop_delay_ms = 250;

static CVReturn display_link_callback(CVDisplayLinkRef, CVTimeStamp const*, CVTimeStamp const*, CVOptionFlags, CVOptionFlags*, void* context);

class DisplayLinkState final : public AtomicRefCounted<DisplayLinkState> {
public:
    static RefPtr<DisplayLinkState> create(u64 display_id, Core::EventLoop& event_loop, Function<void()>&& tick_callback)
    {
        CVDisplayLinkRef display_link = nullptr;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
        auto result = CVDisplayLinkCreateWithCGDisplay(static_cast<CGDirectDisplayID>(display_id), &display_link);
#    pragma clang diagnostic pop

        if (result != kCVReturnSuccess || !display_link)
            return nullptr;

        auto state = adopt_ref(*new DisplayLinkState(event_loop, display_link));

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
        result = CVDisplayLinkSetOutputCallback(display_link, display_link_callback, state.ptr());
#    pragma clang diagnostic pop

        if (result != kCVReturnSuccess)
            return nullptr;

        state->tick_callback = move(tick_callback);
        return state;
    }

    ~DisplayLinkState()
    {
        stop_display_link();
    }

    bool request_tick();
    void stop_display_link();
    void schedule_display_link_stop_if_idle();
    void stop_display_link_if_idle();
    bool consume_tick_request();
    bool is_valid() const;

    Core::EventLoop& event_loop;
    Function<void()> tick_callback;
    Atomic<bool> invalidated { false };
    Atomic<bool> tick_requested { false };
    CVDisplayLinkRef display_link { nullptr };
    NonnullRefPtr<Core::Timer> idle_stop_timer;

private:
    DisplayLinkState(Core::EventLoop& event_loop, CVDisplayLinkRef display_link)
        : event_loop(event_loop)
        , display_link(display_link)
        , idle_stop_timer(Core::Timer::create_single_shot(display_link_idle_stop_delay_ms, [this] {
            stop_display_link_if_idle();
        }))
    {
    }
};

bool DisplayLinkState::request_tick()
{
    if (invalidated.load() || !display_link)
        return false;

    tick_requested.store(true);
    idle_stop_timer->stop();

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto is_running = CVDisplayLinkIsRunning(display_link);
#    pragma clang diagnostic pop

    if (is_running)
        return true;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    auto result = CVDisplayLinkStart(display_link);
#    pragma clang diagnostic pop

    if (result == kCVReturnSuccess || result == kCVReturnDisplayLinkAlreadyRunning)
        return true;

    tick_requested.store(false);
    return false;
}

void DisplayLinkState::stop_display_link()
{
    invalidated.store(true);
    tick_requested.store(false);
    idle_stop_timer->on_timeout = {};
    idle_stop_timer->stop();

    if (!display_link)
        return;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkStop(display_link);
#    pragma clang diagnostic pop
    CVDisplayLinkRelease(display_link);
    display_link = nullptr;
}

void DisplayLinkState::schedule_display_link_stop_if_idle()
{
    if (invalidated.load() || tick_requested.load() || !display_link)
        return;

    idle_stop_timer->restart(display_link_idle_stop_delay_ms);
}

void DisplayLinkState::stop_display_link_if_idle()
{
    if (invalidated.load() || tick_requested.load() || !display_link)
        return;

#    pragma clang diagnostic push
#    pragma clang diagnostic ignored "-Wdeprecated-declarations"
    CVDisplayLinkStop(display_link);
#    pragma clang diagnostic pop
}

bool DisplayLinkState::consume_tick_request()
{
    if (invalidated.load())
        return false;

    if (!tick_requested.exchange(false))
        return false;

    return true;
}

bool DisplayLinkState::is_valid() const
{
    return !invalidated.load();
}

class CVDisplayLinkVSyncScheduler final : public VSyncScheduler {
public:
    static OwnPtr<CVDisplayLinkVSyncScheduler> try_create(u64 display_id, Function<void()>&& tick_callback)
    {
        auto state = DisplayLinkState::create(display_id, Core::EventLoop::current(), move(tick_callback));
        if (!state)
            return nullptr;
        return adopt_own(*new CVDisplayLinkVSyncScheduler(state.release_nonnull()));
    }

    explicit CVDisplayLinkVSyncScheduler(NonnullRefPtr<DisplayLinkState> state)
        : m_state(move(state))
    {
    }

    virtual ~CVDisplayLinkVSyncScheduler() override
    {
        m_state->stop_display_link();
    }

    virtual void schedule(double) override
    {
        m_state->request_tick();
    }

private:
    NonnullRefPtr<DisplayLinkState> m_state;
};

static CVReturn display_link_callback(CVDisplayLinkRef, CVTimeStamp const*, CVTimeStamp const*, CVOptionFlags, CVOptionFlags*, void* context)
{
    auto state = NonnullRefPtr { *static_cast<DisplayLinkState*>(context) };
    if (!state->consume_tick_request())
        return kCVReturnSuccess;

    state->event_loop.deferred_invoke([state = move(state)] {
        if (!state->is_valid())
            return;
        state->tick_callback();
        state->schedule_display_link_stop_if_idle();
    });
    return kCVReturnSuccess;
}

OwnPtr<VSyncScheduler> create_vsync_scheduler(Optional<u64> display_id, Function<void()>&& tick_callback)
{
    if (display_id.has_value()) {
        if (auto scheduler = CVDisplayLinkVSyncScheduler::try_create(*display_id, move(tick_callback)); scheduler)
            return scheduler;
    }

    return make<TimerVSyncScheduler>(move(tick_callback));
}

#else

OwnPtr<VSyncScheduler> create_vsync_scheduler(Optional<u64>, Function<void()>&& tick_callback)
{
    return make<TimerVSyncScheduler>(move(tick_callback));
}

#endif

}
