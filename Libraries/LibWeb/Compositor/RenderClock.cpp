/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Math.h>
#include <Compositor/RenderClockClientEndpoint.h>
#include <Compositor/RenderClockServerEndpoint.h>
#include <LibCore/EventLoop.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Compositor/RenderClock.h>
#include <LibWeb/Layout/LayoutRustFFI.h>

namespace Web::Compositor {

// LibIPC pins a connection to the thread that makes it, so the clock thread makes and destroys every channel.
class RenderClockChannel final : public IPC::ConnectionToServer<RenderClockClientEndpoint, RenderClockServerEndpoint> {
public:
    RenderClockChannel(NonnullOwnPtr<IPC::Transport> transport, RenderClock& clock)
        : IPC::ConnectionToServer<RenderClockClientEndpoint, RenderClockServerEndpoint>(*this, move(transport))
        , m_clock(clock)
    {
    }

    // A channel the clock let go of on purpose reports nothing more, not even its own death.
    void detach() { m_detached = true; }

private:
    // The Compositor went away. The main thread learns of it on its own connection.
    virtual void die() override
    {
        if (!m_detached)
            m_clock.did_lose_channel();
    }

    virtual void clock_tick(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double) override
    {
        if (!m_detached)
            m_clock.did_receive_clock_tick(context_id, frame_time_nanoseconds);
    }

    RenderClock& m_clock;
    bool m_detached { false };
};

ClockTicksHandle::~ClockTicksHandle()
{
    if (m_ticks)
        Layout::RustFFI::clock_ticks_release(m_ticks);
}

bool ClockTicksHandle::tick(i64 frame_time_nanoseconds) const
{
    return Layout::RustFFI::clock_ticks_tick(m_ticks, frame_time_nanoseconds);
}

RenderClock& RenderClock::the()
{
    // The clock lives as long as the process, which ends with its thread still waiting for ticks.
    static RenderClock& clock = *new RenderClock;
    return clock;
}

RenderClock::RenderClock()
    : m_thread(Threading::Thread::construct("RenderClock"sv, [this] {
        return thread_main();
    }))
{
    m_thread->start();
    MutexLocker locker(m_mutex);
    m_condition.wait_while([&] { return !m_started; });
}

intptr_t RenderClock::thread_main()
{
    Core::EventLoop event_loop;
    {
        MutexLocker locker(m_mutex);
        m_event_loop = Core::EventLoop::current_weak();
        m_started = true;
        m_condition.broadcast();
    }

    auto result = event_loop.exec();

    // What is still queued on this thread runs before it returns: a connection's deferred invocations hold references
    // to it, and the last one to go would destroy the channel as the thread exits.
    do {
        drop_channel();
    } while (Core::ThreadEventQueue::current().process() > 0);

    MutexLocker locker(m_mutex);
    m_event_loop.clear();
    return result;
}

bool RenderClock::invoke_on_clock_thread(Function<void()> function)
{
    RefPtr<Core::WeakEventLoopReference> event_loop;
    {
        MutexLocker locker(m_mutex);
        event_loop = m_event_loop;
    }
    if (!event_loop)
        return false;
    auto strong_event_loop = event_loop->take();
    if (!strong_event_loop)
        return false;
    strong_event_loop->deferred_invoke(move(function));
    return true;
}

ErrorOr<IPC::TransportHandle> RenderClock::attach()
{
    Mutex result_mutex;
    ConditionVariable result_condition { result_mutex };
    Optional<ErrorOr<IPC::TransportHandle>> result;

    auto invoked = invoke_on_clock_thread([&] {
        auto handle_or_error = replace_channel();
        MutexLocker locker(result_mutex);
        result = move(handle_or_error);
        result_condition.broadcast();
    });
    if (!invoked)
        return Error::from_string_literal("RenderClock thread is gone");

    // The clock thread goes away only with this RenderClock, so it runs what was just handed to it.
    MutexLocker locker(result_mutex);
    result_condition.wait_while([&] { return !result.has_value(); });
    return result.release_value();
}

ErrorOr<IPC::TransportHandle> RenderClock::replace_channel()
{
    // A new channel starts with nothing armed: the contexts armed on the previous one were armed for a Compositor that
    // is gone.
    drop_channel();
    auto paired = TRY(IPC::Transport::create_paired());
    m_channel = adopt_ref(*new RenderClockChannel(move(paired.local), *this));
    return move(paired.remote_handle);
}

void RenderClock::drop_channel()
{
    m_armed_contexts.clear();
    if (auto channel = move(m_channel)) {
        channel->detach();
        channel->shutdown();
    }
}

void RenderClock::arm(Web::CompositorContextId context_id, double maximum_frames_per_second, OnTick on_tick)
{
    VERIFY(isfinite(maximum_frames_per_second) && maximum_frames_per_second > 0);
    (void)invoke_on_clock_thread([this, context_id, maximum_frames_per_second, on_tick = move(on_tick)]() mutable {
        m_armed_contexts.set(context_id, ArmedContext { maximum_frames_per_second, move(on_tick) });
        request_clock_tick(context_id, maximum_frames_per_second);
    });
}

void RenderClock::request_clock_tick(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (m_channel)
        m_channel->async_request_clock_tick(context_id, maximum_frames_per_second);
}

void RenderClock::did_receive_clock_tick(Web::CompositorContextId context_id, i64 frame_time_nanoseconds)
{
    // A tick for a context disarmed after its request went out, or armed on an earlier channel.
    auto it = m_armed_contexts.find(context_id);
    if (it == m_armed_contexts.end())
        return;

    // Handing a tick on only queues it, so the next request goes out right after: however long this tick takes, the next
    // one is already on its way, and the Compositor's pacing is the only limit on the rate.
    if (!it->value.on_tick(frame_time_nanoseconds)) {
        m_armed_contexts.remove(it);
        return;
    }
    request_clock_tick(context_id, it->value.maximum_frames_per_second);
}

void RenderClock::did_lose_channel()
{
    m_armed_contexts.clear();
    // The channel is dying from inside its own handler, which keeps it alive until it returns.
    m_channel = nullptr;
}

}
