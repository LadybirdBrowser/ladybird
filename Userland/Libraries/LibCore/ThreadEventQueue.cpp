/*
 * Copyright (c) 2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
#include <LibCore/DeferredInvocationContext.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Promise.h>
#include <LibCore/ThreadEventQueue.h>
#include <LibThreading/Mutex.h>
#include <errno.h>
#include <pthread.h>

namespace Core {

struct ThreadEventQueue::Private {
    struct QueuedEvent {
        AK_MAKE_NONCOPYABLE(QueuedEvent);
        AK_MAKE_DEFAULT_MOVABLE(QueuedEvent);

    public:
        QueuedEvent(EventReceiver& receiver, NonnullOwnPtr<Event> event)
            : receiver(receiver)
            , event(move(event))
        {
        }

        ~QueuedEvent() = default;

        WeakPtr<EventReceiver> receiver;
        NonnullOwnPtr<Event> event;
    };

    Threading::Mutex mutex;
    Vector<QueuedEvent, 128> queued_events;
    Vector<NonnullRefPtr<Promise<NonnullRefPtr<EventReceiver>>>, 16> pending_promises;
    bool warned_promise_count { false };
};

static pthread_key_t s_current_thread_event_queue_key;
static pthread_once_t s_current_thread_event_queue_key_once = PTHREAD_ONCE_INIT;

ThreadEventQueue& ThreadEventQueue::current()
{
    pthread_once(&s_current_thread_event_queue_key_once, [] {
        pthread_key_create(&s_current_thread_event_queue_key, [](void* value) {
            if (value)
                delete static_cast<ThreadEventQueue*>(value);
        });
    });

    auto* ptr = static_cast<ThreadEventQueue*>(pthread_getspecific(s_current_thread_event_queue_key));
    if (!ptr) {
        ptr = new ThreadEventQueue;
        pthread_setspecific(s_current_thread_event_queue_key, ptr);
    }
    return *ptr;
}

ThreadEventQueue::ThreadEventQueue()
    : m_private(make<Private>())
{
}

ThreadEventQueue::~ThreadEventQueue() = default;

void ThreadEventQueue::post_event(Core::EventReceiver& receiver, NonnullOwnPtr<Core::Event> event)
{
    {
        Threading::MutexLocker lock(m_private->mutex);
        m_private->queued_events.empend(receiver, move(event));
    }
    Core::EventLoopManager::the().did_post_event();
}

void ThreadEventQueue::add_job(NonnullRefPtr<Promise<NonnullRefPtr<EventReceiver>>> promise)
{
    Threading::MutexLocker lock(m_private->mutex);
    m_private->pending_promises.append(move(promise));
}

void ThreadEventQueue::cancel_all_pending_jobs()
{
    Threading::MutexLocker lock(m_private->mutex);
    for (auto const& promise : m_private->pending_promises)
        promise->reject(Error::from_errno(ECANCELED));

    m_private->pending_promises.clear();
}

size_t ThreadEventQueue::process()
{
    decltype(m_private->queued_events) events;
    {
        Threading::MutexLocker locker(m_private->mutex);
        events = move(m_private->queued_events);
        m_private->pending_promises.remove_all_matching([](auto& job) { return job->is_resolved() || job->is_rejected(); });
    }

    size_t processed_events = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        auto& queued_event = events.at(i);
        auto receiver = queued_event.receiver.strong_ref();
        auto& event = *queued_event.event;

        if (!receiver) {
            switch (event.type()) {
            case Event::Quit:
                VERIFY_NOT_REACHED();
            default:
                // Receiver disappeared, drop the event on the floor.
                break;
            }
        } else if (event.type() == Event::Type::DeferredInvoke) {
            static_cast<DeferredInvocationEvent&>(event).m_invokee();
        } else {
            NonnullRefPtr<EventReceiver> protector(*receiver);
            receiver->dispatch_event(event);
        }
        ++processed_events;
    }

    {
        Threading::MutexLocker locker(m_private->mutex);
        if (m_private->pending_promises.size() > 30 && !m_private->warned_promise_count) {
            m_private->warned_promise_count = true;
            dbgln("ThreadEventQueue::process: Job queue wasn't designed for this load ({} promises)", m_private->pending_promises.size());
        }
    }
    return processed_events;
}

bool ThreadEventQueue::has_pending_events() const
{
    Threading::MutexLocker locker(m_private->mutex);
    return !m_private->queued_events.is_empty();
}

}
