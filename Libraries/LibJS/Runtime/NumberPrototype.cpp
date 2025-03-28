/*
 * Copyright (c) 2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2023, Linus Groh <linusg@serenityos.org>
 * Copyright (c) 2022, the SerenityOS developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Array.h>
#include <AK/Function.h>
#include <AK/StringFloatingPointConversions.h>
#include <AK/TypeCasts.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Error.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/NumberFormatConstructor.h>
#include <LibJS/Runtime/NumberObject.h>
#include <LibJS/Runtime/NumberPrototype.h>
#include <math.h>

namespace JS {

GC_DEFINE_ALLOCATOR(NumberPrototype);

static constexpr AK::Array<u8, 37> max_precision_for_radix = {
    // clang-format off
    0,  0,  52, 32, 26, 22, 20, 18, 17, 16,
    15, 15, 14, 14, 13, 13, 13, 12, 12, 12,
    12, 11, 11, 11, 11, 11, 11, 10, 10, 10,
    10, 10, 10, 10, 10, 10, 10,
    // clang-format on
};

static constexpr AK::Array<char, 36> digits = {
    '0', '1', '2', '3', '4', '5', '6', '7', '8', '9',
    'a', 'b', 'c', 'd', 'e', 'f', 'g', 'h', 'i', 'j', 'k', 'l', 'm',
    'n', 'o', 'p', 'q', 'r', 's', 't', 'u', 'v', 'w', 'x', 'y', 'z'
};

NumberPrototype::NumberPrototype(Realm& realm)
    : NumberObject(0, realm.intrinsics().object_prototype())
{
}

void NumberPrototype::initialize(Realm& realm)
{
    auto& vm = this->vm();
    Base::initialize(realm);
    u8 attr = Attribute::Configurable | Attribute::Writable;
    define_native_function(realm, vm.names.toExponential, to_exponential, 1, attr);
    define_native_function(realm, vm.names.toFixed, to_fixed, 1, attr);
    define_native_function(realm, vm.names.toLocaleString, to_locale_string, 0, attr);
    define_native_function(realm, vm.names.toPrecision, to_precision, 1, attr);
    define_native_function(realm, vm.names.toString, to_string, 1, attr);
    define_native_function(realm, vm.names.valueOf, value_of, 0, attr);
}

// thisNumberValue ( value ), https://tc39.es/ecma262/#thisnumbervalue
static ThrowCompletionOr<Value> this_number_value(VM& vm, Value value)
{
    // 1. If Type(value) is Number, return value.
    if (value.is_number())
        return value;

    // 2. If Type(value) is Object and value has a [[NumberData]] internal slot, then
    if (value.is_object() && is<NumberObject>(value.as_object())) {
        // a. Let n be value.[[NumberData]].
        // b. Assert: Type(n) is Number.
        // c. Return n.
        return Value(static_cast<NumberObject&>(value.as_object()).number());
    }

    // 3. Throw a TypeError exception.
    return vm.throw_completion<TypeError>(ErrorType::NotAnObjectOfType, "Number");
}

// 21.1.3.2 Number.prototype.toExponential ( fractionDigits ), https://tc39.es/ecma262/#sec-number.prototype.toexponential
JS_DEFINE_NATIVE_FUNCTION(NumberPrototype::to_exponential)
{
    auto fraction_digits_value = vm.argument(0);

    // 1. Let x be ? thisNumberValue(this value).
    auto number_value = TRY(this_number_value(vm, vm.this_value()));

    // 2. Let f be ? ToIntegerOrInfinity(fractionDigits).
    auto fraction_digits = TRY(fraction_digits_value.to_integer_or_infinity(vm));

    // 3. Assert: If fractionDigits is undefined, then f is 0.
    VERIFY(!fraction_digits_value.is_undefined() || (fraction_digits == 0));

    // 4. If x is not finite, return Number::toString(x).
    if (!number_value.is_finite_number())
        return PrimitiveString::create(vm, MUST(number_value.to_string(vm)));

    // 5. If f < 0 or f > 100, throw a RangeError exception.
    if (fraction_digits < 0 || fraction_digits > 100)
        return vm.throw_completion<RangeError>(ErrorType::InvalidFractionDigits);

    // 6. Set x to ℝ(x).
    auto number = number_value.as_double();

    // 7. Let s be the empty String.
    auto sign = ""sv;

    ByteString number_string;
    int exponent = 0;

    // 8. If x < 0, then
    if (number < 0) {
        // a. Set s to "-".
        sign = "-"sv;

        // b. Set x to -x.
        number = -number;
    }

    // 9. If x = 0, then
    if (number == 0) {
        // a. Let m be the String value consisting of f + 1 occurrences of the code unit 0x0030 (DIGIT ZERO).
        number_string = ByteString::repeated('0', fraction_digits + 1);

        // b. Let e be 0.
        exponent = 0;
    }
    // 10. Else,
    else {
        // a. If fractionDigits is not undefined, then
        //     i. Let e and n be integers such that 10^f ≤ n < 10^(f+1) and for which n × 10^(e-f) - x is as close to zero as possible.
        //        If there are two such sets of e and n, pick the e and n for which n × 10^(e-f) is larger.
        // b. Else,
        //     i. Let e, n, and f be integers such that f ≥ 0, 10^f ≤ n < 10^(f+1), 𝔽(n × 10^(e-f)) is 𝔽(x), and f is as small as possible.
        //        Note that the decimal representation of n has f + 1 digits, n is not divisible by 10, and the least significant digit of n is not necessarily uniquely determined by these criteria.
        exponent = static_cast<int>(floor(log10(number)));

        if (fraction_digits_value.is_undefined()) {
            auto mantissa = convert_floating_point_to_decimal_exponential_form(number).fraction;

            auto mantissa_length = 0;
            for (; mantissa; mantissa /= 10)
                ++mantissa_length;

            fraction_digits = mantissa_length - 1;
        }

        number = round(number / pow(10, exponent - fraction_digits));

        // c. Let m be the String value consisting of the digits of the decimal representation of n (in order, with no leading zeroes).
        number_string = number_to_byte_string(number, NumberToStringMode::WithoutExponent);
    }

    // 11. If f ≠ 0, then
    if (fraction_digits != 0) {
        // a. Let a be the first code unit of m.
        auto first = number_string.substring_view(0, 1);

        // b. Let b be the other f code units of m.
        auto second = number_string.substring_view(1);

        // c. Set m to the string-concatenation of a, ".", and b.
        number_string = ByteString::formatted("{}.{}", first, second);
    }

    char exponent_sign = 0;
    ByteString exponent_string;

    // 12. If e = 0, then
    if (exponent == 0) {
        // a. Let c be "+".
        exponent_sign = '+';

        // b. Let d be "0".
        exponent_string = "0"sv;
    }
    // 13. Else,
    else {
        // a. If e > 0, let c be "+".
        if (exponent > 0) {
            exponent_sign = '+';
        }
        // b. Else,
        else {
            // i. Assert: e < 0.
            VERIFY(exponent < 0);

            // ii. Let c be "-".
            exponent_sign = '-';

            // iii. Set e to -e.
            exponent = -exponent;
        }

        // c. Let d be the String value consisting of the digits of the decimal representation of e (in order, with no leading zeroes).
        exponent_string = ByteString::number(exponent);
    }

    // 14. Set m to the string-concatenation of m, "e", c, and d.
    // 15. Return the string-concatenation of s and m.
    return PrimitiveString::create(vm, ByteString::formatted("{}{}e{}{}", sign, number_string, exponent_sign, exponent_string));
}

// 21.1.3.3 Number.prototype.toFixed ( fractionDigits ), https://tc39.es/ecma262/#sec-number.prototype.tofixed
JS_DEFINE_NATIVE_FUNCTION(NumberPrototype::to_fixed)
{
    // 1. Let x be ? thisNumberValue(this value).
    auto number_value = TRY(this_number_value(vm, vm.this_value()));

    // 2. Let f be ? ToIntegerOrInfinity(fractionDigits).
    // 3. Assert: If fractionDigits is undefined, then f is 0.
    auto fraction_digits = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 4. If f is not finite, throw a RangeError exception.
    if (!Value(fraction_digits).is_finite_number())
        return vm.throw_completion<RangeError>(ErrorType::InvalidFractionDigits);

    // 5. If f < 0 or f > 100, throw a RangeError exception.
    if (fraction_digits < 0 || fraction_digits > 100)
        return vm.throw_completion<RangeError>(ErrorType::InvalidFractionDigits);

    // 6. If x is not finite, return Number::toString(x).
    if (!number_value.is_finite_number())
        return PrimitiveString::create(vm, TRY(number_value.to_string(vm)));

    // 7. Set x to ℝ(x).
    auto number = number_value.as_double();

    // 8. Let s be the empty String.
    // 9. If x < 0, then
    //    a. Set s to "-".
    auto s = (number < 0 ? "-" : "");
    //    b. Set x to -x.
    if (number < 0)
        number = -number;

    // 10. If x ≥ 10^21, then
    //     a. Let m be ! ToString(𝔽(x)).
    if (number >= 1e+21)
        return PrimitiveString::create(vm, MUST(number_value.to_string(vm)));

    // 11. Else,
    //     a. Let n be an integer for which n / (10^f) - x is as close to zero as possible. If there are two such n, pick the larger n.
    //     b. If n = 0, let m be the String "0". Otherwise, let m be the String value consisting of the digits of the decimal representation of n (in order, with no leading zeroes).
    //     c. If f ≠ 0, then
    //         i. Let k be the length of m.
    //         ii. If k ≤ f, then
    //             1. Let z be the String value consisting of f + 1 - k occurrences of the code unit 0x0030 (DIGIT ZERO).
    //             2. Set m to the string-concatenation of z and m.
    //             3. Set k to f + 1.
    //         iii. Let a be the first k - f code units of m.
    //         iv. Let b be the other f code units of m.
    //         v. Set m to the string-concatenation of a, ".", and b.
    // 12. Return the string-concatenation of s and m.

    // NOTE: the above steps are effectively trying to create a formatted string of the
    //       `number` double. Instead of generating a huge, unwieldy `n`, we format
    //       the double using our existing formatting code.

    auto number_format_string = ByteString::formatted("{{}}{{:.{}f}}", fraction_digits);
    return PrimitiveString::create(vm, ByteString::formatted(number_format_string, s, number));
}

// 20.2.1 Number.prototype.toLocaleString ( [ locales [ , options ] ] ), https://tc39.es/ecma402/#sup-number.prototype.tolocalestring
JS_DEFINE_NATIVE_FUNCTION(NumberPrototype::to_locale_string)
{
    auto& realm = *vm.current_realm();

    auto locales = vm.argument(0);
    auto options = vm.argument(1);

    // 1. Let x be ? thisNumberValue(this value).
    auto number_value = TRY(this_number_value(vm, vm.this_value()));

    // 2. Let numberFormat be ? Construct(%NumberFormat%, « locales, options »).
    auto* number_format = static_cast<Intl::NumberFormat*>(TRY(construct(vm, realm.intrinsics().intl_number_format_constructor(), locales, options)).ptr());

    // 3. Return ? FormatNumeric(numberFormat, x).
    auto formatted = Intl::format_numeric(*number_format, number_value);
    return PrimitiveString::create(vm, move(formatted));
}

// 21.1.3.5 Number.prototype.toPrecision ( precision ), https://tc39.es/ecma262/#sec-number.prototype.toprecision
JS_DEFINE_NATIVE_FUNCTION(NumberPrototype::to_precision)
{
    auto precision_value = vm.argument(0);

    // 1. Let x be ? thisNumberValue(this value).
    auto number_value = TRY(this_number_value(vm, vm.this_value()));

    // 2. If precision is undefined, return ! ToString(x).
    if (precision_value.is_undefined())
        return PrimitiveString::create(vm, MUST(number_value.to_string(vm)));

    // 3. Let p be ? ToIntegerOrInfinity(precision).
    auto precision = TRY(precision_value.to_integer_or_infinity(vm));

    // 4. If x is not finite, return Number::toString(x).
    if (!number_value.is_finite_number())
        return PrimitiveString::create(vm, MUST(number_value.to_string(vm)));

    // 5. If p < 1 or p > 100, throw a RangeError exception.
    if ((precision < 1) || (precision > 100))
        return vm.throw_completion<RangeError>(ErrorType::InvalidPrecision);

    // 6. Set x to ℝ(x).
    auto number = number_value.as_double();

    // 7. Let s be the empty String.
    auto sign = ""sv;

    ByteString number_string;
    int exponent = 0;

    // 8. If x < 0, then
    if (number < 0) {
        // a. Set s to the code unit 0x002D (HYPHEN-MINUS).
        sign = "-"sv;

        // b. Set x to -x.
        number = -number;
    }

    // 9. If x = 0, then
    if (number == 0) {
        // a. Let m be the String value consisting of p occurrences of the code unit 0x0030 (DIGIT ZERO).
        number_string = ByteString::repeated('0', precision);

        // b. Let e be 0.
        exponent = 0;
    }
    // 10. Else,
    else {
        // a. Let e and n be integers such that 10^(p-1) ≤ n < 10^p and for which n × 10^(e-p+1) - x is as close to zero as possible.
        //    If there are two such sets of e and n, pick the e and n for which n × 10^(e-p+1) is larger.
        exponent = static_cast<int>(floor(log10(number)));
        number = round(number / pow(10, exponent - precision + 1));

        // b. Let m be the String value consisting of the digits of the decimal representation of n (in order, with no leading zeroes).
        number_string = number_to_byte_string(number, NumberToStringMode::WithoutExponent);

        // c. If e < -6 or e ≥ p, then
        if ((exponent < -6) || (exponent >= precision)) {
            // i. Assert: e ≠ 0.
            VERIFY(exponent != 0);

            // ii. If p ≠ 1, then
            if (precision != 1) {
                // 1. Let a be the first code unit of m.
                auto first = number_string.substring_view(0, 1);

                // 2. Let b be the other p - 1 code units of m.
                auto second = number_string.substring_view(1);

                // 3. Set m to the string-concatenation of a, ".", and b.
                number_string = ByteString::formatted("{}.{}", first, second);
            }

            char exponent_sign = 0;

            // iii. If e > 0, then
            if (exponent > 0) {
                // 1. Let c be the code unit 0x002B (PLUS SIGN).
                exponent_sign = '+';
            }
            // iv. Else,
            else {
                // 1. Assert: e < 0.
                VERIFY(exponent < 0);

                // 2. Let c be the code unit 0x002D (HYPHEN-MINUS).
                exponent_sign = '-';

                // 3. Set e to -e.
                exponent = -exponent;
            }

            // v. Let d be the String value consisting of the digits of the decimal representation of e (in order, with no leading zeroes).
            auto exponent_string = ByteString::number(exponent);

            // vi. Return the string-concatenation of s, m, the code unit 0x0065 (LATIN SMALL LETTER E), c, and d.
            return PrimitiveString::create(vm, ByteString::formatted("{}{}e{}{}", sign, number_string, exponent_sign, exponent_string));
        }
    }

    // 11. If e = p - 1, return the string-concatenation of s and m.
    if (exponent == precision - 1)
        return PrimitiveString::create(vm, ByteString::formatted("{}{}", sign, number_string));

    // 12. If e ≥ 0, then
    if (exponent >= 0) {
        // a. Set m to the string-concatenation of the first e + 1 code units of m, the code unit 0x002E (FULL STOP), and the remaining p - (e + 1) code units of m.
        number_string = ByteString::formatted(
            "{}.{}",
            number_string.substring_view(0, exponent + 1),
            number_string.substring_view(exponent + 1));
    }
    // 13. Else,
    else {
        // a. Set m to the string-concatenation of the code unit 0x0030 (DIGIT ZERO), the code unit 0x002E (FULL STOP), -(e + 1) occurrences of the code unit 0x0030 (DIGIT ZERO), and the String m.
        number_string = ByteString::formatted(
            "0.{}{}",
            ByteString::repeated('0', -1 * (exponent + 1)),
            number_string);
    }

    // 14. Return the string-concatenation of s and m.
    return PrimitiveString::create(vm, ByteString::formatted("{}{}", sign, number_string));
}

// 21.1.3.6 Number.prototype.toString ( [ radix ] ), https://tc39.es/ecma262/#sec-number.prototype.tostring
JS_DEFINE_NATIVE_FUNCTION(NumberPrototype::to_string)
{
    // 1. Let x be ? thisNumberValue(this value).
    auto number_value = TRY(this_number_value(vm, vm.this_value()));

    double radix_mv;

    // 2. If radix is undefined, let radixMV be 10.
    if (vm.argument(0).is_undefined())
        radix_mv = 10;
    // 3. Else, let radixMV be ? ToIntegerOrInfinity(radix).
    else
        radix_mv = TRY(vm.argument(0).to_integer_or_infinity(vm));

    // 4. If radixMV < 2 or radixMV > 36, throw a RangeError exception.
    if (radix_mv < 2 || radix_mv > 36)
        return vm.throw_completion<RangeError>(ErrorType::InvalidRadix);

    // 5. If radixMV = 10, return ! ToString(x).
    if (radix_mv == 10)
        return PrimitiveString::create(vm, MUST(number_value.to_string(vm)));

    // 6. Return the String representation of this Number value using the radix specified by radixMV. Letters a-z are used for digits with values 10 through 35. The precise algorithm is implementation-defined, however the algorithm should be a generalization of that specified in 6.1.6.1.20.
    if (number_value.is_positive_infinity())
        return PrimitiveString::create(vm, "Infinity"_string);
    if (number_value.is_negative_infinity())
        return PrimitiveString::create(vm, "-Infinity"_string);
    if (number_value.is_nan())
        return PrimitiveString::create(vm, "NaN"_string);
    if (number_value.is_positive_zero() || number_value.is_negative_zero())
        return PrimitiveString::create(vm, "0"_string);

    double number = number_value.as_double();
    bool negative = number < 0;
    if (negative)
        number *= -1;

    double int_part = floor(number);
    double decimal_part = number - int_part;

    int radix = (int)radix_mv;
    Vector<char> backwards_characters;

    if (int_part == 0) {
        backwards_characters.append('0');
    } else {
        while (int_part > 0) {
            backwards_characters.append(digits[floor(fmod(int_part, radix))]);
            int_part /= radix;
            int_part = floor(int_part);
        }
    }

    Vector<char> characters;
    if (negative)
        characters.append('-');

    // Reverse characters;
    for (ssize_t i = backwards_characters.size() - 1; i >= 0; --i) {
        characters.append(backwards_characters[i]);
    }

    // decimal part
    if (decimal_part != 0.0) {
        characters.append('.');

        u8 precision = max_precision_for_radix[radix];

        for (u8 i = 0; i < precision; ++i) {
            decimal_part *= radix;
            u64 integral = floor(decimal_part);
            characters.append(digits[integral]);
            decimal_part -= integral;
        }

        while (characters.last() == '0')
            characters.take_last();
    }

    return PrimitiveString::create(vm, ByteString(characters.data(), characters.size()));
}

// 21.1.3.7 Number.prototype.valueOf ( ), https://tc39.es/ecma262/#sec-number.prototype.valueof
JS_DEFINE_NATIVE_FUNCTION(NumberPrototype::value_of)
{
    // 1. Return ? thisNumberValue(this value).
    return this_number_value(vm, vm.this_value());
}

}
