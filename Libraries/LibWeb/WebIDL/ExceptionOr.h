/*
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/RefPtr.h>
#include <LibJS/Runtime/Completion.h>
#include <LibWeb/WebIDL/DOMException.h>

namespace Web::WebIDL {

#define ENUMERATE_SIMPLE_WEBIDL_EXCEPTION_TYPES(E) \
    E(EvalError)                                   \
    E(RangeError)                                  \
    E(ReferenceError)                              \
    E(TypeError)                                   \
    E(URIError)

#define E(x) x,
enum class SimpleExceptionType {
    ENUMERATE_SIMPLE_WEBIDL_EXCEPTION_TYPES(E)
};
#undef E

struct SimpleException {
    SimpleExceptionType type;
    Variant<String, StringView> message;
};

using Exception = Variant<SimpleException, JS::NonnullGCPtr<DOMException>, JS::Completion>;

template<typename ValueType>
class [[nodiscard]] ExceptionOr {
public:
    ExceptionOr()
    requires(IsSame<ValueType, Empty>)
        : m_result_or_exception(Empty {})
    {
    }

    ExceptionOr(ValueType const& result)
        : m_result_or_exception(result)
    {
    }

    ExceptionOr(ValueType&& result)
        : m_result_or_exception(move(result))
    {
    }

    // Allows implicit construction of ExceptionOr<T> from a type U if T(U) is a supported constructor.
    // Most commonly: Value from Object* or similar, so we can omit the curly braces from "return { TRY(...) };".
    // Disabled for POD types to avoid weird conversion shenanigans.
    template<typename WrappedValueType>
    ExceptionOr(WrappedValueType result)
    requires(!IsPOD<ValueType>)
        : m_result_or_exception(ValueType { move(result) })
    {
    }

    ExceptionOr(JS::NonnullGCPtr<DOMException> exception)
        : m_result_or_exception(exception)
    {
    }

    ExceptionOr(SimpleException exception)
        : m_result_or_exception(move(exception))
    {
    }

    ExceptionOr(JS::Completion exception)
        : m_result_or_exception(move(exception))
    {
        auto const& completion = m_result_or_exception.template get<JS::Completion>();
        VERIFY(completion.is_error());
    }

    ExceptionOr(Exception exception)
        : m_result_or_exception(move(exception))
    {
        if (auto* completion = m_result_or_exception.template get_pointer<JS::Completion>())
            VERIFY(completion->is_error());
    }

    ExceptionOr(ExceptionOr&& other) = default;
    ExceptionOr(ExceptionOr const& other) = default;
    ~ExceptionOr() = default;

    ValueType& value()
    requires(!IsSame<ValueType, Empty>)
    {
        return m_result_or_exception.template get<ValueType>();
    }

    ValueType release_value()
    {
        return move(m_result_or_exception.template get<ValueType>());
    }

    Exception exception() const
    {
        return m_result_or_exception.template downcast<SimpleException, JS::NonnullGCPtr<DOMException>, JS::Completion>();
    }

    bool is_exception() const
    {
        return !m_result_or_exception.template has<ValueType>();
    }

    ValueType release_value_but_fixme_should_propagate_errors()
    {
        VERIFY(!is_error());
        return release_value();
    }

    // These are for compatibility with the TRY() macro in AK.
    [[nodiscard]] bool is_error() const { return is_exception(); }
    Exception release_error() { return exception(); }

private:
    // https://webidl.spec.whatwg.org/#idl-exceptions
    Variant<ValueType, SimpleException, JS::NonnullGCPtr<DOMException>, JS::Completion> m_result_or_exception;
};

template<>
class [[nodiscard]] ExceptionOr<void> : public ExceptionOr<Empty> {
public:
    using ExceptionOr<Empty>::ExceptionOr;
};

}
