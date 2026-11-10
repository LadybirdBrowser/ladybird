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
#include <LibGC/HeapAccess.h>
#include <LibIPC/ConnectionToServer.h>
#include <LibIPC/Transport.h>
#include <LibThreading/Thread.h>
#include <LibWeb/Compositor/RenderClock.h>
#include <LibWeb/Compositor/RenderClockRustFFI.h>

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

    virtual ErrorOr<OwnPtr<IPC::MessageBuffer>> handle_raw(IPC::RawMessage& message) override
    {
        if (m_detached)
            return nullptr;
        auto outcome = message.hand_to_rust([&](IPC::FfiRawMessage raw_message) {
            RustFFI::render_clock_lanes_handle_message(&m_clock.m_lanes, raw_message, m_clock.is_manual_for_testing());
        });
        if (outcome.misbehavior.has_value()) {
            dbgln("RenderClock: Disconnecting the Compositor after {}: {}", message.message_name(), *outcome.misbehavior);
            shutdown();
        }
        return nullptr;
    }

    RenderClock& m_clock;
    bool m_detached { false };
};

ClockTicksHandle::~ClockTicksHandle()
{
    if (m_ticks)
        Layout::RustFFI::clock_ticks_release(m_ticks);
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
    , m_lanes(*RustFFI::render_clock_lanes_create())
{
    m_thread->start();
    MutexLocker locker(m_mutex);
    m_condition.wait_while([&] { return !m_started; });
}

intptr_t RenderClock::thread_main()
{
    GC::forbid_heap_access_on_this_thread();

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

ErrorOr<void> RenderClock::attach(IPC::TransportHandle handle)
{
    Mutex result_mutex;
    ConditionVariable result_condition { result_mutex };
    Optional<ErrorOr<void>> result;

    auto invoked = invoke_on_clock_thread([&, handle = move(handle)]() mutable {
        auto channel_result = replace_channel(move(handle));
        MutexLocker locker(result_mutex);
        result = move(channel_result);
        result_condition.broadcast();
    });
    if (!invoked)
        return Error::from_string_literal("RenderClock thread is gone");

    // The clock thread goes away only with this RenderClock, so it runs what was just handed to it.
    MutexLocker locker(result_mutex);
    result_condition.wait_while([&] { return !result.has_value(); });
    return result.release_value();
}

ErrorOr<void> RenderClock::replace_channel(IPC::TransportHandle handle)
{
    // A new channel starts with nothing armed: the contexts armed on the previous one were armed for a Compositor that
    // is gone.
    drop_channel();
    auto transport = TRY(handle.create_transport());
    m_channel = adopt_ref(*new RenderClockChannel(move(transport), *this));
    m_poster_of_channel = IPC::ThreadSafeMessagePoster::create(m_channel->transport());
    RustFFI::render_clock_lanes_replace_channel(&m_lanes, m_poster_of_channel->to_ffi_with_one_reference());
    return {};
}

void RenderClock::drop_channel()
{
    RustFFI::render_clock_lanes_drop_channel(&m_lanes);
    if (auto poster = move(m_poster_of_channel))
        poster->detach_from_transport();
    if (auto channel = move(m_channel)) {
        channel->detach();
        channel->shutdown();
    }
}

void RenderClock::arm_lane(Web::CompositorContextId context_id, double maximum_frames_per_second, NonnullRefPtr<ClockTicksHandle> ticks, Optional<i64> tick_now_at)
{
    VERIFY(isfinite(maximum_frames_per_second) && maximum_frames_per_second > 0);
    auto arm = [this, context_id, maximum_frames_per_second, ticks = move(ticks), tick_now_at]() mutable {
        RustFFI::render_clock_lanes_arm_lane(&m_lanes, context_id.value(), maximum_frames_per_second, ticks->ticks(), tick_now_at.has_value(), tick_now_at.value_or(0));
    };
    // A test hands the lanes pointer moves by hand, which find the context armed once the rendering update is over.
    if (m_manual_for_testing)
        run_on_clock_thread_and_wait(move(arm));
    else
        (void)invoke_on_clock_thread(move(arm));
}

void RenderClock::forget_context(Web::CompositorContextId context_id)
{
    (void)invoke_on_clock_thread([this, context_id] {
        RustFFI::render_clock_lanes_forget_context(&m_lanes, context_id.value());
    });
}

// Runs `job` on the clock thread, and waits for it. The clock thread never waits for the caller's, and goes away only with
// this RenderClock, so it runs what was just handed to it.
void RenderClock::run_on_clock_thread_and_wait(Function<void()> job)
{
    Mutex done_mutex;
    ConditionVariable done_condition { done_mutex };
    bool done = false;
    auto invoked = invoke_on_clock_thread([&] {
        job();
        MutexLocker locker(done_mutex);
        done = true;
        done_condition.broadcast();
    });
    if (!invoked)
        return;
    MutexLocker locker(done_mutex);
    done_condition.wait_while([&] { return !done; });
}

void RenderClock::disarm_all_for_testing()
{
    run_on_clock_thread_and_wait([this] { RustFFI::render_clock_lanes_disarm_all(&m_lanes); });
}

void RenderClock::hand_pointer_move(Web::CompositorContextId context_id, Optional<Gfx::FloatPoint> device_position, u32 buttons, bool scrolled_since_frame, u64 input_event_id)
{
    auto pointer_move = [this, context_id, device_position, buttons, scrolled_since_frame, input_event_id] {
        auto position = device_position.value_or({});
        RustFFI::render_clock_lanes_pointer_moved(&m_lanes, context_id.value(), device_position.has_value(), position.x(), position.y(), buttons, scrolled_since_frame, input_event_id, m_manual_for_testing);
    };
    if (m_manual_for_testing)
        run_on_clock_thread_and_wait(move(pointer_move));
    else
        (void)invoke_on_clock_thread(move(pointer_move));
}

void RenderClock::did_lose_channel()
{
    RustFFI::render_clock_lanes_drop_channel(&m_lanes);
    if (auto poster = move(m_poster_of_channel))
        poster->detach_from_transport();
    // The channel is dying from inside its own handler, which keeps it alive until it returns.
    m_channel = nullptr;
}

}

extern "C" void web_render_clock_hand_pointer_move(u64, bool, float, float, u32, bool, u64);

// The lane of a document hands the render clock a pointer move over a same-process iframe's compositor context.
extern "C" void web_render_clock_hand_pointer_move(u64 context_id, bool has_position, float x, float y, u32 buttons, bool scrolled_since_frame, u64 input_event_id)
{
    Optional<Gfx::FloatPoint> position;
    if (has_position)
        position = Gfx::FloatPoint { x, y };
    Web::Compositor::RenderClock::the().hand_pointer_move(Web::CompositorContextId { context_id }, position, buttons, scrolled_since_frame, input_event_id);
}
