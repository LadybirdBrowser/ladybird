/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibIPC/ThreadSafeMessagePoster.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Forward.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Web::Layout::RustFFI {

struct ClockTicks;

}

namespace Web::Compositor::RustFFI {

struct RenderClockLanes;

}

namespace Web::Compositor {

class RenderClockChannel;

// A reference to the ticks of a document's clock lanes, which hands them on to the lane that follows the frame the
// document presented last.
class WEB_API ClockTicksHandle : public RefCounted<ClockTicksHandle> {
    AK_MAKE_NONCOPYABLE(ClockTicksHandle);

public:
    explicit ClockTicksHandle(Layout::RustFFI::ClockTicks const* ticks)
        : m_ticks(ticks)
    {
    }
    ~ClockTicksHandle();

    Layout::RustFFI::ClockTicks const* ticks() const { return m_ticks; }

private:
    Layout::RustFFI::ClockTicks const* m_ticks { nullptr };
};

// A thread of its own that asks the Compositor for display ticks for the contexts that are armed, and hands each tick
// it is delivered to what the context was armed with, without passing through the main thread. There is one per
// process; a Compositor reconnect swaps its channel, not its thread.
//
// The main thread reaches the clock thread only through the calls below. The clock thread owns the channel and the
// armed contexts, and is the only one that hands ticks on.
class WEB_API RenderClock {
    AK_MAKE_NONCOPYABLE(RenderClock);
    AK_MAKE_NONMOVABLE(RenderClock);

public:
    AK_ALLOC_WITH_KMALLOC;

    // The process's render clock, made the first time it is asked for.
    static RenderClock& the();

    // Takes up a channel to the Compositor on the clock thread, in place of the previous one and with nothing armed.
    // Blocks until the channel exists.
    ErrorOr<void> attach(IPC::TransportHandle);

    // Asynchronous. Hands the pointer moves over the context to `ticks` from now on, until another document's ticks take
    // their place or the channel is lost, and arms the context's ticks for them where they ask for them; with
    // `tick_now_at`, ticks them at once at that frame time, where no display tick drives them already. A context is
    // armed until it declines a tick or the channel is lost; a tick that arrives for a context that is not armed is
    // dropped.
    void arm_lane(Web::CompositorContextId, double maximum_frames_per_second, NonnullRefPtr<ClockTicksHandle> ticks, Optional<i64> tick_now_at);

    // Asynchronous. Disarms the context, which is destroyed, and lets go of the ticks its pointer moves went to.
    void forget_context(Web::CompositorContextId);

    // Synchronous. Disarms every context, whose ticks a test injects itself from now on: no display tick reaches them
    // after this returns. For a test.
    void disarm_all_for_testing();

    // Hands the ticks that hear of the pointer moves over the context a move, as the compositor hands them those over a
    // page's context, from the lane of the document around a same-process iframe's. Asynchronous, but for a test that
    // ticks by hand, whose ticks hear of it before this returns.
    void hand_pointer_move(Web::CompositorContextId, Optional<Gfx::FloatPoint> device_position, u32 buttons, bool scrolled_since_frame, u64 input_event_id);

    // Has a test tick the lanes by hand, whose pointer moves then arm no context.
    void set_manual_for_testing(bool manual) { m_manual_for_testing = manual; }
    bool is_manual_for_testing() const { return m_manual_for_testing; }

private:
    friend class RenderClockChannel;

    RenderClock();
    intptr_t thread_main();
    [[nodiscard]] bool invoke_on_clock_thread(Function<void()>);
    void run_on_clock_thread_and_wait(Function<void()>);

    // On the clock thread.
    ErrorOr<void> replace_channel(IPC::TransportHandle);
    void drop_channel();
    void did_lose_channel();

    NonnullRefPtr<Threading::Thread> m_thread;
    Atomic<bool> m_manual_for_testing { false };

    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    bool m_started { false };
    RefPtr<Core::WeakEventLoopReference> m_event_loop;

    // Owned by the clock thread.
    RefPtr<RenderClockChannel> m_channel;
    RefPtr<IPC::ThreadSafeMessagePoster> m_poster_of_channel;
    RustFFI::RenderClockLanes& m_lanes;
};

}
