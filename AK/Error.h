/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/CheckedFormatString.h>
#include <AK/StringView.h>
#include <AK/Variant.h>
#include <errno.h>

namespace AK {
namespace Detail {
struct ExplicitStaticString {
    explicit constexpr ExplicitStaticString(StringView str)
        : string(str)
    {
    }
    StringView string;
};

template<typename T>
constexpr bool CanBePlacedInErrorFormattedString = IsOneOf<T, bool, u8, u16, u32, u64, i8, i16, i32, i64, ExplicitStaticString>;

template<typename... Ts>
consteval size_t error_formatted_string_buffer_encoded_size()
{
    return ((sizeof(Ts) + 1) + ...);
}
}

// Error Format Buffer :
//   Type ::= u8 { Nothing{s=0}, U<n>{s=n/8}, I<n>{s=n/8}, Bool{s=1}, ExplicitStaticString{s=sizeof(StringView)} } where n \in {8,16,32,64}
//   FormatBufferEntry ::= Type [ u8 * Type.s ]
//   FormatBuffer ::= FormatBufferEntry* Type::Nothing

enum class ErrorFormattedStringType : u8 {
    Nothing = 0,
    U8 = 1,
    U16 = 2,
    U32 = 3,
    U64 = 4,
    I8 = 5,
    I16 = 6,
    I32 = 7,
    I64 = 8,
    Bool = 9,
    ExplicitStaticString = 10,
};

template<typename FormatBuffer, typename... Ts>
constexpr bool FitsInErrorFormattedStringBuffer = requires {
    requires(Detail::CanBePlacedInErrorFormattedString<Ts> && ...);
    requires Detail::error_formatted_string_buffer_encoded_size<Ts...>() <= sizeof(FormatBuffer) - sizeof(ErrorFormattedStringType);
};

class [[nodiscard]] Error {
    struct Syscall {
        StringView name;
        int code;

        bool operator==(Syscall const&) const = default;
    };

    struct ErrnoCode {
        int code;
        bool operator==(ErrnoCode const&) const = default;
    };

    struct WindowsError {
        u32 code;
        bool operator==(WindowsError const&) const = default;
    };

    struct FormattedString {
        using Type = ErrorFormattedStringType;
        bool operator==(FormattedString const&) const = default;

        StringView format_string;
        Array<u8, 64 - sizeof(StringView)> buffer;
    };
    static_assert(sizeof(FormattedString) == 64, "Broken alignment on Error::FormattedString");

public:
    enum class Kind : u8 {
        Errno,
        Syscall,
        Windows,
        StringLiteral,
        FormattedString,
    };

    static Error from_errno(int code)
    {
        return Error(code);
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

    template<typename... Ts>
    requires(FitsInErrorFormattedStringBuffer<decltype(FormattedString::buffer), Ts...>)
    static Error formatted_error(CheckedFormatString<Ts...> format_string, Ts... args)
    {
        return Error(format_string, args...);
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
        return m_data.visit([&]<typename T>(T const& self) {
            if (auto const* p = other.m_data.get_pointer<T>())
                return self == *p;
            return false;
        });
    }

    bool is_errno() const { return m_data.has<ErrnoCode>() || m_data.has<Syscall>(); }
    bool is_windows_error() const { return m_data.has<WindowsError>(); }
    bool is_formatted_string() const { return m_data.has<FormattedString>(); }

    StringView string_literal() const
    {
        return m_data.visit(
            [](StringView const& str) { return str; },
            [](Syscall const& syscall) {
                return syscall.name;
            },
            [](auto const&) -> StringView {
                return {};
            });
    }
    int code() const {
        return m_data.visit(
            [](OneOf<ErrnoCode, Syscall> auto const& value) { return value.code; },
            [](auto const&) -> int { return 0; }
        );
    }
    Kind kind() const
    {
        return m_data.visit(
            [](ErrnoCode const&) { return Kind::Errno; },
            [](Syscall const&) { return Kind::Syscall; },
            [](WindowsError const&) { return Kind::Windows; },
            [](FormattedString const&) { return Kind::FormattedString; },
            [](StringView const&) { return Kind::StringLiteral; });
    }

    template<typename R>
    R format_impl() const;

private:
    Error(int code)
        : m_data(ErrnoCode { code })
    {
        VERIFY(code != 0);
    }

    Error(StringView string_literal)
        : m_data(string_literal)
    {
    }

    Error(StringView syscall_name, int code)
        : m_data(Syscall { syscall_name, code })
    {
    }

    template<typename... Ts>
    Error(CheckedFormatString<Ts...> format_string, Ts... args)
        : m_data(FormattedString(format_string.view(), pack(args...)))
    {
    }

    template<typename... Ts>
    static decltype(auto) pack(Ts... values)
    {
        decltype(FormattedString::buffer) buffer {};
        size_t offset = 0;
        ([&]<typename T>(T const& value) {
            auto size = sizeof(T);
            if (offset + size + 2 >= buffer.size())
                VERIFY_NOT_REACHED(); // Should be caught by FitsInErrorFormattedStringBuffer
            
            ErrorFormattedStringType tag;
            if constexpr (IsSame<T, u8>) {
                tag = ErrorFormattedStringType::U8;
            } else if constexpr (IsSame<T, u16>) {
                tag = ErrorFormattedStringType::U16;
            } else if constexpr (IsSame<T, u32>) {
                tag = ErrorFormattedStringType::U32;
            } else if constexpr (IsSame<T, u64>) {
                tag = ErrorFormattedStringType::U64;
            } else if constexpr (IsSame<T, i8>) {
                tag = ErrorFormattedStringType::I8;
            } else if constexpr (IsSame<T, i16>) {
                tag = ErrorFormattedStringType::I16;
            } else if constexpr (IsSame<T, i32>) {
                tag = ErrorFormattedStringType::I32;
            } else if constexpr (IsSame<T, i64>) {
                tag = ErrorFormattedStringType::I64;
            } else if constexpr (IsSame<T, bool>) {
                tag = ErrorFormattedStringType::Bool;
            } else if constexpr (IsSame<T, Detail::ExplicitStaticString>) {
                tag = ErrorFormattedStringType::ExplicitStaticString;
            } else {
                static_assert(DependentFalse<T>, "Error::formatted() can only be passed ExplicitStaticString, or integral types");
            }

            buffer[offset] = to_underlying(tag);
            memcpy(&buffer[offset + 1], reinterpret_cast<u8 const*>(&value), size);
            offset += size + 1;
        }(values), ...);

        buffer[offset] = to_underlying(ErrorFormattedStringType::Nothing);
        return buffer;
    }

    Error(Error const&) = default;
    Error& operator=(Error const&) = default;

    Variant<ErrnoCode, Syscall, WindowsError, FormattedString, StringView> m_data;
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
