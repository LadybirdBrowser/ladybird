/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022, kleines Filmröllchen <malu.bertsch@gmail.com>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/GenericAwaiter.h>
#include <AK/Badge.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Promise.h>
#include <LibCore/ThreadEventQueue.h>

namespace Core {

namespace {

OwnPtr<Vector<EventLoop&>>& event_loop_stack_uninitialized()
{
    thread_local OwnPtr<Vector<EventLoop&>> s_event_loop_stack = nullptr;
    return s_event_loop_stack;
}
Vector<EventLoop&>& event_loop_stack()
{
    auto& the_stack = event_loop_stack_uninitialized();
    if (the_stack == nullptr)
        the_stack = make<Vector<EventLoop&>>();
    return *the_stack;
}

}

EventLoop::EventLoop()
    : m_impl(EventLoopManager::the().make_implementation())
{
    if (event_loop_stack().is_empty()) {
        event_loop_stack().append(*this);
    }
}

EventLoop::~EventLoop()
{
    if (!event_loop_stack().is_empty() && &event_loop_stack().last() == this) {
        event_loop_stack().take_last();
    }
}

bool EventLoop::is_running()
{
    auto& stack = event_loop_stack_uninitialized();
    return stack != nullptr && !stack->is_empty();
}

EventLoop& EventLoop::current()
{
    if (event_loop_stack().is_empty())
        dbgln("No EventLoop is present, unable to return current one!");
    return event_loop_stack().last();
}

void EventLoop::quit(int code)
{
    ThreadEventQueue::current().cancel_all_pending_jobs();
    m_impl->quit(code);
}

bool EventLoop::was_exit_requested()
{
    return m_impl->was_exit_requested();
}

struct EventLoopPusher {
public:
    EventLoopPusher(EventLoop& event_loop)
    {
        event_loop_stack().append(event_loop);
    }
    ~EventLoopPusher()
    {
        event_loop_stack().take_last();
    }
};

int EventLoop::exec()
{
    EventLoopPusher pusher(*this);
    return m_impl->exec();
}

void EventLoop::spin_until(Function<bool()> goal_condition)
{
    EventLoopPusher pusher(*this);
    while (!goal_condition())
        pump();
}

size_t EventLoop::pump(WaitMode mode)
{
    return m_impl->pump(mode == WaitMode::WaitForEvents ? EventLoopImplementation::PumpMode::WaitForEvents : EventLoopImplementation::PumpMode::DontWaitForEvents);
}

void EventLoop::post_event(EventReceiver& receiver, NonnullOwnPtr<Event>&& event)
{
    m_impl->post_event(receiver, move(event));
}

void EventLoop::add_job(NonnullRefPtr<Promise<NonnullRefPtr<EventReceiver>>> job_promise)
{
    ThreadEventQueue::current().add_job(move(job_promise));
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

void EventLoop::wake()
{
    m_impl->wake();
}

Coroutine<void> EventLoop::next_turn()
{
    (void)co_await GenericAwaiter{[this](auto ready) mutable {
        deferred_invoke(move(ready));
    }};
}

void EventLoop::adopt_coroutine(Coroutine<void>&& coroutine, SourceLocation location)
{
    class OrphanedCoroutine {
        struct PromiseType;

    public:
        using promise_type = PromiseType;

    private:
        struct Destroyer {
            bool await_ready() const noexcept { return false; }
            void await_suspend(std::coroutine_handle<> handle) const noexcept { handle.destroy(); }
            void await_resume() const noexcept { }
        };

        struct PromiseType {
            OrphanedCoroutine get_return_object() { return {}; }
            AK::Detail::SuspendNever initial_suspend() { return {}; }
            Destroyer final_suspend() noexcept { return {}; }
            void return_void() { }
        };
    };

    (void)location;
    [](Coroutine<void>&& coroutine) mutable -> OrphanedCoroutine {
        auto saved_coroutine = move(coroutine);
        co_await saved_coroutine;
    }(move(coroutine));
}

void EventLoop::deferred_invoke(Function<void()> invokee)
{
    auto context = DeferredInvocationContext::construct();
    post_event(context, make<Core::DeferredInvocationEvent>(context, move(invokee)));
}

void deferred_invoke(Function<void()> invokee)
{
    EventLoop::current().deferred_invoke(move(invokee));
}

}
