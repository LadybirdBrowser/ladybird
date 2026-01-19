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
    // Promise is an implementation detail of BackgroundAction in order to communicate with EventLoop.
    // All of the promise's callbacks and state are either managed by us or by EventLoop.
    using Promise = Core::Promise<NonnullRefPtr<Core::EventReceiver>>;

    virtual ~BackgroundAction() = default;

    Optional<Result> const& result() const { return m_result; }
    Optional<Result>& result() { return m_result; }

    // Cancellation is a best-effort cross-thread signal. No other state is protected by this flag.
    // It is not used to synchronize access to any other state (m_result), so relaxed atomics are fine.
    void cancel() { m_canceled.store(true, AK::MemoryOrder::memory_order_relaxed); }
    // If your action is long-running, you should periodically check the cancel state and possibly return early.
    bool is_canceled() const { return m_canceled.load(AK::MemoryOrder::memory_order_relaxed); }

private:
    BackgroundAction(ESCAPING Function<ErrorOr<Result>(BackgroundAction&)> action, ESCAPING Function<ErrorOr<void>(Result)> on_complete, ESCAPING Optional<Function<void(Error)>> on_error = {})
        : m_action(move(action))
        , m_on_complete(move(on_complete))
    {
        auto promise = Promise::construct();

        if (m_on_complete) {
            auto self = NonnullRefPtr(*this);
            promise->on_resolution = [](NonnullRefPtr<Core::EventReceiver>& object) -> ErrorOr<void> {
                auto self = static_ptr_cast<BackgroundAction<Result>>(object);
                VERIFY(self->m_result.has_value());
                if (auto maybe_error = self->m_on_complete(self->m_result.release_value()); maybe_error.is_error()) {
                    // If on_complete returns an error, we pass it along to your on_error handler.
                    if (self->m_on_error)
                        self->m_on_error(maybe_error.release_error());
                }
                return {};
            };
            promise->on_rejection = [self](Error& error) {
                if (error.is_errno() && error.code() == ECANCELED)
                    self->m_canceled.store(true, AK::MemoryOrder::memory_order_relaxed);
            };
            Core::EventLoop::current().add_job(promise);
        }

        if (on_error.has_value())
            m_on_error = on_error.release_value();

        enqueue_work([self = NonnullRefPtr(*this), promise = move(promise), origin_event_loop = Core::EventLoop::current_weak()]() mutable {
            auto* self_ptr = self.ptr();
            auto post_to_origin = [&](StringView message_type, Function<void()> callback) {
                if (auto origin = origin_event_loop->take()) {
                    origin->deferred_invoke(move(callback));
                } else {
                    dbgln("BackgroundAction {:p}: dropped {} (origin loop gone)", self_ptr, message_type);
                }
            };

            auto result = self->m_action(*self);
            auto const has_job = static_cast<bool>(self->m_on_complete);
            auto const canceled = self->m_canceled.load(AK::MemoryOrder::memory_order_relaxed);

            if (canceled) {
                if (has_job) {
                    post_to_origin("promise rejection"sv, [promise = move(promise)]() mutable {
                        promise->reject(Error::from_errno(ECANCELED));
                    });
                }
                return;
            }
            if (!result.is_error()) {
                self->m_result = result.release_value();
                if (has_job) {
                    post_to_origin("on_complete"sv, [self = move(self), promise = move(promise)]() mutable {
                        // Our promise's resolution function will never error.
                        (void)promise->resolve(*self);
                    });
                }
                return;
            }
            auto error = result.release_error();
            if (has_job) {
                post_to_origin("promise rejection"sv, [promise = move(promise), error = Error::copy(error)]() mutable {
                    promise->reject(Error::copy(error));
                });
            }
            if (self->m_on_error) {
                post_to_origin("on_error"sv, [self = move(self), error = Error::copy(error)]() mutable {
                    self->m_on_error(Error::copy(error));
                });
            }
        });
    }

    Function<ErrorOr<Result>(BackgroundAction&)> m_action;
    Function<ErrorOr<void>(Result)> m_on_complete;
    Function<void(Error)> m_on_error = [](Error error) {
        dbgln("Error occurred while running a BackgroundAction: {}", error);
    };
    Optional<Result> m_result;
    Atomic<bool> m_canceled { false };
};

void quit_background_thread();

}
