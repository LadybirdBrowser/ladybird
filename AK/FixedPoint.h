/*
 * Copyright (c) 2021, Leon Albrecht <leon2002.la@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Format.h>
#include <AK/IntegralMath.h>
#include <AK/Math.h>
#include <AK/NumericLimits.h>
#include <AK/Types.h>

#ifndef __SIZEOF_INT128__
#    include <AK/UFixedBigInt.h>
#    include <AK/UFixedBigIntDivision.h>
#endif

// Solaris' definition of signbit in math_c99.h conflicts with our implementation.
#ifdef AK_OS_SOLARIS
#    undef signbit
#endif

namespace AK {

// FIXME: this always uses round to nearest break-tie to even
// FIXME: use the Integral concept to constrain Underlying
template<size_t precision, typename Underlying>
class FixedPoint {
    using This = FixedPoint<precision, Underlying>;
    constexpr static Underlying radix_mask = (static_cast<Underlying>(1) << precision) - 1;

    template<size_t P, typename U>
    friend class FixedPoint;

public:
    constexpr FixedPoint() = default;
    template<Integral I>
    constexpr FixedPoint(I value)
        : m_value(static_cast<Underlying>(value) << precision)
    {
    }

    template<FloatingPoint F>
    FixedPoint(F value)
        : m_value(round_to<Underlying>(value * (static_cast<Underlying>(1) << precision)))
    {
    }

    template<size_t P, typename U>
    explicit constexpr FixedPoint(FixedPoint<P, U> const& other)
        : m_value(other.template cast_to<precision, Underlying>().m_value)
    {
    }

    template<FloatingPoint F>
    explicit ALWAYS_INLINE operator F() const
    {
        return (F)m_value * pow<F>(0.5, precision);
    }

    template<Integral I>
    explicit constexpr operator I() const
    {
        return trunc().raw() >> precision;
    }

    static constexpr This create_raw(Underlying value)
    {
        This t {};
        t.raw() = value;
        return t;
    }

    constexpr Underlying raw() const
    {
        return m_value;
    }
    constexpr Underlying& raw()
    {
        return m_value;
    }

    constexpr This fract() const
    {
        return create_raw(m_value & radix_mask);
    }

    constexpr This clamp(This minimum, This maximum) const
    {
        if (*this < minimum)
            return minimum;
        if (*this > maximum)
            return maximum;
        return *this;
    }

    constexpr This rint() const
    {
        // Note: Round fair, break tie to even
        Underlying value = m_value >> precision;

        // Note: For negative numbers the ordering are reversed,
        //       and they were already decremented by the shift, so we need to
        //       add 1 when we see a fract values behind the `.5`s place set,
        //       because that means they are smaller than .5
        // fract(m_value) >= .5?
        if (m_value & (static_cast<Underlying>(1) << (precision - 1))) {
            // fract(m_value) > .5?
            if (m_value & (radix_mask >> 1)) {
                // yes: round "up";
                value += 1;
            } else {
                //  no: round to even;
                value += value & 1;
            }
        }
        return value;
    }
    constexpr This floor() const
    {
        return create_raw(m_value & ~radix_mask);
    }
    constexpr This ceil() const
    {
        return create_raw((m_value & ~radix_mask)
            + (m_value & radix_mask ? static_cast<Underlying>(1) << precision : 0));
    }
    constexpr This trunc() const
    {
        return create_raw((m_value & ~radix_mask)
            + ((m_value & radix_mask)
                    ? (m_value > 0 ? 0 : (static_cast<Underlying>(1) << precision))
                    : 0));
    }

    constexpr Underlying lrint() const { return rint().raw() >> precision; }
    constexpr Underlying lfloor() const { return m_value >> precision; }
    constexpr Underlying lceil() const
    {
        return (m_value >> precision)
            + (m_value & radix_mask ? 1 : 0);
    }
    constexpr Underlying ltrunc() const
    {
        return (m_value >> precision)
            + ((m_value & radix_mask)
                    ? m_value > 0 ? 0 : 1
                    : 0);
    }

    // http://www.claysturner.com/dsp/BinaryLogarithm.pdf
    constexpr This log2() const
    {
        // 0.5
        This b = create_raw(static_cast<Underlying>(1) << (precision - 1));
        This y = 0;
        This x = *this;

        // FIXME: There's no negative infinity.
        if (x.raw() <= 0)
            return create_raw(NumericLimits<Underlying>::min());

        if (x != 1) {
            i32 shift_amount = AK::log2<Underlying>(x.raw()) - precision;
            if (shift_amount > 0)
                x >>= shift_amount;
            else
                x <<= -shift_amount;
            y += shift_amount;
        }

        for (size_t i = 0; i < precision; ++i) {
            x *= x;
            if (x >= 2) {
                x >>= 1;
                y += b;
            }
            b >>= 1;
        }

        return y;
    }

    constexpr bool signbit() const
    requires(IsSigned<Underlying>)
    {
        return m_value >> (sizeof(Underlying) * 8 - 1);
    }

    constexpr This operator-() const
    requires(IsSigned<Underlying>)
    {
        return create_raw(-m_value);
    }

    constexpr This operator+(This const& other) const
    {
        return create_raw(m_value + other.m_value);
    }
    constexpr This operator-(This const& other) const
    {
        return create_raw(m_value - other.m_value);
    }
    constexpr This operator*(This const& other) const
    {
#ifdef __SIZEOF_INT128__
        // FIXME: Figure out a nicer way to use more narrow types and avoid __int128
        using MulRes = Conditional<sizeof(Underlying) < sizeof(i64), i64, __int128>;
        MulRes value = raw();
        value *= other.raw();

        This ret = create_raw(value >> precision);
#else
        // Note: We sign extend the raw value to a u128 to emulate the signed multiplication
        //       done in the version above
        // FIXME: Provide narrower intermediate results types
        u128 value = { (u64)(i64)raw(), ~0ull * (raw() < 0) };
        value *= (u64)(i64)other.raw();

        This ret = create_raw((value >> precision).low());
#endif
        // Rounding:
        // If last bit cut off is 1:
        if (value & (static_cast<Underlying>(1) << (precision - 1))) {
            // If the bit after is 1 as well
            if (value & (radix_mask >> 1)) {
                // We round away from 0
                ret.raw() += 1;
            } else {
                // Otherwise we round to the next even value
                // Which means we add the least significant bit of the raw return value
                ret.raw() += ret.raw() & 1;
            }
        }
        return ret;
    }
    constexpr This operator/(This const& other) const
    {
#ifdef __SIZEOF_INT128__
        // FIXME: Figure out a nicer way to use more narrow types and avoid __int128
        using DivRes = Conditional<sizeof(Underlying) < sizeof(i64), i64, __int128>;

        DivRes value = raw();
        value <<= precision;
        value /= other.raw();

        return create_raw(value);
#else
        // Note: We sign extend the raw value to a u128 to emulate the wide division
        //       done in the version above
        if constexpr (sizeof(Underlying) > sizeof(u32)) {
            u128 value = { (u64)(i64)raw(), ~0ull * (raw() < 0) };

            value <<= precision;
            value /= (u64)(i64)other.raw();

            return create_raw(value.low());
        }
        // FIXME: Maybe allow going even narrower
        using DivRes = Conditional<sizeof(Underlying) < sizeof(i32), i32, i64>;
        DivRes value = raw();
        value <<= precision;
        value /= other.raw();

        return create_raw(value);
#endif
    }

    template<Integral I>
    constexpr This operator+(I other) const
    {
        return create_raw(m_value + (static_cast<Underlying>(other) << precision));
    }
    template<Integral I>
    constexpr This operator-(I other) const
    {
        return create_raw(m_value - (static_cast<Underlying>(other) << precision));
    }
    template<Integral I>
    constexpr This operator*(I other) const
    {
        return create_raw(m_value * other);
    }
    template<Integral I>
    constexpr This operator/(I other) const
    {
        return create_raw(m_value / other);
    }
    template<Integral I>
    constexpr This operator>>(I other) const
    {
        return create_raw(m_value >> other);
    }
    template<Integral I>
    constexpr This operator<<(I other) const
    {
        return create_raw(m_value << other);
    }

    This& operator+=(This const& other)
    {
        m_value += other.raw();
        return *this;
    }
    This& operator-=(This const& other)
    {
        m_value -= other.raw();
        return *this;
    }
    This& operator*=(This const& other)
    {
        *this = *this * other;
        return *this;
    }
    This& operator/=(This const& other)
    {
        *this = *this / other;
        return *this;
    }

    template<Integral I>
    This& operator+=(I other)
    {
        m_value += static_cast<Underlying>(other) << precision;
        return *this;
    }
    template<Integral I>
    This& operator-=(I other)
    {
        m_value -= static_cast<Underlying>(other) << precision;
        return *this;
    }
    template<Integral I>
    This& operator*=(I other)
    {
        m_value *= other;
        return *this;
    }
    template<Integral I>
    This& operator/=(I other)
    {
        m_value /= other;
        return *this;
    }
    template<Integral I>
    This& operator>>=(I other)
    {
        m_value >>= other;
        return *this;
    }
    template<Integral I>
    This& operator<<=(I other)
    {
        m_value <<= other;
        return *this;
    }

    bool operator==(This const& other) const { return raw() == other.raw(); }
    bool operator!=(This const& other) const { return raw() != other.raw(); }
    bool operator>(This const& other) const { return raw() > other.raw(); }
    bool operator>=(This const& other) const { return raw() >= other.raw(); }
    bool operator<(This const& other) const { return raw() < other.raw(); }
    bool operator<=(This const& other) const { return raw() <= other.raw(); }

    // FIXME: There are probably better ways to do these
    template<Integral I>
    bool operator==(I other) const
    {
        return m_value >> precision == other && !(m_value & radix_mask);
    }
    template<Integral I>
    bool operator!=(I other) const
    {
        return (m_value >> precision) != other || m_value & radix_mask;
    }
    template<Integral I>
    bool operator>(I other) const
    {
        return !(*this <= other);
    }
    template<Integral I>
    bool operator>=(I other) const
    {
        return !(*this < other);
    }
    template<Integral I>
    bool operator<(I other) const
    {
        return (m_value >> precision) < other || m_value < (static_cast<Underlying>(other) << precision);
    }
    template<Integral I>
    bool operator<=(I other) const
    {
        return *this < other || *this == other;
    }

    // Casting from a float should be faster than casting to a float
    template<FloatingPoint F>
    bool operator==(F other) const { return *this == (This)other; }
    template<FloatingPoint F>
    bool operator!=(F other) const { return *this != (This)other; }
    template<FloatingPoint F>
    bool operator>(F other) const { return *this > (This)other; }
    template<FloatingPoint F>
    bool operator>=(F other) const { return *this >= (This)other; }
    template<FloatingPoint F>
    bool operator<(F other) const { return *this < (This)other; }
    template<FloatingPoint F>
    bool operator<=(F other) const { return *this <= (This)other; }

    template<size_t P, typename U>
    operator FixedPoint<P, U>() const
    {
        return cast_to<P, U>();
    }

private:
    template<size_t P, typename U>
    constexpr FixedPoint<P, U> cast_to() const
    {
        U raw_value = static_cast<U>(m_value >> precision) << P;
        if constexpr (precision > P)
            raw_value |= (m_value & radix_mask) >> (precision - P);
        else if constexpr (precision < P)
            raw_value |= static_cast<U>(m_value & radix_mask) << (P - precision);
        else
            raw_value |= m_value & radix_mask;

        return FixedPoint<P, U>::create_raw(raw_value);
    }

    Underlying m_value;
};

template<size_t precision, typename Underlying>
struct Formatter<FixedPoint<precision, Underlying>> : StandardFormatter {
    Formatter() = default;
    explicit Formatter(StandardFormatter formatter)
        : StandardFormatter(formatter)
    {
    }

    ErrorOr<void> format(FormatBuilder& builder, FixedPoint<precision, Underlying> value)
    {
        u8 base;
        bool upper_case = false;
        if (m_mode == Mode::Default || m_mode == Mode::FixedPoint) {
            base = 10;
        } else if (m_mode == Mode::Hexfloat) {
            base = 16;
        } else if (m_mode == Mode::HexfloatUppercase) {
            base = 16;
            upper_case = true;
        } else if (m_mode == Mode::Binary) {
            base = 2;
        } else if (m_mode == Mode::BinaryUppercase) {
            base = 2;
            upper_case = true;
        } else if (m_mode == Mode::Octal) {
            base = 8;
        } else {
            VERIFY_NOT_REACHED();
        }

        m_width = m_width.value_or(0);
        m_precision = m_precision.value_or(6);

        bool is_negative = false;
        if constexpr (IsSigned<Underlying>)
            is_negative = value < 0;

        i64 integer = value.ltrunc();
        constexpr u64 one = static_cast<Underlying>(1) << precision;
        u64 fraction_raw = value.raw() & (one - 1);
        return builder.put_fixed_point(is_negative, integer, fraction_raw, one, precision, base, upper_case, m_zero_pad, m_use_separator, m_align, m_width.value(), m_precision.value(), m_fill, m_sign_mode);
    }
};

}
