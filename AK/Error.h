/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <errno.h>

namespace AK {

class [[nodiscard]] Error {
public:
    enum class Kind : u8 {
        Errno,
        Syscall,
        Windows,
        StringLiteral,
    };

    static Error from_errno(int code)
    {
        return Error(code, Kind::Errno);
    }

    static Error from_syscall(StringView syscall_name, int code)
    {
        return Error(syscall_name, code);
    }

    // Prefer `from_string_literal` when directly typing out an error message:
    //
    //     return Error::from_string_literal("Class: Some failure");
    //
    // If you need to return a static string based on a dynamic condition (like picking an error from an array), then
    // prefer `from_string_view` instead.
    template<size_t N>
    ALWAYS_INLINE static Error from_string_literal(char const (&string_literal)[N])
    {
        return from_string_view(StringView { string_literal, N - 1 });
    }

    static Error from_string_view(StringView string_literal)
    {
        return Error(string_literal);
    }

    template<OneOf<ByteString, String, FlyString> T>
    static Error from_string_view(T)
    {
        // `Error::from_string_view(ByteString::formatted(...))` is a somewhat common mistake, which leads to a UAF
        // situation. If your string outlives this error and _isn't_ a temporary being passed to this function,
        // explicitly call .view() on it to resolve to the StringView overload.
        static_assert(DependentFalse<T>, "Error::from_string_view(String) is almost always a use-after-free");
        VERIFY_NOT_REACHED();
    }

#ifdef AK_OS_WINDOWS
    static Error from_windows_error(u32 windows_error);
    static Error from_windows_error();
#endif

    static Error copy(Error const& error)
    {
        return Error(error);
    }

    ALWAYS_INLINE Error(Error&&) = default;
    ALWAYS_INLINE Error& operator=(Error&&) = default;

    bool operator==(Error const& other) const
    {
        return m_code == other.m_code && m_string_literal == other.m_string_literal && m_kind == other.m_kind;
    }

    bool is_errno() const { return m_kind == Kind::Errno || m_kind == Kind::Syscall; }
    bool is_windows_error() const { return m_kind == Kind::Windows; }

    StringView string_literal() const { return m_string_literal; }
    int code() const { return m_code; }
    Kind kind() const { return m_kind; }

private:
    Error(int code, Kind kind)
        : m_code(code)
        , m_kind(kind)
    {
        VERIFY(code != 0);
    }

    Error(StringView string_literal)
        : m_string_literal(string_literal)
        , m_kind(Kind::StringLiteral)
    {
    }

    Error(StringView syscall_name, int code)
        : m_string_literal(syscall_name)
        , m_code(code)
        , m_kind(Kind::Syscall)
    {
    }

    Error(Error const&) = default;
    Error& operator=(Error const&) = default;

    StringView m_string_literal;
    int m_code { 0 };
    Kind m_kind {};
};

template<typename T, typename E>
class [[nodiscard]] ErrorOr {
    template<typename U, typename F>
    friend class ErrorOr;

public:
    using ResultType = T;
    using ErrorType = E;

    ErrorOr()
    requires(IsSame<T, Empty>)
        : m_value_or_error(Empty {})
    {
    }

    ALWAYS_INLINE ErrorOr(ErrorOr&&) = default;
    ALWAYS_INLINE ErrorOr& operator=(ErrorOr&&) = default;

    ErrorOr(ErrorOr const&) = delete;
    ErrorOr& operator=(ErrorOr const&) = delete;

    template<typename U>
    ALWAYS_INLINE ErrorOr(ErrorOr<U, ErrorType>&& value)
    requires(IsConvertible<U, T>)
        : m_value_or_error(value.m_value_or_error.visit([](U& v) { return Variant<T, ErrorType>(move(v)); }, [](ErrorType& error) { return Variant<T, ErrorType>(move(error)); }))
    {
    }

    template<typename U>
    ALWAYS_INLINE ErrorOr(U&& value)
    requires(
        requires { T(declval<U>()); } || requires { ErrorType(declval<RemoveCVReference<U>>()); })
        : m_value_or_error(forward<U>(value))
    {
    }

    T& value() { return m_value_or_error.template get<T>(); }
    T const& value() const { return m_value_or_error.template get<T>(); }
    [[nodiscard]] ALWAYS_INLINE T const& value_or(T const& fallback) const
    {
        if (is_error())
            return fallback;
        return m_value_or_error.template get<T>();
    }

    ErrorType& error() { return m_value_or_error.template get<ErrorType>(); }
    ErrorType const& error() const { return m_value_or_error.template get<ErrorType>(); }

    bool is_error() const { return m_value_or_error.template has<ErrorType>(); }

    T release_value() { return move(value()); }
    ErrorType release_error() { return move(error()); }

    T release_value_but_fixme_should_propagate_errors()
    {
        VERIFY(!is_error());
        return release_value();
    }

private:
    Variant<T, ErrorType> m_value_or_error;
};

template<typename ErrorType>
class [[nodiscard]] ErrorOr<void, ErrorType> : public ErrorOr<Empty, ErrorType> {
public:
    using ResultType = void;
    using ErrorOr<Empty, ErrorType>::ErrorOr;
};

}

#if USING_AK_GLOBALLY
using AK::Error;
using AK::ErrorOr;
#endif
