/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/OwnPtr.h>
#include <LibCore/Event.h>
#include <LibCore/Export.h>
#include <LibCore/Forward.h>

namespace Core {

// Per-thread global event queue. This is where events are queued for the EventLoop to process.
// There is only one ThreadEventQueue per thread, and it is accessed via ThreadEventQueue::current().
// It is allowed to post events to other threads' event queues.
class CORE_API ThreadEventQueue {
    AK_MAKE_NONCOPYABLE(ThreadEventQueue);
    AK_MAKE_NONMOVABLE(ThreadEventQueue);

public:
    static ThreadEventQueue* current_or_null();
    static ThreadEventQueue& current();

    // Process all queued events. Returns the number of events that were processed.
    size_t process();

    // Posts an event to the event queue.
    void post_event(EventReceiver*, Core::Event::Type);

    // Post a deferred invocation to the event queue.
    void deferred_invoke(Function<void()>&&);

    // Used by Threading::BackgroundAction.
    void add_job(NonnullRefPtr<Promise<NonnullRefPtr<EventReceiver>>>);
    void cancel_all_pending_jobs();

    // Returns true if there are events waiting to be flushed.
    bool has_pending_events() const;

private:
    ThreadEventQueue();
    ~ThreadEventQueue();

    struct Private;
    OwnPtr<Private> m_private;
};

}
