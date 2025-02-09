/*
 * Copyright (c) 2019-2020, Sergey Bugaev <bugaevc@serenityos.org>
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2022-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Queue.h>
#include <LibCore/Event.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>
#include <LibCore/Promise.h>
#include <LibThreading/Thread.h>

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

    void cancel() { m_canceled = true; }
    // If your action is long-running, you should periodically check the cancel state and possibly return early.
    bool is_canceled() const { return m_canceled; }

private:
    BackgroundAction(ESCAPING Function<ErrorOr<Result>(BackgroundAction&)> action, ESCAPING Function<ErrorOr<void>(Result)> on_complete, ESCAPING Optional<Function<void(Error)>> on_error = {})
        : m_action(move(action))
        , m_on_complete(move(on_complete))
    {
        auto promise = Promise::construct();

        if (m_on_complete) {
            promise->on_resolution = [](NonnullRefPtr<Core::EventReceiver>& object) -> ErrorOr<void> {
                auto self = static_ptr_cast<BackgroundAction<Result>>(object);
                VERIFY(self->m_result.has_value());
                if (auto maybe_error = self->m_on_complete(self->m_result.value()); maybe_error.is_error())
                    self->m_on_error(maybe_error.release_error());

                return {};
            };
            Core::EventLoop::current().add_job(promise);
        }

        if (on_error.has_value())
            m_on_error = on_error.release_value();

        enqueue_work([self = NonnullRefPtr(*this), promise = move(promise), origin_event_loop = &Core::EventLoop::current()]() mutable {
            auto result = self->m_action(*self);

            // The event loop cancels the promise when it exits.
            self->m_canceled |= promise->is_rejected();

            // All of our work was successful and we weren't cancelled; resolve the event loop's promise.
            if (!self->m_canceled && !result.is_error()) {
                self->m_result = result.release_value();

                // If there is no completion callback, we don't rely on the user keeping around the event loop.
                if (self->m_on_complete) {
                    origin_event_loop->deferred_invoke([self, promise = move(promise)] {
                        // Our promise's resolution function will never error.
                        (void)promise->resolve(*self);
                    });
                    origin_event_loop->wake();
                }
            } else {
                // We were either unsuccessful or cancelled (in which case there is no error).
                auto error = Error::from_errno(ECANCELED);
                if (result.is_error())
                    error = result.release_error();

                promise->reject(Error::from_errno(ECANCELED));

                if (!self->m_canceled && self->m_on_error) {
                    origin_event_loop->deferred_invoke([self, error = move(error)]() mutable {
                        self->m_on_error(move(error));
                    });
                    origin_event_loop->wake();
                } else if (self->m_on_error) {
                    self->m_on_error(move(error));
                }
            }
        });
    }

    Function<ErrorOr<Result>(BackgroundAction&)> m_action;
    Function<ErrorOr<void>(Result)> m_on_complete;
    Function<void(Error)> m_on_error = [](Error error) {
        dbgln("Error occurred while running a BackgroundAction: {}", error);
    };
    Optional<Result> m_result;
    bool m_canceled { false };
};

void quit_background_thread();

}
