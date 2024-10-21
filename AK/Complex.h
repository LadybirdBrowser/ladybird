/*
 * Copyright (c) 2021, Cesar Torres <shortanemoia@protonmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Concepts.h>
#include <AK/Math.h>

namespace AK {

template<AK::Concepts::Arithmetic T>
class [[gnu::packed]] Complex {
public:
    constexpr Complex()
        : m_real(0)
        , m_imag(0)
    {
    }

    constexpr Complex(T real)
        : m_real(real)
        , m_imag((T)0)
    {
    }

    constexpr Complex(T real, T imaginary)
        : m_real(real)
        , m_imag(imaginary)
    {
    }

    constexpr T real() const noexcept { return m_real; }

    constexpr T imag() const noexcept { return m_imag; }

    constexpr T magnitude_squared() const noexcept { return m_real * m_real + m_imag * m_imag; }

    constexpr T magnitude() const noexcept
    {
        return hypot(m_real, m_imag);
    }

    constexpr T phase() const noexcept
    {
        return atan2(m_imag, m_real);
    }

    template<AK::Concepts::Arithmetic U, AK::Concepts::Arithmetic V>
    static constexpr Complex<T> from_polar(U magnitude, V phase)
    {
        V s, c;
        sincos(phase, s, c);
        return Complex<T>(magnitude * c, magnitude * s);
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T>& operator=(Complex<U> const& other)
    {
        m_real = other.real();
        m_imag = other.imag();
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T>& operator=(U const& x)
    {
        m_real = x;
        m_imag = 0;
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator+=(Complex<U> const& x)
    {
        m_real += x.real();
        m_imag += x.imag();
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator+=(U const& x)
    {
        m_real += x;
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator-=(Complex<U> const& x)
    {
        m_real -= x.real();
        m_imag -= x.imag();
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator-=(U const& x)
    {
        m_real -= x;
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator*=(Complex<U> const& x)
    {
        T const real = m_real;
        m_real = real * x.real() - m_imag * x.imag();
        m_imag = real * x.imag() + m_imag * x.real();
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator*=(U const& x)
    {
        m_real *= x;
        m_imag *= x;
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator/=(Complex<U> const& x)
    {
        T const real = m_real;
        T const divisor = x.real() * x.real() + x.imag() * x.imag();
        m_real = (real * x.real() + m_imag * x.imag()) / divisor;
        m_imag = (m_imag * x.real() - x.real() * x.imag()) / divisor;
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator/=(U const& x)
    {
        m_real /= x;
        m_imag /= x;
        return *this;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator+(Complex<U> const& a)
    {
        Complex<T> x = *this;
        x += a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator+(U const& a)
    {
        Complex<T> x = *this;
        x += a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator-(Complex<U> const& a)
    {
        Complex<T> x = *this;
        x -= a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator-(U const& a)
    {
        Complex<T> x = *this;
        x -= a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator*(Complex<U> const& a)
    {
        Complex<T> x = *this;
        x *= a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator*(U const& a)
    {
        Complex<T> x = *this;
        x *= a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator/(Complex<U> const& a)
    {
        Complex<T> x = *this;
        x /= a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr Complex<T> operator/(U const& a)
    {
        Complex<T> x = *this;
        x /= a;
        return x;
    }

    template<AK::Concepts::Arithmetic U>
    constexpr bool operator==(Complex<U> const& a) const
    {
        return (this->real() == a.real()) && (this->imag() == a.imag());
    }

    constexpr Complex<T> operator+()
    {
        return *this;
    }

    constexpr Complex<T> operator-()
    {
        return Complex<T>(-m_real, -m_imag);
    }

private:
    T m_real;
    T m_imag;
};

// reverse associativity operators for scalars
template<AK::Concepts::Arithmetic T, AK::Concepts::Arithmetic U>
constexpr Complex<T> operator+(U const& a, Complex<T> const& b)
{
    Complex<T> x = a;
    x += b;
    return x;
}

template<AK::Concepts::Arithmetic T, AK::Concepts::Arithmetic U>
constexpr Complex<T> operator-(U const& a, Complex<T> const& b)
{
    Complex<T> x = a;
    x -= b;
    return x;
}

template<AK::Concepts::Arithmetic T, AK::Concepts::Arithmetic U>
constexpr Complex<T> operator*(U const& a, Complex<T> const& b)
{
    Complex<T> x = a;
    x *= b;
    return x;
}

template<AK::Concepts::Arithmetic T, AK::Concepts::Arithmetic U>
constexpr Complex<T> operator/(U const& a, Complex<T> const& b)
{
    Complex<T> x = a;
    x /= b;
    return x;
}

// some identities
template<AK::Concepts::Arithmetic T>
static constinit Complex<T> complex_real_unit = Complex<T>((T)1, (T)0);
template<AK::Concepts::Arithmetic T>
static constinit Complex<T> complex_imag_unit = Complex<T>((T)0, (T)1);

template<AK::Concepts::Arithmetic T, AK::Concepts::Arithmetic U>
static constexpr bool approx_eq(Complex<T> const& a, Complex<U> const& b, double const margin = 0.000001)
{
    auto const x = const_cast<Complex<T>&>(a) - const_cast<Complex<U>&>(b);
    return x.magnitude() <= margin;
}

// complex version of exp()
template<AK::Concepts::Arithmetic T>
static constexpr Complex<T> cexp(Complex<T> const& a)
{
    // FIXME: this can probably be faster and not use so many "expensive" trigonometric functions
    return exp(a.real()) * Complex<T>(cos(a.imag()), sin(a.imag()));
}
}

template<AK::Concepts::Arithmetic T>
struct AK::Formatter<AK::Complex<T>> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, AK::Complex<T> c)
    {
        return Formatter<StringView>::format(builder, TRY(String::formatted("{}{:+}i", c.real(), c.imag())));
    }
};

#if USING_AK_GLOBALLY
using AK::approx_eq;
using AK::cexp;
using AK::Complex;
using AK::complex_imag_unit;
using AK::complex_real_unit;
#endif
