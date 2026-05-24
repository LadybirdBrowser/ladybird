/*
 * Copyright (C) 2011-2019 Apple Inc. All rights reserved.
 * Copyright (c) 2020-2021, Andreas Kling <andreas@ladybird.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice, this
 *    list of conditions and the following disclaimer.
 *
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include <AK/Assertions.h>
#include <AK/Concepts.h>
#include <AK/NumericLimits.h>
#include <AK/StdLibExtras.h>

namespace AK {

template<typename Destination, typename Source, bool destination_is_wider = (NumericLimits<Destination>::max() >= NumericLimits<Source>::max()), bool destination_is_signed = NumericLimits<Destination>::is_signed(), bool source_is_signed = NumericLimits<Source>::is_signed()>
struct TypeBoundsChecker;

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, false, false, false> {
    static constexpr bool is_within_range(Source value)
    {
        return value <= NumericLimits<Destination>::max();
    }
};

// Computes 2^N as a floating-point Source value. The result is exactly representable in any IEEE floating-point type
// for any N within that type's exponent range — since 2^N has mantissa 1.0.
template<FloatingPoint Source>
static constexpr Source two_to_the(size_t n)
{
    Source result = 1;
    for (size_t i = 0; i < n; ++i)
        result *= 2;
    return result;
}

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, false, true, true> {
    static constexpr bool is_within_range(Source value)
    {
        if constexpr (IsFloatingPoint<Source>) {
            // The value must lie in [Destination::min, Destination::max] *and* must be exactly representable as
            // Destination; that is, the float must be integer-valued — with no fractional part.
            //
            // First gate: the value must fit in the safe-cast window — so static_cast<Destination> below is defined.
            // Comparing directly against NumericLimits<Destination>::max() doesn't work when max isn't exactly
            // representable in Source (e.g., INT_MAX = 2^31-1 in float rounds up to 2^31). Use 2^digits as a strict
            // upper bound instead: exactly representable in any IEEE float — covering the range [-2^digits, 2^digits)
            // for two's-complement signed destinations.
            auto const boundary = two_to_the<Source>(NumericLimits<Destination>::digits());
            if (!(value >= -boundary && value < boundary))
                return false;
            // Second gate: reject fractional values (round-trip check). Values in (-boundary, boundary) with a
            // fractional part would truncate to an in-range integer — but aren't themselves within Destination's range.
            auto const truncated = static_cast<Destination>(value);
            return static_cast<Source>(truncated) == value;
        } else {
            return value <= NumericLimits<Destination>::max()
                && NumericLimits<Destination>::min() <= value;
        }
    }
};

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, false, false, true> {
    static constexpr bool is_within_range(Source value)
    {
        if constexpr (IsFloatingPoint<Source>) {
            // See the signed case above. The first gate is [0, 2^digits). 2^digits is exactly representable in any IEEE
            // float. The second gate is the round-trip check: rejecting fractional values whose magnitudes exceed
            // Destination::max.
            auto const upper_exclusive = two_to_the<Source>(NumericLimits<Destination>::digits());
            if (!(value >= 0 && value < upper_exclusive))
                return false;
            auto const truncated = static_cast<Destination>(value);
            return static_cast<Source>(truncated) == value;
        } else {
            return value >= 0 && value <= NumericLimits<Destination>::max();
        }
    }
};

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, false, true, false> {
    static constexpr bool is_within_range(Source value)
    {
        return value <= static_cast<Source>(NumericLimits<Destination>::max());
    }
};

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, true, false, false> {
    static constexpr bool is_within_range(Source)
    {
        return true;
    }
};

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, true, true, true> {
    static constexpr bool is_within_range(Source)
    {
        return true;
    }
};

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, true, false, true> {
    static constexpr bool is_within_range(Source value)
    {
        return value >= 0;
    }
};

template<typename Destination, typename Source>
struct TypeBoundsChecker<Destination, Source, true, true, false> {
    static constexpr bool is_within_range(Source value)
    {
        if (NumericLimits<Destination>::max() > NumericLimits<Source>::max())
            return true;
        return value <= static_cast<Source>(NumericLimits<Destination>::max());
    }
};

template<typename Destination, typename Source>
[[nodiscard]] constexpr bool is_within_range(Source value)
{
    return TypeBoundsChecker<Destination, Source>::is_within_range(value);
}

template<Integral T>
class Checked {
public:
    constexpr Checked() = default;

    explicit constexpr Checked(T value)
        : m_value(value)
    {
    }

    template<Integral U>
    constexpr Checked(U value)
    {
        m_overflow = !is_within_range<T>(value);
        m_value = value;
    }

    constexpr Checked(Checked const&) = default;

    constexpr Checked(Checked&& other)
        : m_value(exchange(other.m_value, 0))
        , m_overflow(exchange(other.m_overflow, false))
    {
    }

    template<typename U>
    constexpr Checked& operator=(U value)
    {
        *this = Checked(value);
        return *this;
    }

    constexpr Checked& operator=(Checked const& other) = default;

    constexpr Checked& operator=(Checked&& other)
    {
        m_value = exchange(other.m_value, 0);
        m_overflow = exchange(other.m_overflow, false);
        return *this;
    }

    [[nodiscard]] constexpr bool has_overflow() const
    {
        return m_overflow;
    }

    ALWAYS_INLINE constexpr bool operator!() const
    {
        VERIFY(!m_overflow);
        return !m_value;
    }

    ALWAYS_INLINE constexpr T value() const
    {
        VERIFY(!m_overflow);
        return m_value;
    }

    ALWAYS_INLINE constexpr T value_unchecked() const
    {
        return m_value;
    }

    constexpr void add(T other)
    {
        m_overflow |= __builtin_add_overflow(m_value, other, &m_value);
    }

    constexpr void sub(T other)
    {
        m_overflow |= __builtin_sub_overflow(m_value, other, &m_value);
    }

    constexpr void mul(T other)
    {
        m_overflow |= __builtin_mul_overflow(m_value, other, &m_value);
    }

    constexpr void div(T other)
    {
        if constexpr (IsSigned<T>) {
            // Ensure that the resulting value won't be out of range, this can only happen when dividing by -1.
            if (other == -1 && m_value == NumericLimits<T>::min()) {
                m_overflow = true;
                return;
            }
        }
        if (other == 0) {
            m_overflow = true;
            return;
        }
        m_value /= other;
    }

    constexpr void mod(T other)
    {
        auto initial = m_value;
        div(other);
        m_value *= other;
        m_value = initial - m_value;
    }

    constexpr Checked& operator+=(Checked const& other)
    {
        m_overflow |= other.m_overflow;
        add(other.value_unchecked());
        return *this;
    }

    constexpr Checked& operator+=(T other)
    {
        add(other);
        return *this;
    }

    constexpr Checked& operator-=(Checked const& other)
    {
        m_overflow |= other.m_overflow;
        sub(other.value_unchecked());
        return *this;
    }

    constexpr Checked& operator-=(T other)
    {
        sub(other);
        return *this;
    }

    constexpr Checked& operator*=(Checked const& other)
    {
        m_overflow |= other.m_overflow;
        mul(other.value_unchecked());
        return *this;
    }

    constexpr Checked& operator*=(T other)
    {
        mul(other);
        return *this;
    }

    constexpr Checked& operator/=(Checked const& other)
    {
        m_overflow |= other.m_overflow;
        div(other.value_unchecked());
        return *this;
    }

    constexpr Checked& operator/=(T other)
    {
        div(other);
        return *this;
    }

    constexpr Checked& operator%=(Checked const& other)
    {
        m_overflow |= other.m_overflow;
        mod(other.value_unchecked());
        return *this;
    }

    constexpr Checked& operator%=(T other)
    {
        mod(other);
        return *this;
    }

    constexpr Checked& operator++()
    {
        add(1);
        return *this;
    }

    constexpr Checked operator++(int)
    {
        Checked old { *this };
        add(1);
        return old;
    }

    constexpr Checked& operator--()
    {
        sub(1);
        return *this;
    }

    constexpr Checked operator--(int)
    {
        Checked old { *this };
        sub(1);
        return old;
    }

    template<typename U, typename V>
    [[nodiscard]] static constexpr bool addition_would_overflow(U u, V v)
    {
#if __has_builtin(__builtin_add_overflow_p)
        return __builtin_add_overflow_p(u, v, (T)0);
#else
        T result;
        return __builtin_add_overflow(u, v, &result);
#endif
    }

    template<typename... Ts>
    [[nodiscard]] static constexpr bool addition_would_overflow(Ts... values)
    requires(sizeof...(Ts) > 2)
    {
        Checked<T> result;
        ((result += values), ...);
        return result.has_overflow();
    }

    template<typename U, typename V>
    [[nodiscard]] static constexpr bool subtraction_would_overflow(U u, V v)
    {
#if __has_builtin(__builtin_sub_overflow_p)
        return __builtin_sub_overflow_p(u, v, (T)0);
#else
        T result;
        return __builtin_sub_overflow(u, v, &result);
#endif
    }

    template<typename U, typename V>
    [[nodiscard]] static constexpr bool multiplication_would_overflow(U u, V v)
    {
#if __has_builtin(__builtin_mul_overflow_p)
        return __builtin_mul_overflow_p(u, v, (T)0);
#else
        T result;
        return __builtin_mul_overflow(u, v, &result);
#endif
    }

private:
    T m_value {};
    bool m_overflow { false };
};

template<typename T>
constexpr Checked<T> operator+(Checked<T> const& a, Checked<T> const& b)
{
    Checked<T> c { a };
    c += b;
    return c;
}

template<typename T>
constexpr Checked<T> operator-(Checked<T> const& a, Checked<T> const& b)
{
    Checked<T> c { a };
    c -= b;
    return c;
}

template<typename T>
constexpr Checked<T> operator*(Checked<T> const& a, Checked<T> const& b)
{
    Checked<T> c { a };
    c *= b;
    return c;
}

template<typename T>
constexpr Checked<T> operator/(Checked<T> const& a, Checked<T> const& b)
{
    Checked<T> c { a };
    c /= b;
    return c;
}

template<typename T>
constexpr Checked<T> operator%(Checked<T> const& a, Checked<T> const& b)
{
    Checked<T> c { a };
    c %= b;
    return c;
}

template<typename T>
constexpr bool operator<(Checked<T> const& a, T b)
{
    return a.value() < b;
}

template<typename T>
constexpr bool operator>(Checked<T> const& a, T b)
{
    return a.value() > b;
}

template<typename T>
constexpr bool operator>=(Checked<T> const& a, T b)
{
    return a.value() >= b;
}

template<typename T>
constexpr bool operator<=(Checked<T> const& a, T b)
{
    return a.value() <= b;
}

template<typename T>
constexpr bool operator==(Checked<T> const& a, T b)
{
    return a.value() == b;
}

template<typename T>
constexpr bool operator!=(Checked<T> const& a, T b)
{
    return a.value() != b;
}

template<typename T>
constexpr bool operator<(T a, Checked<T> const& b)
{
    return a < b.value();
}

template<typename T>
constexpr bool operator>(T a, Checked<T> const& b)
{
    return a > b.value();
}

template<typename T>
constexpr bool operator>=(T a, Checked<T> const& b)
{
    return a >= b.value();
}

template<typename T>
constexpr bool operator<=(T a, Checked<T> const& b)
{
    return a <= b.value();
}

template<typename T>
constexpr bool operator==(T a, Checked<T> const& b)
{
    return a == b.value();
}

template<typename T>
constexpr bool operator!=(T a, Checked<T> const& b)
{
    return a != b.value();
}

template<typename T>
constexpr Checked<T> make_checked(T value)
{
    return Checked<T>(value);
}

}

#if USING_AK_GLOBALLY
using AK::Checked;
using AK::make_checked;
#endif
