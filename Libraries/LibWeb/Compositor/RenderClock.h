/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ConditionVariable.h>
#include <AK/Function.h>
#include <AK/HashMap.h>
#include <AK/Mutex.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/Optional.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>
#include <LibGfx/Point.h>
#include <LibIPC/TransportHandle.h>
#include <LibThreading/Forward.h>
#include <LibWeb/Export.h>
#include <LibWebCommon/Page/AsyncScrollNodeStableID.h>
#include <LibWebCommon/Page/CompositorContextId.h>

namespace Web::Layout::RustFFI {

struct ClockTicks;

}

namespace Web::Compositor {

class RenderClockChannel;

// What a clock lease wants once it heard where the pointer went.
enum class PointerAnswer : u8 {
    // Nothing more: the lease has ended.
    Disarm = 0,
    // Display ticks too, until a tick declines the next one.
    Ticks = 2,
};

// A reference to the ticks of a document's clock lease, which hands them on to the lease.
class WEB_API ClockTicksHandle : public RefCounted<ClockTicksHandle> {
    AK_MAKE_NONCOPYABLE(ClockTicksHandle);

public:
    explicit ClockTicksHandle(Layout::RustFFI::ClockTicks const* ticks)
        : m_ticks(ticks)
    {
    }
    ~ClockTicksHandle();

    // Hands the lease a tick, with where the Compositor had scrolled to then, and answers whether it wants the next one.
    bool tick(i64 frame_time_nanoseconds, ReadonlySpan<Web::CompositorScrollOffset>) const;

    // Hands the lease where the pointer went, or that it left, and answers what the lease wants next.
    PointerAnswer pointer_moved(Optional<Gfx::FloatPoint> device_position, u32 buttons, bool scrolled_since_frame) const;

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

    // Runs on the clock thread, for each tick delivered to the context it was armed for, with where the Compositor had
    // scrolled the context's scroll nodes to then, and answers whether the context wants the next tick: one that does
    // not is disarmed.
    using OnTick = Function<bool(i64 frame_time_nanoseconds, ReadonlySpan<Web::CompositorScrollOffset>)>;

    // The process's render clock, made the first time it is asked for.
    static RenderClock& the();

    // Builds a new channel on the clock thread, in place of the previous one and with nothing armed, and returns the
    // end to offer the Compositor. Blocks until the channel exists.
    ErrorOr<IPC::TransportHandle> attach();

    // Asynchronous. A context is armed until it declines a tick or the channel is lost; a tick that arrives for a
    // context that is not armed is dropped. Arming an armed context replaces what it hands ticks to.
    void arm(Web::CompositorContextId, double maximum_frames_per_second, OnTick);

    // Asynchronous. Hands the pointer moves over the context to `ticks` from now on, until it answers Disarm or the
    // channel is lost, and arms the context's ticks for it where it asks for them; with `tick_now`, at once.
    void arm_lease(Web::CompositorContextId, double maximum_frames_per_second, NonnullRefPtr<ClockTicksHandle> ticks, bool tick_now);

private:
    struct ArmedContext {
        double maximum_frames_per_second { 60.0 };
        OnTick on_tick;
    };

    friend class RenderClockChannel;

    RenderClock();
    intptr_t thread_main();
    [[nodiscard]] bool invoke_on_clock_thread(Function<void()>);

    // On the clock thread.
    ErrorOr<IPC::TransportHandle> replace_channel();
    void drop_channel();
    void request_clock_tick(Web::CompositorContextId, double maximum_frames_per_second);
    void did_receive_clock_tick(Web::CompositorContextId, i64 frame_time_nanoseconds, ReadonlySpan<Web::CompositorScrollOffset>);
    void did_receive_pointer_move(Web::CompositorContextId, Optional<Gfx::FloatPoint> device_position, u32 buttons, bool scrolled_since_frame);
    void did_lose_channel();

    NonnullRefPtr<Threading::Thread> m_thread;

    Mutex m_mutex;
    ConditionVariable m_condition { m_mutex };
    bool m_started { false };
    RefPtr<Core::WeakEventLoopReference> m_event_loop;

    // Owned by the clock thread.
    RefPtr<RenderClockChannel> m_channel;
    HashMap<Web::CompositorContextId, ArmedContext> m_armed_contexts;
    struct PointerLease {
        double maximum_frames_per_second { 60.0 };
        NonnullRefPtr<ClockTicksHandle> ticks;
    };
    HashMap<Web::CompositorContextId, PointerLease> m_pointer_leases;
};

}
