/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Assertions.h>
#include <AK/Badge.h>
#include <AK/Platform.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Promise.h>
#include <LibCore/ThreadEventQueue.h>

namespace Core {

namespace {

EventLoop*& current_event_loop()
{
    thread_local EventLoop* s_current_event_loop = nullptr;
    return s_current_event_loop;
}

}

EventLoop::EventLoop()
    : m_impl(EventLoopManager::the().make_implementation())
{
    VERIFY(!current_event_loop());
    current_event_loop() = this;
}

EventLoop::~EventLoop()
{
    if (m_weak)
        m_weak->revoke();
    if (current_event_loop() == this)
        current_event_loop() = nullptr;
}

EventLoop& EventLoop::initialize_for_current_thread()
{
    return *new EventLoop;
}

bool EventLoop::is_running()
{
    return current_event_loop() != nullptr;
}

EventLoop& EventLoop::current()
{
    VERIFY(current_event_loop());
    return *current_event_loop();
}

NonnullRefPtr<WeakEventLoopReference> EventLoop::current_weak()
{
    auto& event_loop = current();
    if (!event_loop.m_weak)
        event_loop.m_weak = adopt_ref(*new (nothrow) WeakEventLoopReference(event_loop));
    return *event_loop.m_weak;
}

void EventLoop::quit(int code)
{
    m_impl->quit(code);
}

bool EventLoop::was_exit_requested()
{
    return m_impl->was_exit_requested();
}

int EventLoop::exec()
{
    VERIFY(current_event_loop() == this);
    return m_impl->exec();
}

void EventLoop::spin_until(Function<bool()> goal_condition)
{
    VERIFY(current_event_loop() == this);
    while (!goal_condition())
        pump();
}

size_t EventLoop::pump(WaitMode mode)
{
    VERIFY(current_event_loop() == this);
    return m_impl->pump(mode == WaitMode::WaitForEvents ? EventLoopImplementation::PumpMode::WaitForEvents : EventLoopImplementation::PumpMode::DontWaitForEvents);
}

int EventLoop::register_signal(int signal_number, Function<void(int)> handler)
{
    return EventLoopManager::the().register_signal(signal_number, move(handler));
}

void EventLoop::unregister_signal(int handler_id)
{
    EventLoopManager::the().unregister_signal(handler_id);
}

intptr_t EventLoop::register_timer(EventReceiver& object, int milliseconds, bool should_reload)
{
    return EventLoopManager::the().register_timer(object, milliseconds, should_reload);
}

void EventLoop::unregister_timer(intptr_t timer_id)
{
    EventLoopManager::the().unregister_timer(timer_id);
}

void EventLoop::register_notifier(Badge<Notifier>, Notifier& notifier)
{
    EventLoopManager::the().register_notifier(notifier);
}

void EventLoop::unregister_notifier(Badge<Notifier>, Notifier& notifier)
{
    EventLoopManager::the().unregister_notifier(notifier);
}

void EventLoop::register_process(pid_t pid, ESCAPING Function<void(pid_t)> exit_handler)
{
    EventLoopManager::the().register_process(pid, move(exit_handler));
}

void EventLoop::unregister_process(pid_t pid)
{
    EventLoopManager::the().unregister_process(pid);
}

void EventLoop::wake()
{
    m_impl->wake();
}

void EventLoop::deferred_invoke(Function<void()> invokee)
{
    m_impl->deferred_invoke(move(invokee));
}

void deferred_invoke(Function<void()> invokee)
{
    EventLoop::current().deferred_invoke(move(invokee));
}

WeakEventLoopReference::WeakEventLoopReference(EventLoop& event_loop)
    : m_event_loop(&event_loop)
{
}

void WeakEventLoopReference::revoke()
{
    Sync::RWLockLocker<Sync::LockMode::Write> locker { m_lock };
    m_event_loop = nullptr;
}

StrongEventLoopReference WeakEventLoopReference::take()
{
    return StrongEventLoopReference(*this);
}

StrongEventLoopReference::StrongEventLoopReference(WeakEventLoopReference& event_loop_weak)
{
    event_loop_weak.m_lock.lock_read();
    m_event_loop_weak = &event_loop_weak;
}

StrongEventLoopReference::~StrongEventLoopReference()
{
    m_event_loop_weak->m_lock.unlock_read();
}

bool StrongEventLoopReference::is_alive() const
{
    return m_event_loop_weak->m_event_loop != nullptr;
}

StrongEventLoopReference::operator bool() const
{
    return is_alive();
}

EventLoop* StrongEventLoopReference::operator*() const
{
    VERIFY(is_alive());
    return m_event_loop_weak->m_event_loop;
}

EventLoop* StrongEventLoopReference::operator->() const
{
    VERIFY(is_alive());
    return m_event_loop_weak->m_event_loop;
}

}
