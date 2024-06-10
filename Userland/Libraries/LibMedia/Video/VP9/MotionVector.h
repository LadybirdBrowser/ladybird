/*
 * Copyright (c) 2021, Hunter Salyer <thefalsehonesty@gmail.com>
 * Copyright (c) 2022, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Types.h>

namespace Media::Video::VP9 {

struct MotionVector {
public:
    constexpr MotionVector() = default;
    constexpr MotionVector(MotionVector const& other) = default;
    constexpr MotionVector(i32 row, i32 col)
        : m_row(row)
        , m_column(col)
    {
    }

    constexpr MotionVector& operator=(MotionVector const& other) = default;
    constexpr MotionVector& operator=(MotionVector&& other) = default;

    constexpr i32 row() const { return m_row; }
    constexpr void set_row(i32 row) { m_row = row; }
    constexpr i32 column() const { return m_column; }
    constexpr void set_column(i32 col) { m_column = col; }

    constexpr MotionVector operator+(MotionVector const& other) const
    {
        return MotionVector(this->row() + other.row(), this->column() + other.column());
    }
    constexpr MotionVector& operator+=(MotionVector const& other)
    {
        *this = *this + other;
        return *this;
    }

    constexpr MotionVector operator*(i32 scalar) const
    {
        return MotionVector(this->row() * scalar, this->column() * scalar);
    }
    constexpr MotionVector& operator*=(i32 scalar)
    {
        *this = *this * scalar;
        return *this;
    }

    constexpr bool operator==(MotionVector const& other) const
    {
        return this->row() == other.row() && this->column() == other.column();
    }

private:
    i32 m_row { 0 };
    i32 m_column { 0 };
};

}
