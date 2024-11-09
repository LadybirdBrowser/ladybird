/*
 * Copyright (c) 2022, Lucas Chollet <lucas.chollet@free.fr>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BigFraction.h"
#include <AK/ByteString.h>
#include <AK/Math.h>
#include <AK/StringBuilder.h>
#include <LibCrypto/NumberTheory/ModularFunctions.h>

namespace Crypto {

BigFraction::BigFraction(SignedBigInteger numerator, UnsignedBigInteger denominator)
    : m_numerator(move(numerator))
    , m_denominator(move(denominator))
{
    VERIFY(m_denominator != 0);
    reduce();
}

BigFraction::BigFraction(SignedBigInteger value)
    : BigFraction(move(value), 1)
{
}

ErrorOr<BigFraction> BigFraction::from_string(StringView sv)
{
    auto maybe_dot_index = sv.find('.');

    auto integer_part_view = sv.substring_view(0, maybe_dot_index.value_or(sv.length()));
    auto fraction_part_view = maybe_dot_index.has_value() ? sv.substring_view(1 + *maybe_dot_index) : "0"sv;

    auto integer_part = TRY(SignedBigInteger::from_base(10, integer_part_view));
    auto fractional_part = TRY(SignedBigInteger::from_base(10, fraction_part_view));
    auto fraction_length = UnsignedBigInteger(static_cast<u64>(fraction_part_view.length()));

    if (!sv.is_empty() && sv[0] == '-')
        fractional_part.negate();

    return BigFraction(move(integer_part)) + BigFraction(move(fractional_part), NumberTheory::Power("10"_bigint, move(fraction_length)));
}

BigFraction BigFraction::operator+(BigFraction const& rhs) const
{
    if (rhs.m_numerator == "0"_bigint)
        return *this;

    auto result = *this;
    result.m_numerator.set_to(m_numerator.multiplied_by(rhs.m_denominator).plus(rhs.m_numerator.multiplied_by(m_denominator)));
    result.m_denominator.set_to(m_denominator.multiplied_by(rhs.m_denominator));

    result.reduce();

    return result;
}

BigFraction BigFraction::operator-(BigFraction const& rhs) const
{
    return *this + (-rhs);
}

BigFraction BigFraction::operator*(BigFraction const& rhs) const
{
    auto result = *this;
    result.m_numerator.set_to(result.m_numerator.multiplied_by(rhs.m_numerator));
    result.m_denominator.set_to(result.m_denominator.multiplied_by(rhs.m_denominator));

    result.reduce();

    return result;
}

BigFraction BigFraction::operator-() const
{
    return { m_numerator.negated_value(), m_denominator };
}

BigFraction BigFraction::invert() const
{
    return BigFraction { 1 } / *this;
}

BigFraction BigFraction::operator/(BigFraction const& rhs) const
{
    VERIFY(rhs.m_numerator != "0"_bigint);

    auto result = *this;
    result.m_numerator.set_to(m_numerator.multiplied_by(rhs.m_denominator));
    result.m_denominator.set_to(m_denominator.multiplied_by(rhs.m_numerator.unsigned_value()));

    if (rhs.m_numerator.is_negative())
        result.m_numerator.negate();

    result.reduce();

    return result;
}

bool BigFraction::operator<(BigFraction const& rhs) const
{
    return (*this - rhs).m_numerator.is_negative();
}

bool BigFraction::operator==(BigFraction const& rhs) const
{
    return m_numerator == rhs.m_numerator && m_denominator == rhs.m_denominator;
}

BigFraction::BigFraction(double d)
{
    bool negative = false;
    if (d < 0) {
        negative = true;
        d = -d;
    }
    i8 current_pow = 0;
    while (AK::pow(10.0, (double)current_pow) <= d)
        current_pow += 1;
    current_pow -= 1;
    unsigned decimal_places = 0;
    while (d >= NumericLimits<double>::epsilon() || current_pow >= 0) {
        m_numerator.set_to(m_numerator.multiplied_by(SignedBigInteger { 10 }));
        i8 digit = (u64)(d * AK::pow(0.1, (double)current_pow)) % 10;
        m_numerator.set_to(m_numerator.plus(UnsignedBigInteger { digit }));
        d -= digit * AK::pow(10.0, (double)current_pow);
        if (current_pow < 0) {
            ++decimal_places;
            m_denominator.set_to(NumberTheory::Power("10"_bigint, UnsignedBigInteger { decimal_places }));
        }
        current_pow -= 1;
    }
    m_numerator.set_to(negative ? (m_numerator.negated_value()) : m_numerator);
}

double BigFraction::to_double() const
{
    // FIXME: very naive implementation
    return m_numerator.to_double() / m_denominator.to_double();
}

void BigFraction::set_to_0()
{
    m_numerator.set_to_0();
    m_denominator.set_to(1);
}

BigFraction BigFraction::rounded(unsigned rounding_threshold) const
{
    auto const get_last_digit = [](auto const& integer) {
        return integer.divided_by("10"_bigint).remainder;
    };

    auto res = m_numerator.divided_by(m_denominator);
    BigFraction result { move(res.quotient) };

    auto const needed_power = NumberTheory::Power("10"_bigint, UnsignedBigInteger { rounding_threshold });
    // We get one more digit to do proper rounding
    auto const fractional_value = res.remainder.multiplied_by(needed_power.multiplied_by("10"_bigint)).divided_by(m_denominator).quotient;

    result.m_numerator.set_to(result.m_numerator.multiplied_by(needed_power));
    result.m_numerator.set_to(result.m_numerator.plus(fractional_value.divided_by("10"_bigint).quotient));
    if (get_last_digit(fractional_value) > "4"_bigint)
        result.m_numerator.set_to(result.m_numerator.plus("1"_bigint));

    result.m_denominator.set_to(result.m_denominator.multiplied_by(needed_power));

    return result;
}

void BigFraction::reduce()
{
    auto const gcd = NumberTheory::GCD(m_numerator.unsigned_value(), m_denominator);

    if (gcd == 1)
        return;

    auto const numerator_divide = m_numerator.divided_by(gcd);
    VERIFY(numerator_divide.remainder == "0"_bigint);
    m_numerator = numerator_divide.quotient;

    auto const denominator_divide = m_denominator.divided_by(gcd);
    VERIFY(denominator_divide.remainder == "0"_bigint);
    m_denominator = denominator_divide.quotient;
}

ByteString BigFraction::to_byte_string(unsigned rounding_threshold) const
{
    StringBuilder builder;
    if (m_numerator.is_negative() && m_numerator != "0"_bigint)
        builder.append('-');

    auto const number_of_digits = [](auto integer) {
        unsigned size = 1;
        for (auto division_result = integer.divided_by(UnsignedBigInteger { 10 });
             division_result.remainder == UnsignedBigInteger { 0 } && division_result.quotient != UnsignedBigInteger { 0 };
             division_result = division_result.quotient.divided_by(UnsignedBigInteger { 10 })) {
            ++size;
        }
        return size;
    };

    auto const rounded_fraction = rounded(rounding_threshold);

    // We take the unsigned value as we already manage the '-'
    auto const full_value = rounded_fraction.m_numerator.unsigned_value().to_base_deprecated(10);
    int split = full_value.length() - (number_of_digits(rounded_fraction.m_denominator) - 1);

    if (split < 0)
        split = 0;

    auto const remove_trailing_zeros = [](StringView value) -> StringView {
        auto n = value.length();
        VERIFY(n > 0);
        while (n > 0 && value.characters_without_null_termination()[n - 1] == '0')
            --n;
        return { value.characters_without_null_termination(), n };
    };

    auto const raw_fractional_value = full_value.substring(split, full_value.length() - split);

    auto const integer_value = split == 0 ? "0"sv : full_value.substring_view(0, split);
    auto const fractional_value = rounding_threshold == 0 ? "0"sv : remove_trailing_zeros(raw_fractional_value);

    builder.append(integer_value);

    bool const has_decimal_part = fractional_value.length() > 0 && fractional_value != "0";

    if (has_decimal_part) {
        builder.append('.');

        auto number_pre_zeros = number_of_digits(rounded_fraction.m_denominator) - full_value.length() - 1;
        if (number_pre_zeros > rounding_threshold || fractional_value == "0")
            number_pre_zeros = 0;

        builder.append_repeated('0', number_pre_zeros);

        if (fractional_value != "0")
            builder.append(fractional_value);
    }

    return builder.to_byte_string();
}

BigFraction BigFraction::sqrt() const
{
    // FIXME: very naive implementation
    return BigFraction { AK::sqrt(to_double()) };
}

}
