/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Atomic.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Promise.h>
#include <LibThreading/Forward.h>

namespace Threading {

template<typename Result>
class BackgroundAction;

class BackgroundActionBase {
    template<typename Result>
    friend class BackgroundAction;

private:
    BackgroundActionBase() = default;

    static void enqueue_work(ESCAPING Function<void()>);
    static Thread& background_thread();
};

template<typename Result>
class BackgroundAction final
    : public Core::EventReceiver
    , private BackgroundActionBase {
    C_OBJECT(BackgroundAction);

public:
    virtual ~BackgroundAction() = default;

    Optional<Result> const& result() const { return m_result; }
    Optional<Result>& result() { return m_result; }

    // Cancellation is a best-effort cross-thread signal. No other state is protected by this flag.
    // It is not used to synchronize access to any other state (m_result), so relaxed atomics are fine.
    void cancel() { m_canceled.store(true, AK::MemoryOrder::memory_order_relaxed); }
    // If your action is long-running, you should periodically check the cancel state and possibly return early.
    bool is_canceled() const { return m_canceled.load(AK::MemoryOrder::memory_order_relaxed); }

private:
    BackgroundAction(ESCAPING Function<ErrorOr<Result>(BackgroundAction&)> action, ESCAPING Function<void(Result)> on_complete, ESCAPING Function<void(Error)> on_error = {})
        : m_action(move(action))
        , m_on_complete(move(on_complete))
        , m_on_error(move(on_error))
    {
        enqueue_work([self = NonnullRefPtr(*this), origin_event_loop = Core::EventLoop::current_weak()]() mutable {
            auto result = self->m_action(*self);

            auto event_loop = origin_event_loop->take();
            if (!event_loop) {
                dbgln("BackgroundAction {:p} was dropped, the origin loop is gone.", self.ptr());
                return;
            }
            event_loop->deferred_invoke([self = move(self), result = move(result)]() mutable {
                auto const canceled = self->m_canceled.load(AK::MemoryOrder::memory_order_relaxed);

                if (canceled)
                    return;

                if (result.is_error()) {
                    if (self->m_on_error)
                        self->m_on_error(result.release_error());
                    return;
                }

                if (self->m_on_complete)
                    self->m_on_complete(result.release_value());
            });
        });
    }

    Function<ErrorOr<Result>(BackgroundAction&)> m_action;
    Function<void(Result)> m_on_complete;
    Function<void(Error)> m_on_error;
    Optional<Result> m_result;
    Atomic<bool> m_canceled { false };
};

void quit_background_thread();

}
