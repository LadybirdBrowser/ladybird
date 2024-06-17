/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Format.h>
#include <AK/StdLibExtras.h>
#include <AK/Types.h>
#include <math.h>
#include <stdarg.h>
#include <wchar.h>

#ifdef AK_OS_SERENITY
extern "C" size_t strlen(char const*);
#else
#    include <string.h>
#endif

namespace PrintfImplementation {

template<typename PutChFunc, typename T, typename CharType>
ALWAYS_INLINE int print_hex(PutChFunc putch, CharType*& bufptr, T number, bool upper_case, bool alternate_form, bool left_pad, bool zero_pad, u32 field_width, bool has_precision, u32 precision)
{
    constexpr char const* printf_hex_digits_lower = "0123456789abcdef";
    constexpr char const* printf_hex_digits_upper = "0123456789ABCDEF";

    u32 digits = 0;
    for (T n = number; n > 0; n >>= 4)
        ++digits;
    if (digits == 0)
        digits = 1;

    bool not_zero = number != 0;

    char buf[16];
    char* p = buf;

    if (!(has_precision && precision == 0 && !not_zero)) {
        if (number == 0) {
            (*p++) = '0';
            if (precision > 0)
                precision--;
        } else {
            u8 shift_count = digits * 4;
            while (shift_count) {
                shift_count -= 4;
                (*p++) = upper_case
                    ? printf_hex_digits_upper[(number >> shift_count) & 0x0f]
                    : printf_hex_digits_lower[(number >> shift_count) & 0x0f];
                if (precision > 0)
                    precision--;
            }
        }
    }

    size_t numlen = p - buf;

    if (!field_width || field_width < (numlen + has_precision * precision + (alternate_form * 2 * not_zero)))
        field_width = numlen + has_precision * precision + alternate_form * 2 * not_zero;

    if ((zero_pad && !has_precision) && (alternate_form && not_zero)) {
        putch(bufptr, '0');
        putch(bufptr, 'x');
    }

    if (!left_pad) {
        for (unsigned i = 0; i < field_width - numlen - has_precision * precision - alternate_form * 2 * not_zero; ++i) {
            putch(bufptr, (zero_pad && !has_precision) ? '0' : ' ');
        }
    }

    if (!(zero_pad && !has_precision) && (alternate_form && not_zero)) {
        putch(bufptr, '0');
        putch(bufptr, 'x');
    }

    if (has_precision) {
        for (u32 i = 0; i < precision; ++i) {
            putch(bufptr, '0');
        }
    }

    for (unsigned i = 0; i < numlen; ++i) {
        putch(bufptr, buf[i]);
    }

    if (left_pad) {
        for (unsigned i = 0; i < field_width - numlen - has_precision * precision - alternate_form * 2 * not_zero; ++i) {
            putch(bufptr, ' ');
        }
    }

    return field_width;
}

template<typename PutChFunc, typename CharType>
ALWAYS_INLINE int print_decimal(PutChFunc putch, CharType*& bufptr, u64 number, bool sign, bool always_sign, bool left_pad, bool zero_pad, u32 field_width, bool has_precision, u32 precision)
{
    u64 divisor = 10000000000000000000LLU;
    char ch;
    char padding = 1;
    char buf[21];
    char* p = buf;

    if (!(has_precision && precision == 0 && number == 0)) {
        for (;;) {
            ch = '0' + (number / divisor);
            number %= divisor;
            if (ch != '0')
                padding = 0;
            if (!padding || divisor == 1) {
                *(p++) = ch;
                if (precision > 0)
                    precision--;
            }
            if (divisor == 1)
                break;
            divisor /= 10;
        }
    }

    size_t numlen = p - buf;

    if (!field_width || field_width < (numlen + has_precision * precision + (sign || always_sign)))
        field_width = numlen + has_precision * precision + (sign || always_sign);

    if ((zero_pad && !has_precision) && (sign || always_sign)) {
        putch(bufptr, sign ? '-' : '+');
    }

    if (!left_pad) {
        for (unsigned i = 0; i < field_width - numlen - has_precision * precision - (sign || always_sign); ++i) {
            putch(bufptr, (zero_pad && !has_precision) ? '0' : ' ');
        }
    }

    if (!(zero_pad && !has_precision) && (sign || always_sign)) {
        putch(bufptr, sign ? '-' : '+');
    }

    if (has_precision) {
        for (u32 i = 0; i < precision; ++i) {
            putch(bufptr, '0');
        }
    }

    for (unsigned i = 0; i < numlen; ++i) {
        putch(bufptr, buf[i]);
    }

    if (left_pad) {
        for (unsigned i = 0; i < field_width - numlen - has_precision * precision - (sign || always_sign); ++i) {
            putch(bufptr, ' ');
        }
    }

    return field_width;
}
template<typename PutChFunc, typename CharType>
ALWAYS_INLINE int print_double(PutChFunc putch, CharType*& bufptr, double number, bool always_sign, bool left_pad, bool zero_pad, u32 field_width, u32 precision, bool trailing_zeros)
{
    int length = 0;

    u32 whole_width = (field_width >= precision + 1) ? field_width - precision - 1 : 0;

    bool sign = signbit(number);
    bool nan = isnan(number);
    bool inf = isinf(number);

    if (nan || inf) {
        for (unsigned i = 0; i < field_width - 3 - sign; i++) {
            putch(bufptr, ' ');
            length++;
        }
        if (sign) {
            putch(bufptr, '-');
            length++;
        }
        if (nan) {
            putch(bufptr, 'n');
            putch(bufptr, 'a');
            putch(bufptr, 'n');
        } else {
            putch(bufptr, 'i');
            putch(bufptr, 'n');
            putch(bufptr, 'f');
        }
        return length + 3;
    }

    if (sign)
        number = -number;

    length = print_decimal(putch, bufptr, (i64)number, sign, always_sign, left_pad, zero_pad, whole_width, false, 1);
    if (precision > 0) {
        double fraction = number - (i64)number;

        for (u32 i = 0; i < precision; ++i)
            fraction = fraction * 10;
        if (trailing_zeros || fraction) {
            length++;
            putch(bufptr, '.');

            i64 ifraction = fraction;
            while (!trailing_zeros && ifraction % 10 == 0) {
                ifraction /= 10;
                precision--;
            }

            return length + print_decimal(putch, bufptr, ifraction, false, false, false, true, precision, false, 1);
        }
    }

    return length;
}
template<typename PutChFunc, typename CharType>
ALWAYS_INLINE int print_octal_number(PutChFunc putch, CharType*& bufptr, u64 number, bool alternate_form, bool left_pad, bool zero_pad, u32 field_width, bool has_precision, u32 precision)
{
    u32 divisor = 134217728;
    char ch;
    char padding = 1;
    char buf[32];
    char* p = buf;

    if (alternate_form) {
        (*p++) = '0';
        if (precision > 0)
            precision--;
    }

    if (!(has_precision && precision == 0 && number == 0)) {
        for (;;) {
            ch = '0' + (number / divisor);
            number %= divisor;
            if (ch != '0')
                padding = 0;
            if (!padding || divisor == 1) {
                *(p++) = ch;
                if (precision > 0)
                    precision--;
            }
            if (divisor == 1)
                break;
            divisor /= 8;
        }
    }

    size_t numlen = p - buf;

    if (!field_width || field_width < (numlen + has_precision * precision))
        field_width = numlen + has_precision * precision;

    if (!left_pad) {
        for (unsigned i = 0; i < field_width - numlen - has_precision * precision; ++i) {
            putch(bufptr, (zero_pad && !has_precision) ? '0' : ' ');
        }
    }

    if (has_precision) {
        for (u32 i = 0; i < precision; ++i) {
            putch(bufptr, '0');
        }
    }

    for (unsigned i = 0; i < numlen; ++i) {
        putch(bufptr, buf[i]);
    }

    if (left_pad) {
        for (unsigned i = 0; i < field_width - numlen - has_precision * precision; ++i) {
            putch(bufptr, ' ');
        }
    }

    return field_width;
}

template<typename PutChFunc, typename T, typename CharType>
ALWAYS_INLINE int print_string(PutChFunc putch, CharType*& bufptr, T str, size_t len, bool left_pad, size_t field_width, bool dot, size_t precision, bool has_fraction)
{
    if (has_fraction)
        len = min(len, precision);

    if (!dot && (!field_width || field_width < len))
        field_width = len;

    if (has_fraction && !field_width)
        field_width = len;

    size_t pad_amount = field_width > len ? field_width - len : 0;

    if (!left_pad) {
        for (size_t i = 0; i < pad_amount; ++i)
            putch(bufptr, ' ');
    }
    for (size_t i = 0; i < min(len, field_width); ++i) {
        putch(bufptr, str[i]);
    }
    if (left_pad) {
        for (size_t i = 0; i < pad_amount; ++i)
            putch(bufptr, ' ');
    }
    return field_width;
}

template<typename PutChFunc, typename CharType>
ALWAYS_INLINE int print_signed_number(PutChFunc putch, CharType*& bufptr, i64 number, bool always_sign, bool left_pad, bool zero_pad, u32 field_width, bool has_precision, u32 precision)
{
    // FIXME: `0 - number` overflows if we are trying to negate the smallest possible value.
    return print_decimal(putch, bufptr, (number < 0) ? 0 - number : number, number < 0, always_sign, left_pad, zero_pad, field_width, has_precision, precision);
}

struct ModifierState {
    bool left_pad { false };
    bool zero_pad { false };
    bool dot { false };
    unsigned field_width { 0 };
    bool has_precision { false };
    unsigned precision { 6 };
    unsigned short_qualifiers { 0 }; // TODO: Unimplemented.
    unsigned long_qualifiers { 0 };
    bool intmax_qualifier { false };      // TODO: Unimplemented.
    bool ptrdiff_qualifier { false };     // TODO: Unimplemented.
    bool long_double_qualifier { false }; // TODO: Unimplemented.
    bool size_qualifier { false };        // TODO: Unimplemented.
    bool alternate_form { 0 };
    bool always_sign { false };
};

template<typename PutChFunc, typename ArgumentListRefT, template<typename T, typename U = ArgumentListRefT> typename NextArgument, typename CharType = char>
struct PrintfImpl {
    ALWAYS_INLINE PrintfImpl(PutChFunc& putch, CharType*& bufptr, int const& nwritten)
        : m_bufptr(bufptr)
        , m_nwritten(nwritten)
        , m_putch(putch)
    {
    }

    ALWAYS_INLINE int format_s(ModifierState const& state, ArgumentListRefT ap) const
    {
        // FIXME: Narrow characters should be converted to wide characters on the fly and vice versa.
        // https://pubs.opengroup.org/onlinepubs/9699919799/functions/printf.html
        // https://pubs.opengroup.org/onlinepubs/9699919799/functions/wprintf.html
        if (state.long_qualifiers) {
            wchar_t const* sp = NextArgument<wchar_t const*>()(ap);
            if (!sp)
                sp = L"(null)";
            return print_string(m_putch, m_bufptr, sp, wcslen(sp), state.left_pad, state.field_width, state.dot, state.precision, state.has_precision);
        }
        char const* sp = NextArgument<char const*>()(ap);
        if (!sp)
            sp = "(null)";
        return print_string(m_putch, m_bufptr, sp, strlen(sp), state.left_pad, state.field_width, state.dot, state.precision, state.has_precision);
    }
    ALWAYS_INLINE int format_d(ModifierState const& state, ArgumentListRefT ap) const
    {
        i64 number = [&]() -> i64 {
            if (state.long_qualifiers >= 2)
                return NextArgument<long long int>()(ap);
            if (state.long_qualifiers == 1)
                return NextArgument<long int>()(ap);
            return NextArgument<int>()(ap);
        }();

        return print_signed_number(m_putch, m_bufptr, number, state.always_sign, state.left_pad, state.zero_pad, state.field_width, state.has_precision, state.precision);
    }
    ALWAYS_INLINE int format_i(ModifierState const& state, ArgumentListRefT ap) const
    {
        return format_d(state, ap);
    }
    ALWAYS_INLINE int format_u(ModifierState const& state, ArgumentListRefT ap) const
    {
        u64 number = [&]() -> u64 {
            if (state.long_qualifiers >= 2)
                return NextArgument<unsigned long long int>()(ap);
            if (state.long_qualifiers == 1)
                return NextArgument<unsigned long int>()(ap);
            return NextArgument<unsigned int>()(ap);
        }();

        return print_decimal(m_putch, m_bufptr, number, false, false, state.left_pad, state.zero_pad, state.field_width, state.has_precision, state.precision);
    }
    ALWAYS_INLINE int format_Q(ModifierState const& state, ArgumentListRefT ap) const
    {
        return print_decimal(m_putch, m_bufptr, NextArgument<u64>()(ap), false, false, state.left_pad, state.zero_pad, state.field_width, state.has_precision, state.precision);
    }
    ALWAYS_INLINE int format_q(ModifierState const& state, ArgumentListRefT ap) const
    {
        return print_hex(m_putch, m_bufptr, NextArgument<u64>()(ap), false, false, state.left_pad, state.zero_pad, 16, false, 1);
    }
    ALWAYS_INLINE int format_g(ModifierState const& state, ArgumentListRefT ap) const
    {
        // FIXME: Exponent notation
        return print_double(m_putch, m_bufptr, NextArgument<double>()(ap), state.always_sign, state.left_pad, state.zero_pad, state.field_width, state.precision, false);
    }
    ALWAYS_INLINE int format_f(ModifierState const& state, ArgumentListRefT ap) const
    {
        return print_double(m_putch, m_bufptr, NextArgument<double>()(ap), state.always_sign, state.left_pad, state.zero_pad, state.field_width, state.precision, true);
    }
    ALWAYS_INLINE int format_o(ModifierState const& state, ArgumentListRefT ap) const
    {
        return print_octal_number(m_putch, m_bufptr, NextArgument<u32>()(ap), state.alternate_form, state.left_pad, state.zero_pad, state.field_width, state.has_precision, state.precision);
    }
    ALWAYS_INLINE int format_unsigned_hex(ModifierState const& state, ArgumentListRefT ap, bool uppercase) const
    {
        u64 number = [&]() -> u64 {
            if (state.long_qualifiers >= 2)
                return NextArgument<unsigned long long int>()(ap);
            if (state.long_qualifiers == 1)
                return NextArgument<unsigned long int>()(ap);
            return NextArgument<unsigned int>()(ap);
        }();

        return print_hex(m_putch, m_bufptr, number, uppercase, state.alternate_form, state.left_pad, state.zero_pad, state.field_width, state.has_precision, state.precision);
    }
    ALWAYS_INLINE int format_x(ModifierState const& state, ArgumentListRefT ap) const
    {
        return format_unsigned_hex(state, ap, false);
    }
    ALWAYS_INLINE int format_X(ModifierState const& state, ArgumentListRefT ap) const
    {
        return format_unsigned_hex(state, ap, true);
    }
    ALWAYS_INLINE int format_n(ModifierState const&, ArgumentListRefT ap) const
    {
        *NextArgument<int*>()(ap) = m_nwritten;
        return 0;
    }
    ALWAYS_INLINE int format_p(ModifierState const&, ArgumentListRefT ap) const
    {
        return print_hex(m_putch, m_bufptr, NextArgument<FlatPtr>()(ap), false, true, false, true, 8, false, 1);
    }
    ALWAYS_INLINE int format_P(ModifierState const&, ArgumentListRefT ap) const
    {
        return print_hex(m_putch, m_bufptr, NextArgument<FlatPtr>()(ap), true, true, false, true, 8, false, 1);
    }
    ALWAYS_INLINE int format_percent(ModifierState const&, ArgumentListRefT) const
    {
        m_putch(m_bufptr, '%');
        return 1;
    }
    ALWAYS_INLINE int format_c(ModifierState const& state, ArgumentListRefT ap) const
    {
        char c = NextArgument<int>()(ap);
        return print_string(m_putch, m_bufptr, &c, 1, state.left_pad, state.field_width, state.dot, state.precision, state.has_precision);
    }
    ALWAYS_INLINE int format_unrecognized(CharType format_op, CharType const* fmt, ModifierState const&, ArgumentListRefT) const
    {
        dbgln("printf_internal: Unimplemented format specifier {} (fmt: {})", format_op, fmt);
        return 0;
    }

protected:
    CharType*& m_bufptr;
    int const& m_nwritten;
    PutChFunc& m_putch;
};

template<typename T, typename V>
struct VaArgNextArgument {
    ALWAYS_INLINE T operator()(V ap) const
    {
#ifdef AK_OS_WINDOWS
        // GCC on msys2 complains about the type of ap,
        // so let's force the compiler to believe it's a
        // va_list.
        return va_arg((va_list&)ap, T);
#else
        return va_arg(ap, T);
#endif
    }
};

#define PRINTF_IMPL_DELEGATE_TO_IMPL(c)    \
    case *#c:                              \
        ret += impl.format_##c(state, ap); \
        break;

template<typename PutChFunc, template<typename T, typename U, template<typename X, typename Y> typename V, typename C = char> typename Impl = PrintfImpl, typename ArgumentListT = va_list, template<typename T, typename V = decltype(declval<ArgumentListT&>())> typename NextArgument = VaArgNextArgument, typename CharType = char>
ALWAYS_INLINE int printf_internal(PutChFunc putch, IdentityType<CharType>* buffer, CharType const*& fmt, ArgumentListT ap)
{
    int ret = 0;
    CharType* bufptr = buffer;

    Impl<PutChFunc, ArgumentListT&, NextArgument, CharType> impl { putch, bufptr, ret };

    for (CharType const* p = fmt; *p; ++p) {
        ModifierState state;
        if (*p == '%' && *(p + 1)) {
        one_more:
            ++p;
            if (*p == '.') {
                state.dot = true;
                state.has_precision = true;
                state.precision = 0;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == '-') {
                state.left_pad = true;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == '+') {
                state.always_sign = true;
                if (*(p + 1))
                    goto one_more;
            }
            if (!state.zero_pad && !state.field_width && !state.dot && *p == '0') {
                state.zero_pad = true;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p >= '0' && *p <= '9') {
                if (!state.dot) {
                    state.field_width *= 10;
                    state.field_width += *p - '0';
                    if (*(p + 1))
                        goto one_more;
                } else {
                    state.precision *= 10;
                    state.precision += *p - '0';
                    if (*(p + 1))
                        goto one_more;
                }
            }
            if (*p == '*') {
                if (state.dot) {
                    state.zero_pad = true;
                    state.precision = NextArgument<int>()(ap);
                } else {
                    state.field_width = NextArgument<int>()(ap);
                }

                if (*(p + 1))
                    goto one_more;
            }
            if (*p == 'h') {
                ++state.short_qualifiers;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == 'l') {
                ++state.long_qualifiers;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == 'j') {
                state.intmax_qualifier = true;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == 't') {
                state.ptrdiff_qualifier = true;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == 'L') {
                state.long_double_qualifier = true;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == 'z') {
                state.size_qualifier = true;
                if (*(p + 1))
                    goto one_more;
            }
            if (*p == '#') {
                state.alternate_form = true;
                if (*(p + 1))
                    goto one_more;
            }
            switch (*p) {
            case '%':
                ret += impl.format_percent(state, ap);
                break;

                PRINTF_IMPL_DELEGATE_TO_IMPL(P);
                PRINTF_IMPL_DELEGATE_TO_IMPL(Q);
                PRINTF_IMPL_DELEGATE_TO_IMPL(X);
                PRINTF_IMPL_DELEGATE_TO_IMPL(c);
                PRINTF_IMPL_DELEGATE_TO_IMPL(d);
                PRINTF_IMPL_DELEGATE_TO_IMPL(f);
                PRINTF_IMPL_DELEGATE_TO_IMPL(g);
                PRINTF_IMPL_DELEGATE_TO_IMPL(i);
                PRINTF_IMPL_DELEGATE_TO_IMPL(n);
                PRINTF_IMPL_DELEGATE_TO_IMPL(o);
                PRINTF_IMPL_DELEGATE_TO_IMPL(p);
                PRINTF_IMPL_DELEGATE_TO_IMPL(q);
                PRINTF_IMPL_DELEGATE_TO_IMPL(s);
                PRINTF_IMPL_DELEGATE_TO_IMPL(u);
                PRINTF_IMPL_DELEGATE_TO_IMPL(x);
            default:
                ret += impl.format_unrecognized(*p, fmt, state, ap);
                break;
            }
        } else {
            putch(bufptr, *p);
            ++ret;
        }
    }
    return ret;
}

#undef PRINTF_IMPL_DELEGATE_TO_IMPL

}

using PrintfImplementation::printf_internal;
