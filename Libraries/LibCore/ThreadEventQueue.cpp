/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Vector.h>
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
        QueuedEvent(RefPtr<EventReceiver> const& receiver, Event::Type event_type)
            : receiver(receiver)
            , event_type(event_type)
        {
        }

        QueuedEvent(Function<void()>&& invokee)
            : m_invokee(move(invokee))
            , event_type(Event::Type::DeferredInvoke)
        {
        }

        ~QueuedEvent() = default;

        WeakPtr<EventReceiver> receiver;
        Function<void()> m_invokee;
        u8 event_type { Event::Type::Invalid };
    };

    Threading::Mutex mutex;
    Vector<QueuedEvent> queued_events;
    Vector<NonnullRefPtr<Promise<NonnullRefPtr<EventReceiver>>>, 16> pending_promises;
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

void ThreadEventQueue::post_event(Core::EventReceiver* receiver, Core::Event::Type event_type)
{
    {
        Threading::MutexLocker lock(m_private->mutex);
        m_private->queued_events.empend(receiver, event_type);
    }
    Core::EventLoopManager::the().did_post_event();
}

void ThreadEventQueue::deferred_invoke(Function<void()>&& invokee)
{
    {
        Threading::MutexLocker lock(m_private->mutex);
        m_private->queued_events.empend(move(invokee));
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

    for (auto& queued_event : events) {
        if (auto receiver = queued_event.receiver.strong_ref()) {
            switch (queued_event.event_type) {
            case Event::Type::Timer: {
                TimerEvent timer_event;
                receiver->dispatch_event(timer_event);
                break;
            }
            case Event::Type::NotifierActivation: {
                NotifierActivationEvent notifier_activation_event;
                receiver->dispatch_event(notifier_activation_event);
                break;
            }
            default:
                VERIFY_NOT_REACHED();
            }
        } else {
            if (queued_event.event_type == Event::Type::DeferredInvoke) {
                queued_event.m_invokee();
            } else {
                // Receiver gone, drop the event.
            }
        }
    }

    return events.size();
}

bool ThreadEventQueue::has_pending_events() const
{
    Threading::MutexLocker locker(m_private->mutex);
    return !m_private->queued_events.is_empty();
}

}
