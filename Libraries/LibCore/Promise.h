/*
 * Copyright (c) 2021, Kyle Pereira <hey@xylepereira.me>
 * Copyright (c) 2022, kleines Filmröllchen <filmroellchen@serenityos.org>
 * Copyright (c) 2021-2023, Ali Mohammad Pur <mpfard@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventReceiver.h>

namespace Core {

template<typename Result, typename TError>
class Promise : public EventReceiver {
    C_OBJECT(Promise);

public:
    using ErrorType = TError;

    Function<ErrorOr<void>(Result&)> on_resolution;
    Function<void(ErrorType&)> on_rejection;

    static NonnullRefPtr<Promise> after(Vector<NonnullRefPtr<Promise>>&& promises)
    {
        auto promise = Promise::construct();
        struct Resolved : RefCounted<Resolved> {
            explicit Resolved(size_t n)
                : needed(n)
            {
            }

            size_t count { 0 };
            size_t needed { 0 };
        };

        auto resolved = make_ref_counted<Resolved>(promises.size());
        auto weak_promise = promise->template make_weak_ptr<Promise>();
        for (auto p : promises) {
            p->when_resolved([weak_promise, resolved](Result&) -> ErrorOr<void> {
                if (!weak_promise || weak_promise->is_rejected())
                    return {};

                if (++resolved->count == resolved->needed)
                    weak_promise->resolve({});
                return {};
            });

            p->when_rejected([weak_promise, resolved](ErrorType& error) {
                ++resolved->count;
                if (weak_promise)
                    weak_promise->reject(move(error));
            });

            promise->add_child(*p);
        }

        return promise;
    }

    void resolve(Result&& result)
    {
        m_result_or_rejection = move(result);

        if (on_resolution) {
            auto handler_result = on_resolution(m_result_or_rejection->value());
            possibly_handle_rejection(handler_result);
        }
    }

    void reject(ErrorType&& error)
    {
        m_result_or_rejection = move(error);
        possibly_handle_rejection(*m_result_or_rejection);
    }

    bool is_rejected()
    {
        return m_result_or_rejection.has_value() && m_result_or_rejection->is_error();
    }

    bool is_resolved() const
    {
        return m_result_or_rejection.has_value() && !m_result_or_rejection->is_error();
    }

    ErrorOr<Result, ErrorType> await()
    {
        while (!m_result_or_rejection.has_value())
            Core::EventLoop::current().pump();

        return m_result_or_rejection.release_value();
    }

    // Converts a Promise<A> to a Promise<B> using a function func: A -> B
    template<typename T>
    NonnullRefPtr<Promise<T>> map(Function<T(Result&)> func)
    {
        NonnullRefPtr<Promise<T>> new_promise = Promise<T>::construct();

        if (is_resolved())
            new_promise->resolve(func(m_result_or_rejection->value()));
        if (is_rejected())
            new_promise->reject(m_result_or_rejection->release_error());

        on_resolution = [new_promise, func = move(func)](Result& result) -> ErrorOr<void> {
            new_promise->resolve(func(result));
            return {};
        };
        on_rejection = [new_promise](ErrorType& error) {
            new_promise->reject(move(error));
        };
        return new_promise;
    }

    template<CallableAs<void, Result&> F>
    Promise& when_resolved(F handler)
    {
        return when_resolved([handler = move(handler)](Result& result) mutable -> ErrorOr<void> {
            handler(result);
            return {};
        });
    }

    template<CallableAs<ErrorOr<void>, Result&> F>
    Promise& when_resolved(F handler)
    {
        on_resolution = move(handler);
        if (is_resolved()) {
            auto handler_result = on_resolution(m_result_or_rejection->value());
            possibly_handle_rejection(handler_result);
        }

        return *this;
    }

    template<CallableAs<void, ErrorType&> F>
    Promise& when_rejected(F handler)
    {
        on_rejection = move(handler);
        if (is_rejected())
            on_rejection(m_result_or_rejection->error());

        return *this;
    }

private:
    template<typename T>
    void possibly_handle_rejection(ErrorOr<T>& result)
    {
        if (result.is_error() && on_rejection)
            on_rejection(result.error());
    }

    Promise() = default;
    Promise(EventReceiver* parent)
        : EventReceiver(parent)
    {
    }

    Optional<ErrorOr<Result, ErrorType>> m_result_or_rejection;
};

}
