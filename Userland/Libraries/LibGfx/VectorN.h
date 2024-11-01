/*
 * Copyright (c) 2020, Stephan Unverwerth <s.unverwerth@serenityos.org>
 * Copyright (c) 2022, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/ByteString.h>
#include <AK/Error.h>
#include <AK/Math.h>
#include <AK/StdLibExtras.h>
#include <AK/StringView.h>

#define LOOP_UNROLL_N 4

#define STRINGIFY_HELPER(x) #x
#define STRINGIFY(x) STRINGIFY_HELPER(x)

#if defined(AK_COMPILER_CLANG)
#    define UNROLL_LOOP _Pragma(STRINGIFY(unroll))
#else
#    define UNROLL_LOOP _Pragma(STRINGIFY(GCC unroll(LOOP_UNROLL_N)))
#endif

namespace Gfx {

template<size_t N, typename T>
requires(N >= 2 && N <= 4) class VectorN final {
    static_assert(LOOP_UNROLL_N >= N, "Unroll the entire loop for performance.");

public:
    [[nodiscard]] constexpr VectorN() = default;
    [[nodiscard]] constexpr VectorN(T x, T y)
    requires(N == 2)
        : m_data { x, y }
    {
    }
    [[nodiscard]] constexpr VectorN(T x, T y, T z)
    requires(N == 3)
        : m_data { x, y, z }
    {
    }
    [[nodiscard]] constexpr VectorN(T x, T y, T z, T w)
    requires(N == 4)
        : m_data { x, y, z, w }
    {
    }

    [[nodiscard]] constexpr T x() const { return m_data[0]; }
    [[nodiscard]] constexpr T y() const { return m_data[1]; }
    [[nodiscard]] constexpr T z() const
    requires(N >= 3)
    {
        return m_data[2];
    }
    [[nodiscard]] constexpr T w() const
    requires(N >= 4)
    {
        return m_data[3];
    }

    constexpr void set_x(T value) { m_data[0] = value; }
    constexpr void set_y(T value) { m_data[1] = value; }
    constexpr void set_z(T value)
    requires(N >= 3)
    {
        m_data[2] = value;
    }
    constexpr void set_w(T value)
    requires(N >= 4)
    {
        m_data[3] = value;
    }

    [[nodiscard]] constexpr T const& operator[](size_t index) const
    {
        VERIFY(index < N);
        return m_data[index];
    }

    [[nodiscard]] constexpr T& operator[](size_t index)
    {
        VERIFY(index < N);
        return m_data[index];
    }

    constexpr VectorN& operator+=(VectorN const& other)
    {
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            m_data[i] += other.data()[i];
        return *this;
    }

    constexpr VectorN& operator-=(VectorN const& other)
    {
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            m_data[i] -= other.data()[i];
        return *this;
    }

    constexpr VectorN& operator*=(T const& t)
    {
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            m_data[i] *= t;
        return *this;
    }

    [[nodiscard]] constexpr VectorN operator+(VectorN const& other) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] + other.data()[i];
        return result;
    }

    [[nodiscard]] constexpr VectorN operator-(VectorN const& other) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] - other.data()[i];
        return result;
    }

    [[nodiscard]] constexpr VectorN operator*(VectorN const& other) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] * other.data()[i];
        return result;
    }

    [[nodiscard]] constexpr VectorN operator-() const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = -m_data[i];
        return result;
    }

    [[nodiscard]] constexpr VectorN operator/(VectorN const& other) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] / other.data()[i];
        return result;
    }

    template<typename U>
    [[nodiscard]] constexpr VectorN operator+(U f) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] + f;
        return result;
    }

    template<typename U>
    [[nodiscard]] constexpr VectorN operator-(U f) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] - f;
        return result;
    }

    template<typename U>
    [[nodiscard]] constexpr VectorN operator*(U f) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] * f;
        return result;
    }

    template<typename U>
    [[nodiscard]] constexpr VectorN operator/(U f) const
    {
        VectorN result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.m_data[i] = m_data[i] / f;
        return result;
    }

    template<ConvertibleTo<T> U>
    constexpr bool operator==(VectorN<N, U> const& other) const
    {
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i) {
            if (m_data[i] != static_cast<T>(other.m_data[i]))
                return false;
        }
        return true;
    }

    [[nodiscard]] constexpr T dot(VectorN const& other) const
    {
        T result {};
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result += m_data[i] * other.data()[i];
        return result;
    }

    [[nodiscard]] constexpr VectorN cross(VectorN const& other) const
    requires(N == 3)
    {
        return VectorN(
            y() * other.z() - z() * other.y(),
            z() * other.x() - x() * other.z(),
            x() * other.y() - y() * other.x());
    }

    [[nodiscard]] constexpr VectorN normalized() const
    {
        VectorN copy { *this };
        copy.normalize();
        return copy;
    }

    [[nodiscard]] constexpr VectorN clamped(T m, T x) const
    {
        VectorN copy { *this };
        copy.clamp(m, x);
        return copy;
    }

    constexpr void clamp(T min_value, T max_value)
    {
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i) {
            m_data[i] = max(min_value, m_data[i]);
            m_data[i] = min(max_value, m_data[i]);
        }
    }

    constexpr void normalize()
    {
        T const inv_length = 1 / length();
        operator*=(inv_length);
    }

    template<typename O = T>
    [[nodiscard]] constexpr O length() const
    {
        return AK::sqrt<O>(dot(*this));
    }

    [[nodiscard]] constexpr VectorN<2, T> xy() const
    requires(N >= 3)
    {
        return VectorN<2, T>(x(), y());
    }

    [[nodiscard]] constexpr VectorN<3, T> xyz() const
    requires(N >= 4)
    {
        return VectorN<3, T>(x(), y(), z());
    }

    [[nodiscard]] ByteString to_byte_string() const
    {
        if constexpr (N == 2)
            return ByteString::formatted("[{},{}]", x(), y());
        else if constexpr (N == 3)
            return ByteString::formatted("[{},{},{}]", x(), y(), z());
        else
            return ByteString::formatted("[{},{},{},{}]", x(), y(), z(), w());
    }

    template<typename U>
    [[nodiscard]] VectorN<N, U> to_type() const
    {
        VectorN<N, U> result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.data()[i] = static_cast<U>(m_data[i]);
        return result;
    }

    template<typename U>
    [[nodiscard]] VectorN<N, U> to_rounded() const
    {
        VectorN<N, U> result;
        UNROLL_LOOP
        for (auto i = 0u; i < N; ++i)
            result.data()[i] = round_to<U>(m_data[i]);
        return result;
    }

    constexpr auto& data() { return m_data; }
    constexpr auto const& data() const { return m_data; }

private:
    Array<T, N> m_data;
};

}

namespace AK {

template<size_t N, typename T>
constexpr Gfx::VectorN<N, T> min(Gfx::VectorN<N, T> const& a, Gfx::VectorN<N, T> const& b)
{
    Gfx::VectorN<N, T> result;
    UNROLL_LOOP
    for (auto i = 0u; i < N; ++i)
        result[i] = min(a[i], b[i]);
    return result;
}

template<size_t N, typename T>
constexpr Gfx::VectorN<N, T> max(Gfx::VectorN<N, T> const& a, Gfx::VectorN<N, T> const& b)
{
    Gfx::VectorN<N, T> result;
    UNROLL_LOOP
    for (auto i = 0u; i < N; ++i)
        result[i] = max(a[i], b[i]);
    return result;
}

}
