/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/CheckedFormatString.h>

#include <AK/AllOf.h>
#include <AK/AnyOf.h>
#include <AK/Error.h>
#include <AK/Forward.h>
#include <AK/Optional.h>
#include <AK/StringView.h>
#include <stdio.h>
#include <string.h>

namespace AK {

class TypeErasedFormatParams;
class FormatParser;
class FormatBuilder;

template<typename T, typename = void>
struct Formatter {
    using __no_formatter_defined = void;
};

enum AllowDebugOnlyFormatters {
    No,
    Yes
};

template<typename T, typename = void>
inline constexpr bool HasFormatter = true;

template<typename T>
inline constexpr bool HasFormatter<T, typename Formatter<T>::__no_formatter_defined> = false;

template<typename Formatter>
inline constexpr bool is_debug_only_formatter()
{
    constexpr bool has_is_debug_only = requires(Formatter const& formatter) { formatter.is_debug_only(); };
    if constexpr (has_is_debug_only)
        return Formatter::is_debug_only();
    return false;
}

template<typename T>
concept Formattable = HasFormatter<T>;

constexpr size_t max_format_arguments = 256;

template<typename T>
ErrorOr<void> __format_value(TypeErasedFormatParams& params, FormatBuilder& builder, FormatParser& parser, void const* value)
{
    Formatter<T> formatter;

    formatter.parse(params, parser);
    return formatter.format(builder, *static_cast<T const*>(value));
}

struct TypeErasedParameter {
    enum class Type {
        UnsignedInteger,
        SignedInteger,
        Boolean,
        Character,
        Float,
        Double,
        StringView,
        CString,
        CustomType
    };

    struct CustomType {
        void const* value;
        ErrorOr<void> (*formatter)(TypeErasedFormatParams&, FormatBuilder&, FormatParser&, void const* value);
    };

    template<typename T>
    static bool const IsChar = IsOneOf<T, char, wchar_t, char8_t, char16_t, char32_t>;

    template<Unsigned U>
    explicit constexpr TypeErasedParameter(U const& value)
    requires(!IsChar<U> && sizeof(U) <= sizeof(u64))
        : value { .as_unsigned = value }
        , type { Type::UnsignedInteger }
    {
    }

    template<Signed I>
    explicit constexpr TypeErasedParameter(I const& value)
    requires(!IsChar<I> && sizeof(I) <= sizeof(i64))
        : value { .as_signed = value }
        , type { Type::SignedInteger }
    {
    }

    explicit constexpr TypeErasedParameter(bool const& value)
        : value { .as_bool = value }
        , type { Type::Boolean }
    {
    }

    explicit constexpr TypeErasedParameter(char const& value)
        : value { .as_char = value }
        , type { Type::Character }
    {
    }

    explicit constexpr TypeErasedParameter(float const& value)
        : value { .as_float = value }
        , type { Type::Float }
    {
    }

    explicit constexpr TypeErasedParameter(double const& value)
        : value { .as_double = value }
        , type { Type::Double }
    {
    }

    explicit constexpr TypeErasedParameter(StringView const& value)
        : value { .as_string_view = value }
        , type { Type::StringView }
    {
    }

    explicit constexpr TypeErasedParameter(char const* value)
        : value { .as_c_string = value }
        , type { Type::CString }
    {
    }

    template<typename T>
    explicit constexpr TypeErasedParameter(T const& value)
        : value { .as_custom_type = { &value, __format_value<T> } }
        , type { Type::CustomType }
    {
    }

    template<typename Visitor>
    constexpr auto visit(Visitor&& visitor) const
    {
        switch (type) {
        case Type::UnsignedInteger:
            return visitor(value.as_unsigned);
        case Type::SignedInteger:
            return visitor(value.as_signed);
        case Type::Boolean:
            return visitor(value.as_bool);
        case Type::Character:
            return visitor(value.as_char);
        case Type::Float:
            return visitor(value.as_float);
        case Type::Double:
            return visitor(value.as_double);
        case Type::StringView:
            return visitor(value.as_string_view);
        case Type::CString:
            return visitor(value.as_c_string);
        case Type::CustomType:
            return visitor(value.as_custom_type);
        }
        VERIFY_NOT_REACHED();
    }

    constexpr size_t to_size() const
    {
        return visit([]<typename T>(T value) -> size_t {
            if constexpr (IsSame<T, u64>)
                return static_cast<size_t>(value);

            if constexpr (IsSame<T, i64>) {
                VERIFY(value >= 0);
                return static_cast<size_t>(value);
            }

            TODO();
        });
    }

    union {
        u64 as_unsigned;
        i64 as_signed;
        bool as_bool;
        char as_char;
        float as_float;
        double as_double;
        StringView as_string_view;
        char const* as_c_string;
        CustomType as_custom_type;
    } value;
    Type type;
};

class FormatBuilder {
public:
    enum class Align {
        Default,
        Left,
        Center,
        Right,
    };
    enum class SignMode {
        OnlyIfNeeded,
        Always,
        Reserved,
        Default = OnlyIfNeeded,
    };

    enum class RealNumberDisplayMode {
        FixedPoint,
        General,
        Default = General,
    };

    explicit FormatBuilder(StringBuilder& builder)
        : m_builder(builder)
    {
    }

    ErrorOr<void> put_padding(char fill, size_t amount);

    ErrorOr<void> put_literal(StringView value);

    ErrorOr<void> put_string(
        StringView value,
        Align align = Align::Left,
        size_t min_width = 0,
        size_t max_width = NumericLimits<size_t>::max(),
        char fill = ' ');

    ErrorOr<void> put_u64(
        u64 value,
        u8 base = 10,
        bool prefix = false,
        bool upper_case = false,
        bool zero_pad = false,
        bool use_separator = false,
        Align align = Align::Right,
        size_t min_width = 0,
        char fill = ' ',
        SignMode sign_mode = SignMode::OnlyIfNeeded,
        bool is_negative = false);

    ErrorOr<void> put_i64(
        i64 value,
        u8 base = 10,
        bool prefix = false,
        bool upper_case = false,
        bool zero_pad = false,
        bool use_separator = false,
        Align align = Align::Right,
        size_t min_width = 0,
        char fill = ' ',
        SignMode sign_mode = SignMode::OnlyIfNeeded);

    ErrorOr<void> put_fixed_point(
        bool is_negative,
        i64 integer_value,
        u64 fraction_value,
        u64 fraction_one,
        size_t precision,
        u8 base = 10,
        bool upper_case = false,
        bool zero_pad = false,
        bool use_separator = false,
        Align align = Align::Right,
        size_t min_width = 0,
        size_t fraction_max_width = 6,
        char fill = ' ',
        SignMode sign_mode = SignMode::OnlyIfNeeded);

    ErrorOr<void> put_f80(
        long double value,
        u8 base = 10,
        bool upper_case = false,
        bool use_separator = false,
        Align align = Align::Right,
        size_t min_width = 0,
        size_t precision = 6,
        char fill = ' ',
        SignMode sign_mode = SignMode::OnlyIfNeeded,
        RealNumberDisplayMode = RealNumberDisplayMode::Default);

    template<OneOf<f32, f64> T>
    ErrorOr<void> put_f32_or_f64(
        T value,
        u8 base = 10,
        bool upper_case = false,
        bool zero_pad = false,
        bool use_separator = false,
        Align align = Align::Right,
        size_t min_width = 0,
        Optional<size_t> precision = {},
        char fill = ' ',
        SignMode sign_mode = SignMode::OnlyIfNeeded,
        RealNumberDisplayMode = RealNumberDisplayMode::Default);

    ErrorOr<void> put_hexdump(
        ReadonlyBytes,
        size_t width,
        char fill = ' ');

    StringBuilder const& builder() const
    {
        return m_builder;
    }
    StringBuilder& builder() { return m_builder; }

private:
    StringBuilder& m_builder;

    ErrorOr<void> put_f64_with_precision(
        double value,
        u8 base,
        bool upper_case,
        bool zero_pad,
        bool use_separator,
        Align align,
        size_t min_width,
        size_t precision,
        char fill,
        SignMode sign_mode,
        RealNumberDisplayMode);
};

class TypeErasedFormatParams {
public:
    TypeErasedFormatParams(u32 size)
        : m_size(size)
    {
    }

    ReadonlySpan<TypeErasedParameter> parameters() const { return { m_parameters, m_size }; }

    size_t take_next_index() { return m_next_index++; }

private:
    u32 m_size { 0 };
    u32 m_next_index { 0 };
    TypeErasedParameter m_parameters[0];
};

template<AllowDebugOnlyFormatters allow_debug_formatters, typename... Parameters>
class VariadicFormatParams : public TypeErasedFormatParams {
public:
    static_assert(sizeof...(Parameters) <= max_format_arguments);

    explicit VariadicFormatParams(Parameters const&... parameters)
        : TypeErasedFormatParams(sizeof...(Parameters))
        , m_parameter_storage { TypeErasedParameter { parameters }... }
    {
        constexpr bool any_debug_formatters = (is_debug_only_formatter<Formatter<Parameters>>() || ...);
        static_assert(!any_debug_formatters || allow_debug_formatters == AllowDebugOnlyFormatters::Yes,
            "You are attempting to use a debug-only formatter outside of a debug log! Maybe one of your format values is an ErrorOr<T>?");
    }

private:
    TypeErasedParameter m_parameter_storage[sizeof...(Parameters)];
};

// We use the same format for most types for consistency. This is taken directly from
// std::format. One difference is that we are not counting the width or sign towards the
// total width when calculating zero padding for numbers.
// https://en.cppreference.com/w/cpp/utility/format/formatter#Standard_format_specification
struct StandardFormatter {
    enum class Mode {
        Default,
        Binary,
        BinaryUppercase,
        Decimal,
        Octal,
        Hexadecimal,
        HexadecimalUppercase,
        Character,
        String,
        Pointer,
        FixedPoint,
        Hexfloat,
        HexfloatUppercase,
        HexDump,
    };

    FormatBuilder::Align m_align = FormatBuilder::Align::Default;
    FormatBuilder::SignMode m_sign_mode = FormatBuilder::SignMode::OnlyIfNeeded;
    Mode m_mode = Mode::Default;
    bool m_alternative_form = false;
    bool m_use_separator = false;
    char m_fill = ' ';
    bool m_zero_pad = false;
    Optional<size_t> m_width;
    Optional<size_t> m_precision;

    void parse(TypeErasedFormatParams&, FormatParser&);
};

template<Integral T>
struct Formatter<T> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(move(formatter))
    {
    }

    ErrorOr<void> format(FormatBuilder&, T);
};

template<>
struct Formatter<StringView> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(move(formatter))
    {
    }

    ErrorOr<void> format(FormatBuilder&, StringView);
};

template<typename T>
requires(HasFormatter<T>)
struct Formatter<ReadonlySpan<T>> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(move(formatter))
    {
    }

    ErrorOr<void> format(FormatBuilder& builder, ReadonlySpan<T> value)
    {
        if (m_mode == Mode::Pointer) {
            Formatter<FlatPtr> formatter { *this };
            TRY(formatter.format(builder, reinterpret_cast<FlatPtr>(value.data())));
            return {};
        }

        if (m_sign_mode != FormatBuilder::SignMode::Default)
            VERIFY_NOT_REACHED();
        if (m_alternative_form)
            VERIFY_NOT_REACHED();
        if (m_zero_pad)
            VERIFY_NOT_REACHED();
        if (m_mode != Mode::Default)
            VERIFY_NOT_REACHED();
        if (m_width.has_value() && m_precision.has_value())
            VERIFY_NOT_REACHED();

        m_width = m_width.value_or(0);
        m_precision = m_precision.value_or(NumericLimits<size_t>::max());

        Formatter<T> content_fmt;
        TRY(builder.put_literal("[ "sv));
        bool first = true;
        for (auto& content : value) {
            if (!first) {
                TRY(builder.put_literal(", "sv));
                content_fmt = Formatter<T> {};
            }
            first = false;
            TRY(content_fmt.format(builder, content));
        }
        TRY(builder.put_literal(" ]"sv));
        return {};
    }
};

template<typename T>
requires(HasFormatter<T>)
struct Formatter<Span<T>> : Formatter<ReadonlySpan<T>> {
    ErrorOr<void> format(FormatBuilder& builder, Span<T> value)
    {
        return Formatter<ReadonlySpan<T>>::format(builder, value);
    }
};

template<typename T, size_t inline_capacity>
requires(HasFormatter<T>)
struct Formatter<Vector<T, inline_capacity>> : Formatter<ReadonlySpan<T>> {
    ErrorOr<void> format(FormatBuilder& builder, Vector<T, inline_capacity> const& value)
    {
        return Formatter<ReadonlySpan<T>>::format(builder, value.span());
    }
};

template<>
struct Formatter<ReadonlyBytes> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, ReadonlyBytes value)
    {
        if (m_mode == Mode::Pointer) {
            Formatter<FlatPtr> formatter { *this };
            return formatter.format(builder, reinterpret_cast<FlatPtr>(value.data()));
        }
        if (m_mode == Mode::Default || m_mode == Mode::HexDump) {
            m_mode = Mode::HexDump;
            return Formatter<StringView>::format(builder, value);
        }
        return Formatter<StringView>::format(builder, value);
    }
};

template<>
struct Formatter<Bytes> : Formatter<ReadonlyBytes> {
};

// FIXME: Printing raw char pointers is inherently dangerous. Remove this and
//        its users and prefer StringView over it.
template<>
struct Formatter<char const*> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, char const* value)
    {
        if (m_mode == Mode::Pointer) {
            Formatter<FlatPtr> formatter { *this };
            return formatter.format(builder, reinterpret_cast<FlatPtr>(value));
        }

        return Formatter<StringView>::format(builder, value != nullptr ? StringView { value, __builtin_strlen(value) } : "(null)"sv);
    }
};
template<>
struct Formatter<char*> : Formatter<char const*> {
};
template<size_t Size>
struct Formatter<char[Size]> : Formatter<char const*> {
};
template<>
struct Formatter<ByteString> : Formatter<StringView> {
};

template<typename T>
struct Formatter<T*> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, T* value)
    {
        if (m_mode == Mode::Default)
            m_mode = Mode::Pointer;

        Formatter<FlatPtr> formatter { *this };
        return formatter.format(builder, reinterpret_cast<FlatPtr>(value));
    }
};

template<>
struct Formatter<char> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder&, char);
};
template<>
struct Formatter<char32_t> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder& builder, char32_t);
};
template<>
struct Formatter<bool> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder&, bool);
};

template<>
struct Formatter<float> : StandardFormatter {
    ErrorOr<void> format(FormatBuilder&, float value);
};
template<>
struct Formatter<double> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(formatter)
    {
    }

    ErrorOr<void> format(FormatBuilder&, double);
};

template<>
struct Formatter<long double> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(formatter)
    {
    }

    ErrorOr<void> format(FormatBuilder&, long double value);
};

template<>
struct Formatter<f16> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(formatter)
    {
    }

    ErrorOr<void> format(FormatBuilder&, f16 value);
};

template<>
struct Formatter<nullptr_t> : Formatter<FlatPtr> {
    ErrorOr<void> format(FormatBuilder& builder, nullptr_t)
    {
        if (m_mode == Mode::Default)
            m_mode = Mode::Pointer;

        return Formatter<FlatPtr>::format(builder, 0);
    }
};

ErrorOr<void> vformat(StringBuilder&, StringView fmtstr, TypeErasedFormatParams&);

void vout(FILE*, StringView fmtstr, TypeErasedFormatParams&, bool newline = false);

template<typename... Parameters>
void out(FILE* file, CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vout(file, fmtstr.view(), variadic_format_params);
}

template<typename... Parameters>
void outln(FILE* file, CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vout(file, fmtstr.view(), variadic_format_params, true);
}

inline void outln(FILE* file) { fputc('\n', file); }

#ifndef AK_OS_ANDROID
template<typename... Parameters>
void out(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    out(stdout, move(fmtstr), parameters...);
}

template<typename... Parameters>
void outln(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters) { outln(stdout, move(fmtstr), parameters...); }

inline void outln() { outln(stdout); }

template<typename... Parameters>
void warn(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    out(stderr, move(fmtstr), parameters...);
}

template<typename... Parameters>
void warnln(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters) { outln(stderr, move(fmtstr), parameters...); }

inline void warnln() { outln(stderr); }
#else  // v Android ^ No Android
enum class LogLevel {
    Debug,
    Info,
    Warning,
};

void vout(LogLevel, StringView fmtstr, TypeErasedFormatParams&, bool newline = false);

template<typename... Parameters>
void out(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vout(LogLevel::Info, fmtstr.view(), variadic_format_params);
}

template<typename... Parameters>
void outln(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vout(LogLevel::Info, fmtstr.view(), variadic_format_params, true);
}

inline void outln() { outln(""); }

template<typename... Parameters>
void warn(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vout(LogLevel::Warning, fmtstr.view(), variadic_format_params);
}

template<typename... Parameters>
void warnln(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vout(LogLevel::Warning, fmtstr.view(), variadic_format_params, true);
}

inline void warnln() { warnln(""); }

void set_log_tag_name(char const*);
#endif // AK_OS_ANDROID

#define outln_if(flag, fmt, ...)       \
    do {                               \
        if constexpr (flag)            \
            outln(fmt, ##__VA_ARGS__); \
    } while (0)

#define warnln_if(flag, fmt, ...)       \
    do {                                \
        if constexpr (flag)             \
            warnln(fmt, ##__VA_ARGS__); \
    } while (0)

void vdbg(StringView fmtstr, TypeErasedFormatParams&, bool newline = false);

template<typename... Parameters>
void dbg(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vdbg(fmtstr.view(), variadic_format_params, false);
}

template<typename... Parameters>
void dbgln(CheckedFormatString<Parameters...>&& fmtstr, Parameters const&... parameters)
{
    VariadicFormatParams<AllowDebugOnlyFormatters::Yes, Parameters...> variadic_format_params { parameters... };
    vdbg(fmtstr.view(), variadic_format_params, true);
}

inline void dbgln() { dbgln(""); }

void set_debug_enabled(bool);
void set_rich_debug_enabled(bool);

template<typename T>
class FormatIfSupported {
public:
    explicit FormatIfSupported(T const& value)
        : m_value(value)
    {
    }

    T const& value() const { return m_value; }

private:
    T const& m_value;
};
template<typename T, bool Supported = false>
struct __FormatIfSupported : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, FormatIfSupported<T> const&)
    {
        return Formatter<StringView>::format(builder, "?"sv);
    }
};
template<typename T>
struct __FormatIfSupported<T, true> : Formatter<T> {
    ErrorOr<void> format(FormatBuilder& builder, FormatIfSupported<T> const& value)
    {
        return Formatter<T>::format(builder, value.value());
    }
};
template<typename T>
struct Formatter<FormatIfSupported<T>> : __FormatIfSupported<T, HasFormatter<T>> {
};

// This is a helper class, the idea is that if you want to implement a formatter you can inherit
// from this class to "break down" the formatting.
struct FormatString {
};
template<>
struct Formatter<FormatString> : Formatter<StringView> {
    template<typename... Parameters>
    ErrorOr<void> format(FormatBuilder& builder, StringView fmtstr, Parameters const&... parameters)
    {
        VariadicFormatParams<AllowDebugOnlyFormatters::No, Parameters...> variadic_format_params { parameters... };
        return vformat(builder, fmtstr, variadic_format_params);
    }
    ErrorOr<void> vformat(FormatBuilder& builder, StringView fmtstr, TypeErasedFormatParams& params);
};

template<>
struct Formatter<Error> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Error const& error);

private:
    ErrorOr<void> format_windows_error(FormatBuilder& builder, Error const& error);
};

template<typename T, typename ErrorType>
struct Formatter<ErrorOr<T, ErrorType>> : Formatter<FormatString> {
    static constexpr bool is_debug_only() { return true; }

    ErrorOr<void> format(FormatBuilder& builder, ErrorOr<T, ErrorType> const& error_or)
    {
        if (error_or.is_error())
            return Formatter<FormatString>::format(builder, "{}"sv, error_or.error());
        return Formatter<FormatString>::format(builder, "{{{}}}"sv, error_or.value());
    }
};

template<typename T>
struct Formatter<Optional<T>> : Formatter<FormatString> {
    ErrorOr<void> format(FormatBuilder& builder, Optional<T> const& optional)
    {
        if (optional.has_value())
            return Formatter<FormatString>::format(builder, "{}"sv, *optional);
        return builder.put_literal("None"sv);
    }
};

} // namespace AK

#undef AK_HANDLE_UNEXPECTED_ERROR
#define AK_HANDLE_UNEXPECTED_ERROR(result)                                    \
    if (result.is_error()) [[unlikely]] {                                     \
        if (ak_colorize_output()) {                                           \
            ::AK::warn("\033[31;1mUNEXPECTED ERROR\033[0m");                  \
        } else {                                                              \
            ::AK::warn("UNEXPECTED ERROR");                                   \
        }                                                                     \
        if constexpr (::AK::HasFormatter<decltype(result.release_error())>) { \
            ::AK::warn(": {}", result.release_error());                       \
        }                                                                     \
        ::AK::warnln(" at {}:{}", __FILE__, __LINE__);                        \
        ak_trap();                                                            \
    }

#if USING_AK_GLOBALLY
using AK::out;
using AK::outln;

using AK::warn;
using AK::warnln;

using AK::dbg;
using AK::dbgln;

using AK::CheckedFormatString;
using AK::FormatIfSupported;
using AK::FormatString;

#    define dbgln_if(flag, fmt, ...)       \
        do {                               \
            if constexpr (flag)            \
                dbgln(fmt, ##__VA_ARGS__); \
        } while (0)

#endif
