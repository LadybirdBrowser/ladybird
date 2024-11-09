/*
 * Copyright (c) 2023, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2012-2023, Apple Inc. All rights reserved.
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Debug.h>
#include <AK/DistinctNumeric.h>
#include <AK/Math.h>
#include <AK/Traits.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Rect.h>
#include <LibIPC/Forward.h>
#include <math.h>

namespace Web {

/// DevicePixels: A position or length on the physical display.
AK_TYPEDEF_DISTINCT_NUMERIC_GENERAL(int, DevicePixels, Arithmetic, CastToUnderlying, Comparison, Increment);

template<Integral T>
constexpr bool operator==(DevicePixels left, T right) { return left.value() == right; }

template<Integral T>
constexpr bool operator!=(DevicePixels left, T right) { return left.value() != right; }

template<Integral T>
constexpr bool operator>(DevicePixels left, T right) { return left.value() > right; }

template<Integral T>
constexpr bool operator<(DevicePixels left, T right) { return left.value() < right; }

template<Integral T>
constexpr bool operator>=(DevicePixels left, T right) { return left.value() >= right; }

template<Integral T>
constexpr bool operator<=(DevicePixels left, T right) { return left.value() <= right; }

template<Integral T>
constexpr DevicePixels operator*(DevicePixels left, T right) { return left.value() * right; }
template<Integral T>
constexpr DevicePixels operator*(T left, DevicePixels right) { return right * left; }

template<Integral T>
constexpr DevicePixels operator/(DevicePixels left, T right) { return left.value() / right; }

template<Integral T>
constexpr DevicePixels operator%(DevicePixels left, T right) { return left.value() % right; }

class CSSPixelFraction;

/// CSSPixels: A position or length in CSS "reference pixels", independent of zoom or screen DPI.
/// See https://www.w3.org/TR/css-values-3/#reference-pixel
class CSSPixels {
public:
    static constexpr i32 fractional_bits = 6;
    static constexpr i32 fixed_point_denominator = 1 << fractional_bits;

    static constexpr i32 radix_mask = fixed_point_denominator - 1;

    static constexpr i32 max_integer_value = NumericLimits<int>::max() >> fractional_bits;
    static constexpr i32 min_integer_value = NumericLimits<int>::min() >> fractional_bits;

    constexpr CSSPixels() = default;
    template<Signed I>
    constexpr CSSPixels(I value)
    {
        if (value > max_integer_value) [[unlikely]]
            m_value = NumericLimits<int>::max();
        else if (value < min_integer_value) [[unlikely]]
            m_value = NumericLimits<int>::min();
        else
            m_value = static_cast<int>(value) << fractional_bits;
    }

    template<FloatingPoint F>
    explicit CSSPixels(F value)
    {
        *this = nearest_value_for(value);
    }

    template<FloatingPoint F>
    static CSSPixels nearest_value_for(F value)
    {
        i32 raw_value = 0;
        if (!isnan(value))
            raw_value = AK::clamp_to<int>(value * fixed_point_denominator);
        // Note: The resolution of CSSPixels is 0.015625, so care must be taken when converting
        // floats/doubles to CSSPixels as small values (such as scale factors) can underflow to zero,
        // or otherwise produce inaccurate results (when scaled back up).
        if (raw_value == 0 && value != 0)
            dbgln_if(LIBWEB_CSS_DEBUG, "CSSPixels: Conversion from float or double underflowed to zero");
        return from_raw(raw_value);
    }

    template<FloatingPoint F>
    static CSSPixels floored_value_for(F value)
    {
        i32 raw_value = 0;
        if (!isnan(value))
            raw_value = AK::clamp_to<int>(floor(value * fixed_point_denominator));
        return from_raw(raw_value);
    }

    template<Unsigned U>
    constexpr CSSPixels(U value)
    {
        if (value > max_integer_value) [[unlikely]]
            m_value = NumericLimits<int>::max();
        else
            m_value = static_cast<int>(value) << fractional_bits;
    }

    static constexpr CSSPixels from_raw(int value)
    {
        CSSPixels res;
        res.set_raw_value(value);
        return res;
    }

    static constexpr CSSPixels min()
    {
        return from_raw(NumericLimits<int>::min());
    }

    static constexpr CSSPixels max()
    {
        return from_raw(NumericLimits<int>::max());
    }

    static constexpr CSSPixels smallest_positive_value()
    {
        return from_raw(1);
    }

    float to_float() const;
    double to_double() const;
    int to_int() const;

    constexpr int raw_value() const { return m_value; }
    constexpr void set_raw_value(int value) { m_value = value; }

    constexpr bool might_be_saturated() const { return raw_value() == NumericLimits<i32>::max() || raw_value() == NumericLimits<i32>::min(); }

    constexpr bool operator==(CSSPixels const& other) const = default;

    explicit operator double() const { return to_double(); }
    explicit operator float() const { return to_float(); }
    explicit operator int() const { return to_int(); }

    constexpr CSSPixels& operator++()
    {
        m_value = Checked<int>::saturating_add(m_value, fixed_point_denominator);
        return *this;
    }
    constexpr CSSPixels& operator--()
    {
        m_value = Checked<int>::saturating_sub(m_value, fixed_point_denominator);
        return *this;
    }

    constexpr int operator<=>(CSSPixels const& other) const
    {
        return raw_value() > other.raw_value()
            ? 1
            : raw_value() < other.raw_value()
            ? -1
            : 0;
    }

    constexpr CSSPixels operator+() const { return from_raw(+raw_value()); }
    constexpr CSSPixels operator-() const { return from_raw(-raw_value()); }

    constexpr CSSPixels operator+(CSSPixels const& other) const
    {
        return from_raw(Checked<int>::saturating_add(raw_value(), other.raw_value()));
    }

    constexpr CSSPixels operator-(CSSPixels const& other) const
    {
        return from_raw(Checked<int>::saturating_sub(raw_value(), other.raw_value()));
    }

    constexpr CSSPixels operator*(CSSPixels const& other) const
    {
        i64 value = raw_value();
        value *= other.raw_value();

        int int_value = AK::clamp_to<int>(value >> fractional_bits);

        // Rounding:
        // If last bit cut off was 1:
        if (value & (1u << (fractional_bits - 1))) {
            // If any bit after was 1 as well
            if (value & (radix_mask >> 1u)) {
                // We need to round away from 0
                int_value = Checked<int>::saturating_add(int_value, 1);
            } else {
                // Otherwise we round to the next even value
                // Which means we add the least significant bit of the raw integer value
                int_value = Checked<int>::saturating_add(int_value, int_value & 1);
            }
        }

        return from_raw(int_value);
    }
    constexpr CSSPixels operator*(CSSPixelFraction const& other) const;

    constexpr CSSPixelFraction operator/(CSSPixels const& other) const;
    constexpr CSSPixels operator/(CSSPixelFraction const& other) const;

    constexpr CSSPixels& operator+=(CSSPixels const& other)
    {
        *this = *this + other;
        return *this;
    }
    constexpr CSSPixels& operator-=(CSSPixels const& other)
    {
        *this = *this - other;
        return *this;
    }
    constexpr CSSPixels& operator*=(CSSPixels const& other)
    {
        *this = *this * other;
        return *this;
    }
    constexpr CSSPixels& operator*=(CSSPixelFraction const& other)
    {
        *this = *this * other;
        return *this;
    }
    constexpr CSSPixels& operator/=(CSSPixels const& other)
    {
        *this = *this * other;
        return *this;
    }

    constexpr CSSPixels abs() const { return from_raw(::abs(m_value)); }

    CSSPixels& scale_by(float value)
    {
        *this = CSSPixels(to_float() * value);
        return *this;
    }

    CSSPixels& scale_by(double value)
    {
        *this = CSSPixels(to_double() * value);
        return *this;
    }

    CSSPixels scaled(float value) const
    {
        auto result = *this;
        result.scale_by(value);
        return result;
    }

    CSSPixels scaled(double value) const
    {
        auto result = *this;
        result.scale_by(value);
        return result;
    }

private:
    i32 m_value { 0 };
};

template<Integral T>
constexpr bool operator==(CSSPixels left, T right) { return left == CSSPixels(right); }
inline bool operator==(CSSPixels left, float right) { return left.to_float() == right; }
inline bool operator==(CSSPixels left, double right) { return left.to_double() == right; }

template<Integral T>
constexpr bool operator>(CSSPixels left, T right) { return left > CSSPixels(right); }
inline bool operator>(CSSPixels left, float right) { return left.to_float() > right; }
inline bool operator>(CSSPixels left, double right) { return left.to_double() > right; }

template<Integral T>
constexpr bool operator<(CSSPixels left, T right) { return left < CSSPixels(right); }
inline bool operator<(CSSPixels left, float right) { return left.to_float() < right; }
inline bool operator<(CSSPixels left, double right) { return left.to_double() < right; }

template<Integral T>
constexpr CSSPixels operator*(CSSPixels left, T right) { return left * CSSPixels(right); }
inline float operator*(CSSPixels left, float right) { return left.to_float() * right; }
inline double operator*(CSSPixels left, double right) { return left.to_double() * right; }

template<Integral T>
constexpr CSSPixels operator*(T left, CSSPixels right) { return CSSPixels(left) * right; }
inline float operator*(float left, CSSPixels right) { return right.to_float() * left; }
inline double operator*(double left, CSSPixels right) { return right.to_double() * left; }

class CSSPixelFraction {
public:
    constexpr CSSPixelFraction(CSSPixels numerator, CSSPixels denominator)
        : m_numerator(numerator)
        , m_denominator(denominator)
    {
        VERIFY(denominator != 0);
    }

    explicit constexpr CSSPixelFraction(CSSPixels value)
        : m_numerator(value)
        , m_denominator(1)
    {
    }

    template<Signed I>
    constexpr CSSPixelFraction(I numerator, I denominator = 1)
        : m_numerator(numerator)
        , m_denominator(denominator)
    {
        VERIFY(denominator != 0);
    }

    template<FloatingPoint F>
    constexpr CSSPixelFraction(F numerator, F denominator = 1)
    {
        if (CSSPixels::nearest_value_for(denominator) == 0) {
            numerator = numerator / denominator;
            denominator = 1;
        }

        m_numerator = CSSPixels(numerator);
        m_denominator = CSSPixels(denominator);

        VERIFY(denominator != 0);
    }

    constexpr operator CSSPixels() const
    {
        i64 wide_value = m_numerator.raw_value();
        wide_value <<= CSSPixels::fractional_bits;
        wide_value /= m_denominator.raw_value();
        return CSSPixels::from_raw(AK::clamp_to<int>(wide_value));
    }

    constexpr CSSPixels operator-(CSSPixels const& other) const
    {
        return CSSPixels(*this) - other;
    }
    constexpr CSSPixels operator+(CSSPixels const& other) const
    {
        return CSSPixels(*this) + other;
    }

    constexpr CSSPixelFraction operator-() const
    {
        return CSSPixelFraction(-numerator(), denominator());
    }

    constexpr int operator<=>(CSSPixelFraction const& other) const
    {
        auto left = static_cast<i64>(m_numerator.raw_value()) * other.m_denominator.raw_value();
        auto right = static_cast<i64>(other.m_numerator.raw_value()) * m_denominator.raw_value();
        if (left > right)
            return 1;
        if (left < right)
            return -1;
        return 0;
    }

    template<Signed I>
    constexpr int operator<=>(I const& other) const
    {
        return *this <=> CSSPixelFraction(other);
    }

    constexpr CSSPixels numerator() const { return m_numerator; }
    constexpr CSSPixels denominator() const { return m_denominator; }

    float to_float() const { return CSSPixels(*this).to_float(); }
    double to_double() const { return CSSPixels(*this).to_double(); }
    int to_int() const { return CSSPixels(*this).to_int(); }
    bool might_be_saturated() const { return CSSPixels(*this).might_be_saturated(); }

private:
    CSSPixels m_numerator;
    CSSPixels m_denominator;
};

constexpr CSSPixels CSSPixels::operator*(CSSPixelFraction const& other) const
{
    i64 wide_value = raw_value();
    wide_value *= other.numerator().raw_value();
    wide_value /= other.denominator().raw_value();
    return CSSPixels::from_raw(AK::clamp_to<int>(wide_value));
}

constexpr CSSPixelFraction CSSPixels::operator/(CSSPixels const& other) const
{
    return CSSPixelFraction(*this, other);
}
constexpr CSSPixels CSSPixels::operator/(CSSPixelFraction const& other) const
{
    i64 wide_value = raw_value();
    wide_value *= other.denominator().raw_value();
    wide_value /= other.numerator().raw_value();
    return CSSPixels::from_raw(AK::clamp_to<int>(wide_value));
}

template<Integral T>
constexpr CSSPixelFraction operator/(CSSPixels left, T right) { return left / CSSPixels(right); }
inline float operator/(CSSPixels left, float right) { return left.to_float() / right; }
inline double operator/(CSSPixels left, double right) { return left.to_double() / right; }

using CSSPixelLine = Gfx::Line<CSSPixels>;
using CSSPixelPoint = Gfx::Point<CSSPixels>;
using CSSPixelRect = Gfx::Rect<CSSPixels>;
using CSSPixelSize = Gfx::Size<CSSPixels>;

using DevicePixelLine = Gfx::Line<DevicePixels>;
using DevicePixelPoint = Gfx::Point<DevicePixels>;
using DevicePixelRect = Gfx::Rect<DevicePixels>;
using DevicePixelSize = Gfx::Size<DevicePixels>;

}

constexpr Web::CSSPixels abs(Web::CSSPixels const& value)
{
    return value.abs();
}

constexpr Web::CSSPixels floor(Web::CSSPixels const& value)
{
    return Web::CSSPixels::from_raw(value.raw_value() & ~Web::CSSPixels::radix_mask);
}

constexpr Web::CSSPixels ceil(Web::CSSPixels const& value)
{
    auto floor_value = value.raw_value() & ~Web::CSSPixels::radix_mask;
    auto ceil_value = floor_value + (value.raw_value() & Web::CSSPixels::radix_mask ? Web::CSSPixels::fixed_point_denominator : 0);
    return Web::CSSPixels::from_raw(ceil_value);
}

constexpr Web::CSSPixels round(Web::CSSPixels const& value)
{
    // FIXME: Maybe do this with bit-fiddling instead
    if (value > 0)
        return floor(value + Web::CSSPixels::from_raw(Web::CSSPixels::fixed_point_denominator >> 1 /* 0.5 */));
    return ceil(value - Web::CSSPixels::from_raw(Web::CSSPixels::fixed_point_denominator >> 1 /* 0.5 */));
}

inline Web::CSSPixels sqrt(Web::CSSPixels const& value)
{
    return Web::CSSPixels::nearest_value_for(AK::sqrt(value.to_float()));
}

constexpr Web::DevicePixels abs(Web::DevicePixels const& value)
{
    return AK::abs(value.value());
}

constexpr Web::CSSPixels square_distance_between(Web::CSSPixelPoint const& a, Web::CSSPixelPoint const& b)
{
    auto delta_x = abs(a.x() - b.x());
    auto delta_y = abs(a.y() - b.y());
    return delta_x * delta_x + delta_y * delta_y;
}

template<>
template<>
[[nodiscard]] ALWAYS_INLINE Web::CSSPixelRect Web::CSSPixelRect::to_rounded<Web::CSSPixels>() const
{
    return {
        round(x()),
        round(y()),
        round(width()),
        round(height()),
    };
}

namespace AK {

template<>
struct Traits<Web::CSSPixels> : public DefaultTraits<Web::CSSPixels> {
    static unsigned hash(Web::CSSPixels const& key)
    {
        return Traits<int>::hash(key.raw_value());
    }

    static bool equals(Web::CSSPixels const& a, Web::CSSPixels const& b)
    {
        return a == b;
    }
};

template<>
struct Traits<Web::DevicePixels> : public DefaultTraits<Web::DevicePixels> {
    static unsigned hash(Web::DevicePixels const& key)
    {
        return Traits<Web::DevicePixels::Type>::hash(key.value());
    }

    static bool equals(Web::DevicePixels const& a, Web::DevicePixels const& b)
    {
        return a == b;
    }
};

template<>
struct Formatter<Web::CSSPixels> : Formatter<double> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSSPixels const& value)
    {
        return Formatter<double>::format(builder, value.to_double());
    }
};

template<>
struct Formatter<Web::DevicePixels> : Formatter<Web::DevicePixels::Type> {
    ErrorOr<void> format(FormatBuilder& builder, Web::DevicePixels const& value)
    {
        return Formatter<Web::DevicePixels::Type>::format(builder, value.value());
    }
};

}

namespace IPC {

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixels const& value);
template<>
ErrorOr<Web::DevicePixels> decode(Decoder& decoder);

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixelPoint const& value);
template<>
ErrorOr<Web::DevicePixelPoint> decode(Decoder& decoder);

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixelSize const& value);
template<>
ErrorOr<Web::DevicePixelSize> decode(Decoder& decoder);

template<>
ErrorOr<void> encode(Encoder& encoder, Web::DevicePixelRect const& value);
template<>
ErrorOr<Web::DevicePixelRect> decode(Decoder& decoder);

}
