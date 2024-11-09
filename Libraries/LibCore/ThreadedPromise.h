/*
 * Copyright (c) 2021, Kyle Pereira <hey@xylepereira.me>
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2021-2023, Ali Mohammad Pur <mpfard@serenityos.org>
 * Copyright (c) 2023, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/AtomicRefCounted.h>
#include <AK/Concepts.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>
#include <LibThreading/Mutex.h>

namespace Core {

template<typename TResult, typename TError>
class ThreadedPromise
    : public AtomicRefCounted<ThreadedPromise<TResult, TError>> {
public:
    static NonnullRefPtr<ThreadedPromise<TResult, TError>> create()
    {
        return adopt_ref(*new ThreadedPromise<TResult, TError>());
    }

    using ResultType = Conditional<IsSame<TResult, void>, Empty, TResult>;
    using ErrorType = TError;

    void resolve(ResultType&& result)
    {
        when_error_handler_is_ready([self = NonnullRefPtr(*this), result = move(result)]() mutable {
            if (self->m_resolution_handler) {
                auto handler_result = self->m_resolution_handler(forward<ResultType>(result));
                if (handler_result.is_error())
                    self->m_rejection_handler(handler_result.release_error());
                self->m_has_completed = true;
            }
        });
    }
    void resolve()
    requires IsSame<ResultType, Empty>
    {
        resolve(Empty());
    }

    void reject(ErrorType&& error)
    {
        when_error_handler_is_ready([this, error = move(error)]() mutable {
            m_rejection_handler(forward<ErrorType>(error));
            m_has_completed = true;
        });
    }
    void reject(ErrorType const& error)
    requires IsTriviallyCopyable<ErrorType>
    {
        reject(ErrorType(error));
    }

    bool has_completed()
    {
        Threading::MutexLocker locker { m_mutex };
        return m_has_completed;
    }

    void await()
    {
        while (!has_completed())
            Core::EventLoop::current().pump(EventLoop::WaitMode::PollForEvents);
    }

    // Set the callback to be called when the promise is resolved. A rejection callback
    // must also be provided before any callback will be called.
    template<CallableAs<ErrorOr<void>, ResultType&&> ResolvedHandler>
    ThreadedPromise& when_resolved(ResolvedHandler handler)
    {
        Threading::MutexLocker locker { m_mutex };
        VERIFY(!m_resolution_handler);
        m_resolution_handler = move(handler);
        return *this;
    }

    template<CallableAs<void, ResultType&&> ResolvedHandler>
    ThreadedPromise& when_resolved(ResolvedHandler handler)
    {
        return when_resolved([handler = move(handler)](ResultType&& result) -> ErrorOr<void> {
            handler(forward<ResultType>(result));
            return {};
        });
    }

    template<CallableAs<ErrorOr<void>> ResolvedHandler>
    ThreadedPromise& when_resolved(ResolvedHandler handler)
    {
        return when_resolved([handler = move(handler)](ResultType&&) -> ErrorOr<void> {
            return handler();
        });
    }

    template<CallableAs<void> ResolvedHandler>
    ThreadedPromise& when_resolved(ResolvedHandler handler)
    {
        return when_resolved([handler = move(handler)](ResultType&&) -> ErrorOr<void> {
            handler();
            return {};
        });
    }

    // Set the callback to be called when the promise is rejected. Setting this callback
    // will cause the promise fulfillment to be ready to be handled.
    template<CallableAs<void, ErrorType&&> RejectedHandler>
    ThreadedPromise& when_rejected(RejectedHandler when_rejected = [](ErrorType&) {})
    {
        Threading::MutexLocker locker { m_mutex };
        VERIFY(!m_rejection_handler);
        m_rejection_handler = move(when_rejected);
        return *this;
    }

    template<typename T, CallableAs<NonnullRefPtr<ThreadedPromise<T, ErrorType>>, ResultType&&> ChainedResolution>
    NonnullRefPtr<ThreadedPromise<T, ErrorType>> chain_promise(ChainedResolution chained_resolution)
    {
        auto new_promise = ThreadedPromise<T, ErrorType>::create();
        when_resolved([=, chained_resolution = move(chained_resolution)](ResultType&& result) mutable -> ErrorOr<void> {
            chained_resolution(forward<ResultType>(result))
                ->when_resolved([=](auto&& new_result) { new_promise->resolve(move(new_result)); })
                .when_rejected([=](ErrorType&& error) { new_promise->reject(move(error)); });
            return {};
        });
        when_rejected([=](ErrorType&& error) { new_promise->reject(move(error)); });
        return new_promise;
    }

    template<typename T, CallableAs<ErrorOr<T, ErrorType>, ResultType&&> MappingFunction>
    NonnullRefPtr<ThreadedPromise<T, ErrorType>> map(MappingFunction mapping_function)
    {
        auto new_promise = ThreadedPromise<T, ErrorType>::create();
        when_resolved([=, mapping_function = move(mapping_function)](ResultType&& result) -> ErrorOr<void> {
            new_promise->resolve(TRY(mapping_function(forward<ResultType>(result))));
            return {};
        });
        when_rejected([=](ErrorType&& error) { new_promise->reject(move(error)); });
        return new_promise;
    }

private:
    template<typename F>
    static void deferred_handler_check(NonnullRefPtr<ThreadedPromise> self, F&& function)
    {
        Threading::MutexLocker locker { self->m_mutex };
        if (self->m_rejection_handler) {
            function();
            return;
        }
        EventLoop::current().deferred_invoke([self, function = forward<F>(function)]() mutable {
            deferred_handler_check(self, move(function));
        });
    }

    template<typename F>
    void when_error_handler_is_ready(F function)
    {
        if (EventLoop::is_running()) {
            deferred_handler_check(NonnullRefPtr(*this), move(function));
        } else {
            // NOTE: Handlers should always be set almost immediately, so we can expect this
            //       to spin extremely briefly. Therefore, sleeping the thread should not be
            //       necessary.
            while (true) {
                Threading::MutexLocker locker { m_mutex };
                if (m_rejection_handler)
                    break;
            }
            VERIFY(m_rejection_handler);
            function();
        }
    }

    ThreadedPromise() = default;
    ThreadedPromise(EventReceiver* parent)
        : EventReceiver(parent)
    {
    }

    Function<ErrorOr<void>(ResultType&&)> m_resolution_handler;
    Function<void(ErrorType&&)> m_rejection_handler;
    Threading::Mutex m_mutex;
    bool m_has_completed;
};

}
