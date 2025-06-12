/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Intl/MathematicalValue.h>

namespace JS::Intl {

bool MathematicalValue::is_number() const
{
    return m_value.has<double>();
}

double MathematicalValue::as_number() const
{
    VERIFY(is_number());
    return m_value.get<double>();
}

bool MathematicalValue::is_string() const
{
    return m_value.has<String>();
}

String const& MathematicalValue::as_string() const
{
    VERIFY(is_string());
    return m_value.get<String>();
}

bool MathematicalValue::is_mathematical_value() const
{
    return is_number() || is_string();
}

bool MathematicalValue::is_positive_infinity() const
{
    if (is_mathematical_value())
        return false;
    return m_value.get<Symbol>() == Symbol::PositiveInfinity;
}

bool MathematicalValue::is_negative_infinity() const
{
    if (is_mathematical_value())
        return false;
    return m_value.get<Symbol>() == Symbol::NegativeInfinity;
}

bool MathematicalValue::is_negative_zero() const
{
    if (is_mathematical_value())
        return false;
    return m_value.get<Symbol>() == Symbol::NegativeZero;
}

bool MathematicalValue::is_nan() const
{
    if (is_mathematical_value())
        return false;
    return m_value.get<Symbol>() == Symbol::NotANumber;
}

Unicode::NumberFormat::Value MathematicalValue::to_value() const
{
    return m_value.visit(
        [](double value) -> Unicode::NumberFormat::Value {
            return value;
        },
        [](String const& value) -> Unicode::NumberFormat::Value {
            return value;
        },
        [](auto symbol) -> Unicode::NumberFormat::Value {
            switch (symbol) {
            case Symbol::PositiveInfinity:
                return js_infinity().as_double();
            case Symbol::NegativeInfinity:
                return js_negative_infinity().as_double();
            case Symbol::NegativeZero:
                return -0.0;
            case Symbol::NotANumber:
                return js_nan().as_double();
            }

            VERIFY_NOT_REACHED();
        });
}

MathematicalValue::ValueType MathematicalValue::value_from_number(double number)
{
    Value value(number);

    if (value.is_positive_infinity())
        return Symbol::PositiveInfinity;
    if (value.is_negative_infinity())
        return Symbol::NegativeInfinity;
    if (value.is_negative_zero())
        return Symbol::NegativeZero;
    if (value.is_nan())
        return Symbol::NotANumber;
    return number;
}

}
