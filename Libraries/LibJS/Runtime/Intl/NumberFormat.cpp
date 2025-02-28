/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Checked.h>
#include <AK/StringBuilder.h>
#include <AK/Utf8View.h>
#include <LibCrypto/BigInt/SignedBigInteger.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/BigInt.h>
#include <LibJS/Runtime/GlobalObject.h>
#include <LibJS/Runtime/Intl/NumberFormat.h>
#include <LibJS/Runtime/Intl/NumberFormatFunction.h>
#include <LibJS/Runtime/Intl/PluralRules.h>
#include <LibJS/Runtime/ValueInlines.h>
#include <LibUnicode/CurrencyCode.h>
#include <LibUnicode/DisplayNames.h>
#include <math.h>
#include <stdlib.h>

namespace JS::Intl {

GC_DEFINE_ALLOCATOR(NumberFormatBase);
GC_DEFINE_ALLOCATOR(NumberFormat);

NumberFormatBase::NumberFormatBase(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 16 NumberFormat Objects, https://tc39.es/ecma402/#numberformat-objects
NumberFormat::NumberFormat(Object& prototype)
    : NumberFormatBase(prototype)
{
}

void NumberFormat::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (m_bound_format)
        visitor.visit(m_bound_format);
}

StringView NumberFormatBase::computed_rounding_priority_string() const
{
    switch (m_computed_rounding_priority) {
    case ComputedRoundingPriority::Auto:
        return "auto"sv;
    case ComputedRoundingPriority::MorePrecision:
        return "morePrecision"sv;
    case ComputedRoundingPriority::LessPrecision:
        return "lessPrecision"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

Value NumberFormat::use_grouping_to_value(VM& vm) const
{
    switch (m_use_grouping) {
    case Unicode::Grouping::Always:
    case Unicode::Grouping::Auto:
    case Unicode::Grouping::Min2:
        return PrimitiveString::create(vm, Unicode::grouping_to_string(m_use_grouping));
    case Unicode::Grouping::False:
        return Value(false);
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormat::set_use_grouping(StringOrBoolean const& use_grouping)
{
    use_grouping.visit(
        [this](StringView grouping) {
            m_use_grouping = Unicode::grouping_from_string(grouping);
        },
        [this](bool grouping) {
            VERIFY(!grouping);
            m_use_grouping = Unicode::Grouping::False;
        });
}

Unicode::RoundingOptions NumberFormatBase::rounding_options() const
{
    return {
        .type = m_rounding_type,
        .mode = m_rounding_mode,
        .trailing_zero_display = m_trailing_zero_display,
        .min_significant_digits = m_min_significant_digits,
        .max_significant_digits = m_max_significant_digits,
        .min_fraction_digits = m_min_fraction_digits,
        .max_fraction_digits = m_max_fraction_digits,
        .min_integer_digits = m_min_integer_digits,
        .rounding_increment = m_rounding_increment
    };
}

Unicode::DisplayOptions NumberFormat::display_options() const
{
    return {
        .style = m_style,
        .sign_display = m_sign_display,
        .notation = m_notation,
        .compact_display = m_compact_display,
        .grouping = m_use_grouping,
        .currency = m_currency,
        .currency_display = m_currency_display,
        .currency_sign = m_currency_sign,
        .unit = m_unit,
        .unit_display = m_unit_display,
    };
}

// 16.5.1 CurrencyDigits ( currency ), https://tc39.es/ecma402/#sec-currencydigits
int currency_digits(StringView currency)
{
    // 1. If the ISO 4217 currency and funds code list contains currency as an alphabetic code, return the minor
    //    unit value corresponding to the currency from the list; otherwise, return 2.
    if (auto currency_code = Unicode::get_currency_code(currency); currency_code.has_value())
        return currency_code->minor_unit.value_or(2);
    return 2;
}

// 16.5.3 FormatNumericToString ( intlObject, x ), https://tc39.es/ecma402/#sec-formatnumberstring
String format_numeric_to_string(NumberFormatBase const& intl_object, MathematicalValue const& number)
{
    return intl_object.formatter().format_to_decimal(number.to_value());
}

// 16.5.4 PartitionNumberPattern ( numberFormat, x ), https://tc39.es/ecma402/#sec-partitionnumberpattern
Vector<Unicode::NumberFormat::Partition> partition_number_pattern(NumberFormat const& number_format, MathematicalValue const& number)
{
    return number_format.formatter().format_to_parts(number.to_value());
}

// 16.5.6 FormatNumeric ( numberFormat, x ), https://tc39.es/ecma402/#sec-formatnumber
String format_numeric(NumberFormat const& number_format, MathematicalValue const& number)
{
    // 1. Let parts be ? PartitionNumberPattern(numberFormat, x).
    // 2. Let result be the empty String.
    // 3. For each Record { [[Type]], [[Value]] } part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    return number_format.formatter().format(number.to_value());
}

// 16.5.7 FormatNumericToParts ( numberFormat, x ), https://tc39.es/ecma402/#sec-formatnumbertoparts
GC::Ref<Array> format_numeric_to_parts(VM& vm, NumberFormat const& number_format, MathematicalValue const& number)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionNumberPattern(numberFormat, x).
    auto parts = partition_number_pattern(number_format, number);

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    size_t n = 0;

    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (auto& part : parts) {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // e. Increment n by 1.
        ++n;
    }

    // 5. Return result.
    return result;
}

// 16.5.16 ToIntlMathematicalValue ( value ), https://tc39.es/ecma402/#sec-tointlmathematicalvalue
ThrowCompletionOr<MathematicalValue> to_intl_mathematical_value(VM& vm, Value value)
{
    // 1. Let primValue be ? ToPrimitive(value, number).
    auto primitive_value = TRY(value.to_primitive(vm, Value::PreferredType::Number));

    // 2. If Type(primValue) is BigInt, return the mathematical value of primValue.
    if (primitive_value.is_bigint())
        return MUST(value.as_bigint().big_integer().to_base(10));

    // FIXME: The remaining steps are being refactored into a new Runtime Semantic, StringIntlMV.
    //        We short-circuit some of these steps to avoid known pitfalls.
    //        See: https://github.com/tc39/proposal-intl-numberformat-v3/pull/82
    if (!primitive_value.is_string()) {
        auto number = TRY(primitive_value.to_number(vm));
        return number.as_double();
    }

    // 3. If Type(primValue) is String,
    // a.     Let str be primValue.
    auto string = primitive_value.as_string().utf8_string();

    // Step 4 handled separately by the FIXME above.

    // 5. If the grammar cannot interpret str as an expansion of StringNumericLiteral, return not-a-number.
    // 6. Let mv be the MV, a mathematical value, of ? ToNumber(str), as described in 7.1.4.1.1.
    auto mathematical_value = TRY(primitive_value.to_number(vm)).as_double();

    if (Value(mathematical_value).is_nan())
        return MathematicalValue::Symbol::NotANumber;

    // 7. If mv is 0 and the first non white space code point in str is -, return negative-zero.
    if (mathematical_value == 0.0 && string.bytes_as_string_view().trim_whitespace(TrimMode::Left).starts_with('-'))
        return MathematicalValue::Symbol::NegativeZero;

    // 8. If mv is 10^10000 and str contains Infinity, return positive-infinity.
    if (mathematical_value == pow(10, 10000) && string.contains("Infinity"sv))
        return MathematicalValue::Symbol::PositiveInfinity;

    // 9. If mv is -10^10000 and str contains Infinity, return negative-infinity.
    if (mathematical_value == pow(-10, 10000) && string.contains("Infinity"sv))
        return MathematicalValue::Symbol::NegativeInfinity;

    // 10. Return mv.
    return string;
}

// 16.5.19 PartitionNumberRangePattern ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-partitionnumberrangepattern
ThrowCompletionOr<Vector<Unicode::NumberFormat::Partition>> partition_number_range_pattern(VM& vm, NumberFormat const& number_format, MathematicalValue const& start, MathematicalValue const& end)
{
    // 1. If x is NaN or y is NaN, throw a RangeError exception.
    if (start.is_nan())
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "start"sv);
    if (end.is_nan())
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "end"sv);

    return number_format.formatter().format_range_to_parts(start.to_value(), end.to_value());
}

// 16.5.22 FormatNumericRange ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-formatnumericrange
ThrowCompletionOr<String> format_numeric_range(VM& vm, NumberFormat const& number_format, MathematicalValue const& start, MathematicalValue const& end)
{
    // 1. Let parts be ? PartitionNumberRangePattern(numberFormat, x, y).
    {
        // NOTE: We short-circuit PartitionNumberRangePattern as we do not need individual partitions. But we must still
        //       perform the NaN sanity checks from its first step.

        // 1. If x is NaN or y is NaN, throw a RangeError exception.
        if (start.is_nan())
            return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "start"sv);
        if (end.is_nan())
            return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "end"sv);
    }

    // 2. Let result be the empty String.
    // 3. For each part in parts, do
    //     a. Set result to the string-concatenation of result and part.[[Value]].
    // 4. Return result.
    return number_format.formatter().format_range(start.to_value(), end.to_value());
}

// 16.5.23 FormatNumericRangeToParts ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-formatnumericrangetoparts
ThrowCompletionOr<GC::Ref<Array>> format_numeric_range_to_parts(VM& vm, NumberFormat const& number_format, MathematicalValue const& start, MathematicalValue const& end)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionNumberRangePattern(numberFormat, x, y).
    auto parts = TRY(partition_number_range_pattern(vm, number_format, start, end));

    // 2. Let result be ! ArrayCreate(0).
    auto result = MUST(Array::create(realm, 0));

    // 3. Let n be 0.
    size_t n = 0;

    // 4. For each Record { [[Type]], [[Value]] } part in parts, do
    for (auto& part : parts) {
        // a. Let O be OrdinaryObjectCreate(%Object.prototype%).
        auto object = Object::create(realm, realm.intrinsics().object_prototype());

        // b. Perform ! CreateDataPropertyOrThrow(O, "type", part.[[Type]]).
        MUST(object->create_data_property_or_throw(vm.names.type, PrimitiveString::create(vm, part.type)));

        // c. Perform ! CreateDataPropertyOrThrow(O, "value", part.[[Value]]).
        MUST(object->create_data_property_or_throw(vm.names.value, PrimitiveString::create(vm, move(part.value))));

        // d. Perform ! CreateDataPropertyOrThrow(O, "source", part.[[Source]]).
        MUST(object->create_data_property_or_throw(vm.names.source, PrimitiveString::create(vm, part.source)));

        // e. Perform ! CreateDataPropertyOrThrow(result, ! ToString(n), O).
        MUST(result->create_data_property_or_throw(n, object));

        // f. Increment n by 1.
        ++n;
    }

    // 5. Return result.
    return result;
}

}
