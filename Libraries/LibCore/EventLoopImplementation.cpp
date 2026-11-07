/*
 * Copyright (c) 2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Atomic.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibCore/ThreadEventQueue.h>
#ifdef AK_OS_WINDOWS
#    include <LibCore/EventLoopImplementationWindows.h>
#else
#    include <LibCore/EventLoopImplementationUnix.h>
#endif

namespace Core {

EventLoopImplementation::EventLoopImplementation()
    : m_thread_event_queue(ThreadEventQueue::current())
{
}

EventLoopImplementation::~EventLoopImplementation() = default;

void EventLoopImplementation::request_exit(int code)
{
    m_exit_code.store(code, AK::MemoryOrder::memory_order_relaxed);
    m_exit_requested.store(true, AK::MemoryOrder::memory_order_release);
}

bool EventLoopImplementation::was_exit_requested() const
{
    return m_exit_requested.load(AK::MemoryOrder::memory_order_acquire);
}

Optional<int> EventLoopImplementation::exit_code_if_requested() const
{
    if (!m_exit_requested.load(AK::MemoryOrder::memory_order_acquire))
        return {};
    return m_exit_code.load(AK::MemoryOrder::memory_order_relaxed);
}

void EventLoopImplementation::deferred_invoke(Function<void()>&& invokee)
{
    m_thread_event_queue.deferred_invoke(move(invokee));
    if (&m_thread_event_queue != ThreadEventQueue::current_or_null())
        wake();
}

static Atomic<EventLoopManager*> s_event_loop_manager { nullptr };
EventLoopManager& EventLoopManager::the()
{
    if (auto* manager = s_event_loop_manager.load(AK::memory_order_acquire))
        return *manager;

    // NB: Threads can get here at the same time, and the first one to store its manager wins.
    auto* manager = new EventLoopManagerPlatform;
    EventLoopManager* existing_manager = nullptr;
    if (s_event_loop_manager.compare_exchange_strong(existing_manager, manager, AK::memory_order_acq_rel))
        return *manager;
    delete manager;
    return *existing_manager;
}

void EventLoopManager::install(Core::EventLoopManager& manager)
{
    EventLoopManager* existing_manager = nullptr;
    auto did_install = s_event_loop_manager.compare_exchange_strong(existing_manager, &manager, AK::memory_order_acq_rel);
    VERIFY(did_install);
}

EventLoopManager::EventLoopManager() = default;

EventLoopManager::~EventLoopManager() = default;

}
