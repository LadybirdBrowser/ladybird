/*
 * Copyright (c) 2020, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>
#include <initializer_list>

namespace Gfx {

template<size_t N, typename T>
class Matrix {
    template<size_t U, typename V>
    friend class Matrix;

public:
    static constexpr size_t Size = N;

    constexpr Matrix() = default;
    constexpr Matrix(std::initializer_list<T> elements)
    {
        VERIFY(elements.size() == N * N);
        size_t i = 0;
        for (auto& element : elements) {
            m_elements[i / N][i % N] = element;
            ++i;
        }
    }

    template<typename... Args>
    constexpr Matrix(Args... args)
        : Matrix({ (T)args... })
    {
    }

    constexpr Matrix(Matrix const& other)
    {
        *this = other;
    }

    constexpr Matrix& operator=(Matrix const& other)
    {
#ifndef __clang__
        if (is_constant_evaluated()) {
            for (size_t i = 0; i < N; i++) {
                for (size_t j = 0; j < N; j++) {
                    m_elements[i][j] = other.elements()[i][j];
                }
            }
            return *this;
        }
#endif
        __builtin_memcpy(m_elements, other.elements(), sizeof(T) * N * N);
        return *this;
    }

    constexpr auto elements() const { return m_elements; }
    constexpr auto elements() { return m_elements; }

    // FIXME: Change to multi-arg operator[] once we upgrade to C++23
    constexpr auto const& operator()(size_t row, size_t col) const { return m_elements[row][col]; }
    constexpr auto& operator()(size_t row, size_t col) { return m_elements[row][col]; }

    [[nodiscard]] constexpr Matrix operator*(Matrix const& other) const
    {
        Matrix product;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j) {
                auto& element = product.m_elements[i][j];

                if constexpr (N == 4) {
                    element = m_elements[i][0] * other.m_elements[0][j]
                        + m_elements[i][1] * other.m_elements[1][j]
                        + m_elements[i][2] * other.m_elements[2][j]
                        + m_elements[i][3] * other.m_elements[3][j];
                } else if constexpr (N == 3) {
                    element = m_elements[i][0] * other.m_elements[0][j]
                        + m_elements[i][1] * other.m_elements[1][j]
                        + m_elements[i][2] * other.m_elements[2][j];
                } else if constexpr (N == 2) {
                    element = m_elements[i][0] * other.m_elements[0][j]
                        + m_elements[i][1] * other.m_elements[1][j];
                } else if constexpr (N == 1) {
                    element = m_elements[i][0] * other.m_elements[0][j];
                } else {
                    T value {};
                    for (size_t k = 0; k < N; ++k)
                        value += m_elements[i][k] * other.m_elements[k][j];

                    element = value;
                }
            }
        }

        return product;
    }

    [[nodiscard]] constexpr Matrix operator+(Matrix const& other) const
    {
        Matrix sum;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j)
                sum.m_elements[i][j] = m_elements[i][j] + other.m_elements[i][j];
        }
        return sum;
    }

    [[nodiscard]] constexpr Matrix operator/(T divisor) const
    {
        Matrix division;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j)
                division.m_elements[i][j] = m_elements[i][j] / divisor;
        }
        return division;
    }

    [[nodiscard]] friend constexpr Matrix operator*(Matrix const& matrix, T scalar)
    {
        Matrix scaled;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j)
                scaled.m_elements[i][j] = matrix.m_elements[i][j] * scalar;
        }
        return scaled;
    }

    [[nodiscard]] friend constexpr Matrix operator*(T scalar, Matrix const& matrix)
    {
        return matrix * scalar;
    }

    [[nodiscard]] constexpr Matrix adjugate() const
    {
        if constexpr (N == 1)
            return Matrix(1);

        Matrix adjugate;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j) {
                int sign = (i + j) % 2 == 0 ? 1 : -1;
                adjugate.m_elements[j][i] = sign * first_minor(i, j);
            }
        }
        return adjugate;
    }

    [[nodiscard]] constexpr T determinant() const
    {
        if constexpr (N == 1) {
            return m_elements[0][0];
        } else {
            T result = {};
            int sign = 1;
            for (size_t j = 0; j < N; ++j) {
                result += sign * m_elements[0][j] * first_minor(0, j);
                sign *= -1;
            }
            return result;
        }
    }

    [[nodiscard]] constexpr T first_minor(size_t skip_row, size_t skip_column) const
    {
        static_assert(N > 1);
        VERIFY(skip_row < N);
        VERIFY(skip_column < N);

        Matrix<N - 1, T> first_minor;
        constexpr auto new_size = N - 1;
        size_t k = 0;

        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j) {
                if (i == skip_row || j == skip_column)
                    continue;

                first_minor.elements()[k / new_size][k % new_size] = m_elements[i][j];
                ++k;
            }
        }

        return first_minor.determinant();
    }

    [[nodiscard]] constexpr static Matrix identity()
    {
        Matrix result;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j) {
                if (i == j)
                    result.m_elements[i][j] = 1;
                else
                    result.m_elements[i][j] = 0;
            }
        }
        return result;
    }

    [[nodiscard]] constexpr Matrix inverse() const
    {
        return adjugate() / determinant();
    }

    [[nodiscard]] constexpr Matrix transpose() const
    {
        Matrix result;
        for (size_t i = 0; i < N; ++i) {
            for (size_t j = 0; j < N; ++j)
                result.m_elements[i][j] = m_elements[j][i];
        }
        return result;
    }

    template<size_t U>
    [[nodiscard]] constexpr Matrix<U, T> submatrix_from_topleft() const
    requires(U > 0 && U < N)
    {
        Matrix<U, T> result;
        for (size_t i = 0; i < U; ++i) {
            for (size_t j = 0; j < U; ++j)
                result.m_elements[i][j] = m_elements[i][j];
        }
        return result;
    }

    constexpr bool is_invertible() const
    {
        return determinant() != static_cast<T>(0.0);
    }

private:
    T m_elements[N][N];
};

}

using Gfx::Matrix;
