/*
 * Copyright (c) 2021-2023, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibJS/Runtime/VM.h>
#include <LibWeb/WebIDL/ExceptionOr.h>

namespace Web::Bindings {

template<typename>
constexpr bool IsExceptionOr = false;

template<typename T>
constexpr bool IsExceptionOr<WebIDL::ExceptionOr<T>> = true;

template<typename>
constexpr bool IsThrowCompletionOr = false;

template<typename T>
constexpr bool IsThrowCompletionOr<JS::ThrowCompletionOr<T>> = true;

namespace Detail {

template<typename T>
struct ExtractExceptionOrValueType {
    using Type = T;
};

template<typename T>
struct ExtractExceptionOrValueType<WebIDL::ExceptionOr<T>> {
    using Type = T;
};

template<typename T>
struct ExtractExceptionOrValueType<JS::ThrowCompletionOr<T>> {
    using Type = T;
};

template<>
struct ExtractExceptionOrValueType<void> {
    using Type = JS::Value;
};

template<>
struct ExtractExceptionOrValueType<WebIDL::ExceptionOr<Empty>> {
    using Type = JS::Value;
};

template<>
struct ExtractExceptionOrValueType<WebIDL::ExceptionOr<void>> {
    using Type = JS::Value;
};

}

ALWAYS_INLINE JS::Completion dom_exception_to_throw_completion(JS::VM& vm, auto&& exception)
{
    return exception.visit(
        [&](WebIDL::SimpleException const& exception) {
            auto message = exception.message.visit([](auto const& s) -> StringView { return s; });
            switch (exception.type) {
#define E(x)                             \
    case WebIDL::SimpleExceptionType::x: \
        return vm.template throw_completion<JS::x>(message);

                ENUMERATE_SIMPLE_WEBIDL_EXCEPTION_TYPES(E)

#undef E
            default:
                VERIFY_NOT_REACHED();
            }
        },
        [&](JS::NonnullGCPtr<WebIDL::DOMException> const& exception) {
            return throw_completion(exception);
        },
        [&](JS::Completion const& completion) {
            return completion;
        });
}

template<typename T>
using ExtractExceptionOrValueType = typename Detail::ExtractExceptionOrValueType<T>::Type;

// Return type depends on the return type of 'fn' (when invoked with no args):
// void or ExceptionOr<void>: JS::ThrowCompletionOr<JS::Value>, always returns JS::js_undefined()
// ExceptionOr<T>: JS::ThrowCompletionOr<T>
// T: JS::ThrowCompletionOr<T>
template<typename F, typename T = decltype(declval<F>()()), typename Ret = Conditional<!IsExceptionOr<T> && !IsVoid<T> && !IsThrowCompletionOr<T>, T, ExtractExceptionOrValueType<T>>>
JS::ThrowCompletionOr<Ret> throw_dom_exception_if_needed(JS::VM& vm, F&& fn)
{
    if constexpr (IsExceptionOr<T>) {
        auto&& result = fn();

        if (result.is_exception())
            return dom_exception_to_throw_completion(vm, result.exception());

        if constexpr (requires(T v) { v.value(); })
            return result.value();
        else
            return JS::js_undefined();
    } else if constexpr (IsVoid<T>) {
        fn();
        return JS::js_undefined();
    } else {
        return fn();
    }
}

}
