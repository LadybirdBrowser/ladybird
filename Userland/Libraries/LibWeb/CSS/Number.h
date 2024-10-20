/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Types.h>
#include <math.h>

namespace Web::CSS {

class Number {
public:
    enum class Type {
        Number,
        IntegerWithExplicitSign, // This only exists for the nightmarish An+B parsing algorithm
        Integer
    };

    Number()
        : m_value(0)
        , m_type(Type::Number)
    {
    }
    Number(Type type, double value)
        : m_value(value)
        , m_type(type)
    {
    }

    Type type() const { return m_type; }
    double value() const { return m_value; }
    i64 integer_value() const
    {
        // https://www.w3.org/TR/css-values-4/#numeric-types
        // When a value cannot be explicitly supported due to range/precision limitations, it must be converted
        // to the closest value supported by the implementation, but how the implementation defines "closest"
        // is explicitly undefined as well.
        return llround(m_value);
    }
    bool is_integer() const { return m_type == Type::Integer || m_type == Type::IntegerWithExplicitSign; }
    bool is_integer_with_explicit_sign() const { return m_type == Type::IntegerWithExplicitSign; }

    Number operator+(Number const& other) const
    {
        if (is_integer() && other.is_integer())
            return { Type::Integer, m_value + other.m_value };
        return { Type::Number, m_value + other.m_value };
    }

    Number operator-(Number const& other) const
    {
        if (is_integer() && other.is_integer())
            return { Type::Integer, m_value - other.m_value };
        return { Type::Number, m_value - other.m_value };
    }

    Number operator*(Number const& other) const
    {
        if (is_integer() && other.is_integer())
            return { Type::Integer, m_value * other.m_value };
        return { Type::Number, m_value * other.m_value };
    }

    Number operator/(Number const& other) const
    {
        return { Type::Number, m_value / other.m_value };
    }

    String to_string() const
    {
        if (m_type == Type::IntegerWithExplicitSign)
            return MUST(String::formatted("{:+}", m_value));
        return String::number(m_value);
    }

    bool operator==(Number const& other) const
    {
        return m_type == other.m_type && m_value == other.m_value;
    }

    int operator<=>(Number const& other) const
    {
        if (m_value < other.m_value)
            return -1;
        if (m_value > other.m_value)
            return 1;
        return 0;
    }

private:
    double m_value { 0 };
    Type m_type;
};
}

template<>
struct AK::Formatter<Web::CSS::Number> : Formatter<StringView> {
    ErrorOr<void> format(FormatBuilder& builder, Web::CSS::Number const& number)
    {
        return Formatter<StringView>::format(builder, number.to_string());
    }
};
