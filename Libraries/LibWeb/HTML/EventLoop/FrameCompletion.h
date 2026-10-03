/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/Mutex.h>
#include <AK/Noncopyable.h>
#include <AK/RefPtr.h>
#include <LibCore/Forward.h>

namespace Web::HTML {

// The completion of a frame that ran beside the event loop. The thread that finishes the frame posts it; the thread
// that registered receives it through its own Core event loop, which runs the delivery steps whether the page is
// visible, idle or waiting for a rendering opportunity. Posts between two deliveries coalesce into one.
class FrameCompletion {
    AK_MAKE_NONCOPYABLE(FrameCompletion);
    AK_MAKE_NONMOVABLE(FrameCompletion);

public:
    AK_ALLOC_WITH_KMALLOC;

    static FrameCompletion& the();

    FrameCompletion() = default;

    // Registering thread only. Takes the current thread's Core event loop. Registering again only replaces the
    // delivery steps.
    void register_event_loop(Function<void()> deliver);

    // Any thread.
    void post();

private:
    void deliver();

    Mutex m_mutex;
    RefPtr<Core::WeakEventLoopReference> m_event_loop;
    Function<void()> m_deliver;
    bool m_delivery_queued { false };
};

}
