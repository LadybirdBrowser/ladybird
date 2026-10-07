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

    virtual void clock_tick(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, double, Vector<Web::CompositorScrollOffset> scroll_offsets) override
    {
        if (!m_detached)
            m_clock.did_receive_clock_tick(context_id, frame_time_nanoseconds, scroll_offsets);
    }

    virtual void pointer_moved(Web::CompositorContextId context_id, Web::DevicePixelPoint position, u32 buttons, bool scrolled_since_frame) override
    {
        if (!m_detached)
            m_clock.did_receive_pointer_move(context_id, Gfx::FloatPoint { position.x().value(), position.y().value() }, buttons, scrolled_since_frame);
    }

    virtual void pointer_left(Web::CompositorContextId context_id) override
    {
        if (!m_detached)
            m_clock.did_receive_pointer_move(context_id, {}, 0, false);
    }

    RenderClock& m_clock;
    bool m_detached { false };
};

ClockTicksHandle::~ClockTicksHandle()
{
    if (m_ticks)
        Layout::RustFFI::clock_ticks_release(m_ticks);
}

bool ClockTicksHandle::tick(i64 frame_time_nanoseconds, ReadonlySpan<Web::CompositorScrollOffset> scroll_offsets) const
{
    // A scroll timeline follows the scroll node of an element or a document's viewport, which its unique id names.
    Vector<Layout::RustFFI::FfiScrollOffset, 8> scrollers;
    for (auto const& scroll_offset : scroll_offsets) {
        if (scroll_offset.scroll_node.kind == Web::AsyncScrollNodeKind::PseudoElement)
            continue;
        scrollers.append({ scroll_offset.scroll_node.node_id.value(), scroll_offset.offset.x().to_double(), scroll_offset.offset.y().to_double() });
    }
    return Layout::RustFFI::clock_ticks_tick(m_ticks, frame_time_nanoseconds, scrollers.data(), scrollers.size());
}

PointerAnswer ClockTicksHandle::pointer_moved(Optional<Gfx::FloatPoint> device_position, u32 buttons, bool scrolled_since_frame) const
{
    auto position = device_position.value_or({});
    return static_cast<PointerAnswer>(Layout::RustFFI::clock_ticks_pointer_moved(m_ticks, device_position.has_value(), position.x(), position.y(), buttons, scrolled_since_frame));
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
    m_pointer_lanes.clear();
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

void RenderClock::arm_lane(Web::CompositorContextId context_id, double maximum_frames_per_second, NonnullRefPtr<ClockTicksHandle> ticks, Optional<i64> tick_now_at)
{
    VERIFY(isfinite(maximum_frames_per_second) && maximum_frames_per_second > 0);
    (void)invoke_on_clock_thread([this, context_id, maximum_frames_per_second, ticks = move(ticks), tick_now_at]() mutable {
        // Another document of the context hears of the pointer no more, and a tick armed for it would hand its ticks to
        // this one's.
        if (auto it = m_pointer_lanes.find(context_id); it != m_pointer_lanes.end() && !it->value.ticks->hands_on_to(*ticks))
            m_armed_contexts.remove(context_id);
        // The display ticks of an armed context drive its ticks at their rate, however many tasks begin meanwhile.
        if (tick_now_at.has_value() && !m_armed_contexts.contains(context_id)) {
            (void)ticks->tick(*tick_now_at, {});
            m_armed_contexts.set(context_id, ArmedContext { maximum_frames_per_second, [ticks](i64 frame_time_nanoseconds, ReadonlySpan<Web::CompositorScrollOffset> scroll_offsets) { return ticks->tick(frame_time_nanoseconds, scroll_offsets); } });
            request_clock_tick(context_id, maximum_frames_per_second);
        }
        m_pointer_lanes.set(context_id, PointerLane { maximum_frames_per_second, move(ticks) });
    });
}

void RenderClock::forget_context(Web::CompositorContextId context_id)
{
    (void)invoke_on_clock_thread([this, context_id] {
        m_armed_contexts.remove(context_id);
        m_pointer_lanes.remove(context_id);
    });
}

void RenderClock::did_receive_pointer_move(Web::CompositorContextId context_id, Optional<Gfx::FloatPoint> device_position, u32 buttons, bool scrolled_since_frame)
{
    auto it = m_pointer_lanes.find(context_id);
    if (it == m_pointer_lanes.end())
        return;
    switch (it->value.ticks->pointer_moved(device_position, buttons, scrolled_since_frame)) {
    case PointerAnswer::Moves:
        return;
    case PointerAnswer::Ticks:
        break;
    }
    // A context armed for ticks already asked for the next one.
    if (m_armed_contexts.contains(context_id))
        return;
    auto ticks = it->value.ticks;
    auto maximum_frames_per_second = it->value.maximum_frames_per_second;
    m_armed_contexts.set(context_id, ArmedContext { maximum_frames_per_second, [ticks](i64 frame_time_nanoseconds, ReadonlySpan<Web::CompositorScrollOffset> scroll_offsets) { return ticks->tick(frame_time_nanoseconds, scroll_offsets); } });
    request_clock_tick(context_id, maximum_frames_per_second);
}

void RenderClock::request_clock_tick(Web::CompositorContextId context_id, double maximum_frames_per_second)
{
    if (m_channel)
        m_channel->async_request_clock_tick(context_id, maximum_frames_per_second);
}

void RenderClock::did_receive_clock_tick(Web::CompositorContextId context_id, i64 frame_time_nanoseconds, ReadonlySpan<Web::CompositorScrollOffset> scroll_offsets)
{
    // A tick for a context disarmed after its request went out, or armed on an earlier channel.
    auto it = m_armed_contexts.find(context_id);
    if (it == m_armed_contexts.end())
        return;

    // Handing a tick on only queues it, so the next request goes out right after: however long this tick takes, the next
    // one is already on its way, and the Compositor's pacing is the only limit on the rate.
    if (!it->value.on_tick(frame_time_nanoseconds, scroll_offsets)) {
        m_armed_contexts.remove(it);
        return;
    }
    request_clock_tick(context_id, it->value.maximum_frames_per_second);
}

void RenderClock::did_lose_channel()
{
    m_armed_contexts.clear();
    m_pointer_lanes.clear();
    // The channel is dying from inside its own handler, which keeps it alive until it returns.
    m_channel = nullptr;
}

}
