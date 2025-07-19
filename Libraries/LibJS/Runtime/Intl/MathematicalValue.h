/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Variant.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/Value.h>
#include <LibUnicode/NumberFormat.h>

namespace JS::Intl {

// https://tc39.es/ecma402/#intl-mathematical-value
class MathematicalValue {
public:
    enum class Symbol {
        PositiveInfinity,
        NegativeInfinity,
        NegativeZero,
        NotANumber,
    };

    MathematicalValue() = default;

    explicit MathematicalValue(double value)
        : m_value(value_from_number(value))
    {
    }

    explicit MathematicalValue(String value)
        : m_value(move(value))
    {
    }

    explicit MathematicalValue(Symbol symbol)
        : m_value(symbol)
    {
    }

    MathematicalValue(Value value)
        : m_value(value.is_number()
                  ? value_from_number(value.as_double())
                  : ValueType(MUST(value.as_bigint().big_integer().to_base(10))))
    {
    }

    bool is_number() const;
    double as_number() const;

    bool is_string() const;
    String const& as_string() const;

    bool is_mathematical_value() const;
    bool is_positive_infinity() const;
    bool is_negative_infinity() const;
    bool is_negative_zero() const;
    bool is_nan() const;

    Unicode::NumberFormat::Value to_value() const;

private:
    using ValueType = Variant<double, String, Symbol>;

    static ValueType value_from_number(double number);

    ValueType m_value { 0.0 };
};

}
