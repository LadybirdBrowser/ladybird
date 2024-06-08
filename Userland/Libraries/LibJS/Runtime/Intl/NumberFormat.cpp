/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
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
#include <LibLocale/DisplayNames.h>
#include <LibUnicode/CurrencyCode.h>
#include <math.h>
#include <stdlib.h>

namespace JS::Intl {

JS_DEFINE_ALLOCATOR(NumberFormatBase);
JS_DEFINE_ALLOCATOR(NumberFormat);

NumberFormatBase::NumberFormatBase(Object& prototype)
    : Object(ConstructWithPrototypeTag::Tag, prototype)
{
}

// 15 NumberFormat Objects, https://tc39.es/ecma402/#numberformat-objects
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

void NumberFormat::set_style(StringView style)
{
    if (style == "decimal"sv)
        m_style = Style::Decimal;
    else if (style == "percent"sv)
        m_style = Style::Percent;
    else if (style == "currency"sv)
        m_style = Style::Currency;
    else if (style == "unit"sv)
        m_style = Style::Unit;
    else
        VERIFY_NOT_REACHED();
}

StringView NumberFormat::style_string() const
{
    switch (m_style) {
    case Style::Decimal:
        return "decimal"sv;
    case Style::Percent:
        return "percent"sv;
    case Style::Currency:
        return "currency"sv;
    case Style::Unit:
        return "unit"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormat::set_currency_display(StringView currency_display)
{
    m_resolved_currency_display.clear();

    if (currency_display == "code"sv)
        m_currency_display = CurrencyDisplay::Code;
    else if (currency_display == "symbol"sv)
        m_currency_display = CurrencyDisplay::Symbol;
    else if (currency_display == "narrowSymbol"sv)
        m_currency_display = CurrencyDisplay::NarrowSymbol;
    else if (currency_display == "name"sv)
        m_currency_display = CurrencyDisplay::Name;
    else
        VERIFY_NOT_REACHED();
}

StringView NumberFormat::resolve_currency_display()
{
    if (m_resolved_currency_display.has_value())
        return *m_resolved_currency_display;

    switch (currency_display()) {
    case NumberFormat::CurrencyDisplay::Code:
        m_resolved_currency_display = currency();
        break;
    case NumberFormat::CurrencyDisplay::Symbol:
        m_resolved_currency_display = ::Locale::currency_display_name(data_locale(), currency(), ::Locale::Style::Short);
        break;
    case NumberFormat::CurrencyDisplay::NarrowSymbol:
        m_resolved_currency_display = ::Locale::currency_display_name(data_locale(), currency(), ::Locale::Style::Narrow);
        break;
    case NumberFormat::CurrencyDisplay::Name:
        m_resolved_currency_display = ::Locale::currency_numeric_display_name(data_locale(), currency());
        break;
    default:
        VERIFY_NOT_REACHED();
    }

    if (!m_resolved_currency_display.has_value())
        m_resolved_currency_display = currency();

    return *m_resolved_currency_display;
}

StringView NumberFormat::currency_display_string() const
{
    VERIFY(m_currency_display.has_value());

    switch (*m_currency_display) {
    case CurrencyDisplay::Code:
        return "code"sv;
    case CurrencyDisplay::Symbol:
        return "symbol"sv;
    case CurrencyDisplay::NarrowSymbol:
        return "narrowSymbol"sv;
    case CurrencyDisplay::Name:
        return "name"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormat::set_currency_sign(StringView currency_sign)
{
    if (currency_sign == "standard"sv)
        m_currency_sign = CurrencySign::Standard;
    else if (currency_sign == "accounting"sv)
        m_currency_sign = CurrencySign::Accounting;
    else
        VERIFY_NOT_REACHED();
}

StringView NumberFormat::currency_sign_string() const
{
    VERIFY(m_currency_sign.has_value());

    switch (*m_currency_sign) {
    case CurrencySign::Standard:
        return "standard"sv;
    case CurrencySign::Accounting:
        return "accounting"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

StringView NumberFormatBase::rounding_type_string() const
{
    switch (m_rounding_type) {
    case RoundingType::SignificantDigits:
        return "significantDigits"sv;
    case RoundingType::FractionDigits:
        return "fractionDigits"sv;
    case RoundingType::MorePrecision:
        return "morePrecision"sv;
    case RoundingType::LessPrecision:
        return "lessPrecision"sv;
    default:
        VERIFY_NOT_REACHED();
    }
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

StringView NumberFormatBase::rounding_mode_string() const
{
    switch (m_rounding_mode) {
    case RoundingMode::Ceil:
        return "ceil"sv;
    case RoundingMode::Expand:
        return "expand"sv;
    case RoundingMode::Floor:
        return "floor"sv;
    case RoundingMode::HalfCeil:
        return "halfCeil"sv;
    case RoundingMode::HalfEven:
        return "halfEven"sv;
    case RoundingMode::HalfExpand:
        return "halfExpand"sv;
    case RoundingMode::HalfFloor:
        return "halfFloor"sv;
    case RoundingMode::HalfTrunc:
        return "halfTrunc"sv;
    case RoundingMode::Trunc:
        return "trunc"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormatBase::set_rounding_mode(StringView rounding_mode)
{
    if (rounding_mode == "ceil"sv)
        m_rounding_mode = RoundingMode::Ceil;
    else if (rounding_mode == "expand"sv)
        m_rounding_mode = RoundingMode::Expand;
    else if (rounding_mode == "floor"sv)
        m_rounding_mode = RoundingMode::Floor;
    else if (rounding_mode == "halfCeil"sv)
        m_rounding_mode = RoundingMode::HalfCeil;
    else if (rounding_mode == "halfEven"sv)
        m_rounding_mode = RoundingMode::HalfEven;
    else if (rounding_mode == "halfExpand"sv)
        m_rounding_mode = RoundingMode::HalfExpand;
    else if (rounding_mode == "halfFloor"sv)
        m_rounding_mode = RoundingMode::HalfFloor;
    else if (rounding_mode == "halfTrunc"sv)
        m_rounding_mode = RoundingMode::HalfTrunc;
    else if (rounding_mode == "trunc"sv)
        m_rounding_mode = RoundingMode::Trunc;
    else
        VERIFY_NOT_REACHED();
}

StringView NumberFormatBase::trailing_zero_display_string() const
{
    switch (m_trailing_zero_display) {
    case TrailingZeroDisplay::Auto:
        return "auto"sv;
    case TrailingZeroDisplay::StripIfInteger:
        return "stripIfInteger"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormatBase::set_trailing_zero_display(StringView trailing_zero_display)
{
    if (trailing_zero_display == "auto"sv)
        m_trailing_zero_display = TrailingZeroDisplay::Auto;
    else if (trailing_zero_display == "stripIfInteger"sv)
        m_trailing_zero_display = TrailingZeroDisplay::StripIfInteger;
    else
        VERIFY_NOT_REACHED();
}

Value NumberFormat::use_grouping_to_value(VM& vm) const
{
    switch (m_use_grouping) {
    case UseGrouping::Always:
        return PrimitiveString::create(vm, "always"_string);
    case UseGrouping::Auto:
        return PrimitiveString::create(vm, "auto"_string);
    case UseGrouping::Min2:
        return PrimitiveString::create(vm, "min2"_string);
    case UseGrouping::False:
        return Value(false);
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormat::set_use_grouping(StringOrBoolean const& use_grouping)
{
    use_grouping.visit(
        [this](StringView grouping) {
            if (grouping == "always"sv)
                m_use_grouping = UseGrouping::Always;
            else if (grouping == "auto"sv)
                m_use_grouping = UseGrouping::Auto;
            else if (grouping == "min2"sv)
                m_use_grouping = UseGrouping::Min2;
            else
                VERIFY_NOT_REACHED();
        },
        [this](bool grouping) {
            VERIFY(!grouping);
            m_use_grouping = UseGrouping::False;
        });
}

void NumberFormat::set_notation(StringView notation)
{
    if (notation == "standard"sv)
        m_notation = Notation::Standard;
    else if (notation == "scientific"sv)
        m_notation = Notation::Scientific;
    else if (notation == "engineering"sv)
        m_notation = Notation::Engineering;
    else if (notation == "compact"sv)
        m_notation = Notation::Compact;
    else
        VERIFY_NOT_REACHED();
}

StringView NumberFormat::notation_string() const
{
    switch (m_notation) {
    case Notation::Standard:
        return "standard"sv;
    case Notation::Scientific:
        return "scientific"sv;
    case Notation::Engineering:
        return "engineering"sv;
    case Notation::Compact:
        return "compact"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormat::set_compact_display(StringView compact_display)
{
    if (compact_display == "short"sv)
        m_compact_display = CompactDisplay::Short;
    else if (compact_display == "long"sv)
        m_compact_display = CompactDisplay::Long;
    else
        VERIFY_NOT_REACHED();
}

StringView NumberFormat::compact_display_string() const
{
    VERIFY(m_compact_display.has_value());

    switch (*m_compact_display) {
    case CompactDisplay::Short:
        return "short"sv;
    case CompactDisplay::Long:
        return "long"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

void NumberFormat::set_sign_display(StringView sign_display)
{
    if (sign_display == "auto"sv)
        m_sign_display = SignDisplay::Auto;
    else if (sign_display == "never"sv)
        m_sign_display = SignDisplay::Never;
    else if (sign_display == "always"sv)
        m_sign_display = SignDisplay::Always;
    else if (sign_display == "exceptZero"sv)
        m_sign_display = SignDisplay::ExceptZero;
    else if (sign_display == "negative"sv)
        m_sign_display = SignDisplay::Negative;
    else
        VERIFY_NOT_REACHED();
}

StringView NumberFormat::sign_display_string() const
{
    switch (m_sign_display) {
    case SignDisplay::Auto:
        return "auto"sv;
    case SignDisplay::Never:
        return "never"sv;
    case SignDisplay::Always:
        return "always"sv;
    case SignDisplay::ExceptZero:
        return "exceptZero"sv;
    case SignDisplay::Negative:
        return "negative"sv;
    default:
        VERIFY_NOT_REACHED();
    }
}

// 15.5.1 CurrencyDigits ( currency ), https://tc39.es/ecma402/#sec-currencydigits
int currency_digits(StringView currency)
{
    // 1. If the ISO 4217 currency and funds code list contains currency as an alphabetic code, return the minor
    //    unit value corresponding to the currency from the list; otherwise, return 2.
    if (auto currency_code = Unicode::get_currency_code(currency); currency_code.has_value())
        return currency_code->minor_unit.value_or(2);
    return 2;
}

// 15.5.3 FormatNumericToString ( intlObject, x ), https://tc39.es/ecma402/#sec-formatnumberstring
FormatResult format_numeric_to_string(NumberFormatBase const& intl_object, MathematicalValue number)
{
    bool is_negative = false;

    // 1. If x is negative-zero, then
    if (number.is_negative_zero()) {
        // a. Let isNegative be true.
        is_negative = true;

        // b. Set x to 0.
        number = MathematicalValue(0.0);
    }
    // 2. Else,
    else {
        // a. Assert: x is a mathematical value.
        VERIFY(number.is_mathematical_value());

        // b. If x < 0, let isNegative be true; else let isNegative be false.
        is_negative = number.is_negative();

        // c. If isNegative is true, then
        if (is_negative) {
            // i. Set x to -x.
            number.negate();
        }
    }

    // 3. Let unsignedRoundingMode be GetUnsignedRoundingMode(intlObject.[[RoundingMode]], isNegative).
    auto unsigned_rounding_mode = get_unsigned_rounding_mode(intl_object.rounding_mode(), is_negative);

    RawFormatResult result {};

    switch (intl_object.rounding_type()) {
    // 4. If intlObject.[[RoundingType]] is significantDigits, then
    case NumberFormatBase::RoundingType::SignificantDigits:
        // a. Let result be ToRawPrecision(x, intlObject.[[MinimumSignificantDigits]], intlObject.[[MaximumSignificantDigits]], unsignedRoundingMode).
        result = to_raw_precision(number, intl_object.min_significant_digits(), intl_object.max_significant_digits(), unsigned_rounding_mode);
        break;

    // 5. Else if intlObject.[[RoundingType]] is fractionDigits, then
    case NumberFormatBase::RoundingType::FractionDigits:
        // a. Let result be ToRawFixed(x, intlObject.[[MinimumFractionDigits]], intlObject.[[MaximumFractionDigits]], intlObject.[[RoundingIncrement]], unsignedRoundingMode).
        result = to_raw_fixed(number, intl_object.min_fraction_digits(), intl_object.max_fraction_digits(), intl_object.rounding_increment(), unsigned_rounding_mode);
        break;

    // 6. Else,
    case NumberFormatBase::RoundingType::MorePrecision:
    case NumberFormatBase::RoundingType::LessPrecision: {
        // a. Let sResult be ToRawPrecision(x, intlObject.[[MinimumSignificantDigits]], intlObject.[[MaximumSignificantDigits]], unsignedRoundingMode).
        auto significant_result = to_raw_precision(number, intl_object.min_significant_digits(), intl_object.max_significant_digits(), unsigned_rounding_mode);

        // b. Let fResult be ToRawFixed(x, intlObject.[[MinimumFractionDigits]], intlObject.[[MaximumFractionDigits]], intlObject.[[RoundingIncrement]], unsignedRoundingMode).
        auto fraction_result = to_raw_fixed(number, intl_object.min_fraction_digits(), intl_object.max_fraction_digits(), intl_object.rounding_increment(), unsigned_rounding_mode);

        // c. If intlObj.[[RoundingType]] is morePrecision, then
        if (intl_object.rounding_type() == NumberFormatBase::RoundingType::MorePrecision) {
            // i. If sResult.[[RoundingMagnitude]] ≤ fResult.[[RoundingMagnitude]], then
            if (significant_result.rounding_magnitude <= fraction_result.rounding_magnitude) {
                // 1. Let result be sResult.
                result = move(significant_result);
            }
            // ii. Else,
            else {
                // 2. Let result be fResult.
                result = move(fraction_result);
            }
        }
        // d. Else,
        else {
            // i. Assert: intlObj.[[RoundingType]] is lessPrecision.
            VERIFY(intl_object.rounding_type() == NumberFormatBase::RoundingType::LessPrecision);

            // ii. If sResult.[[RoundingMagnitude]] ≤ fResult.[[RoundingMagnitude]], then
            if (significant_result.rounding_magnitude <= fraction_result.rounding_magnitude) {
                // 1. Let result be fResult.
                result = move(fraction_result);
            }
            // iii. Else,
            else {
                // 1. Let result be sResult.
                result = move(significant_result);
            }
        }

        break;
    }

    default:
        VERIFY_NOT_REACHED();
    }

    // 7. Set x to result.[[RoundedNumber]].
    number = move(result.rounded_number);

    // 8. Let string be result.[[FormattedString]].
    auto string = move(result.formatted_string);

    // 9. If intlObject.[[TrailingZeroDisplay]] is "stripIfInteger" and x modulo 1 = 0, then
    if ((intl_object.trailing_zero_display() == NumberFormat::TrailingZeroDisplay::StripIfInteger) && number.modulo_is_zero(1)) {
        // a. Let i be StringIndexOf(string, ".", 0).
        auto index = string.find_byte_offset('.');

        // b. If i ≠ -1, set string to the substring of string from 0 to i.
        if (index.has_value())
            string = MUST(string.substring_from_byte_offset(0, *index));
    }

    // 10. Let int be result.[[IntegerDigitsCount]].
    int digits = result.digits;

    // 11. Let minInteger be intlObject.[[MinimumIntegerDigits]].
    int min_integer = intl_object.min_integer_digits();

    // 12. If int < minInteger, then
    if (digits < min_integer) {
        // a. Let forwardZeros be the String consisting of minInteger - int occurrences of the code unit 0x0030 (DIGIT ZERO).
        auto forward_zeros = MUST(String::repeated('0', min_integer - digits));

        // b. Set string to the string-concatenation of forwardZeros and string.
        string = MUST(String::formatted("{}{}", forward_zeros, string));
    }

    // 13. If isNegative is true, then
    if (is_negative) {
        // a. If x is 0, set x to negative-zero. Otherwise, set x to -x.
        if (number.is_zero())
            number = MathematicalValue { MathematicalValue::Symbol::NegativeZero };
        else
            number.negate();
    }

    // 14. Return the Record { [[RoundedNumber]]: x, [[FormattedString]]: string }.
    return { move(string), move(number) };
}

// 15.5.4 PartitionNumberPattern ( numberFormat, x ), https://tc39.es/ecma402/#sec-partitionnumberpattern
Vector<PatternPartition> partition_number_pattern(VM& vm, NumberFormat& number_format, MathematicalValue number)
{
    // 1. Let exponent be 0.
    int exponent = 0;

    String formatted_string;

    // 2. If x is not-a-number, then
    if (number.is_nan()) {
        // a. Let n be an implementation- and locale-dependent (ILD) String value indicating the NaN value.
        auto symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::NaN).value_or("NaN"sv);
        formatted_string = MUST(String::from_utf8(symbol));
    }
    // 3. Else if x is positive-infinity, then
    else if (number.is_positive_infinity()) {
        // a. Let n be an ILD String value indicating positive infinity.
        auto symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::Infinity).value_or("infinity"sv);
        formatted_string = MUST(String::from_utf8(symbol));
    }
    // 4. Else if x is negative-infinity, then
    else if (number.is_negative_infinity()) {
        // a. Let n be an ILD String value indicating negative infinity.
        // NOTE: The CLDR does not contain unique strings for negative infinity. The negative sign will
        //       be inserted by the pattern returned from GetNumberFormatPattern.
        auto symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::Infinity).value_or("infinity"sv);
        formatted_string = MUST(String::from_utf8(symbol));
    }
    // 5. Else,
    else {
        // a. If x is not negative-zero,
        if (!number.is_negative_zero()) {
            // i. Assert: x is a mathematical value.
            VERIFY(number.is_mathematical_value());

            // ii. If numberFormat.[[Style]] is "percent", let x be 100 × x.
            if (number_format.style() == NumberFormat::Style::Percent)
                number = number.multiplied_by(100);

            // iii. Let exponent be ComputeExponent(numberFormat, x).
            exponent = compute_exponent(number_format, number);

            // iv. Let x be x × 10^-exponent.
            number = number.multiplied_by_power(-exponent);
        }

        // b. Let formatNumberResult be FormatNumericToString(numberFormat, x).
        auto format_number_result = format_numeric_to_string(number_format, move(number));

        // c. Let n be formatNumberResult.[[FormattedString]].
        formatted_string = move(format_number_result.formatted_string);

        // d. Let x be formatNumberResult.[[RoundedNumber]].
        number = move(format_number_result.rounded_number);
    }

    ::Locale::NumberFormat found_pattern {};

    // 6. Let pattern be GetNumberFormatPattern(numberFormat, x).
    auto pattern = get_number_format_pattern(vm, number_format, number, found_pattern);
    if (!pattern.has_value())
        return {};

    // 7. Let result be a new empty List.
    Vector<PatternPartition> result;

    // 8. Let patternParts be PartitionPattern(pattern).
    auto pattern_parts = pattern->visit([](auto const& p) { return partition_pattern(p); });

    // 9. For each Record { [[Type]], [[Value]] } patternPart of patternParts, do
    for (auto& pattern_part : pattern_parts) {
        // a. Let p be patternPart.[[Type]].
        auto part = pattern_part.type;

        // b. If p is "literal", then
        if (part == "literal"sv) {
            // i. Append a new Record { [[Type]]: "literal", [[Value]]: patternPart.[[Value]] } as the last element of result.
            result.append({ "literal"sv, move(pattern_part.value) });
        }

        // c. Else if p is equal to "number", then
        else if (part == "number"sv) {
            // i. Let notationSubParts be PartitionNotationSubPattern(numberFormat, x, n, exponent).
            auto notation_sub_parts = partition_notation_sub_pattern(number_format, number, formatted_string, exponent);
            // ii. Append all elements of notationSubParts to result.
            result.extend(move(notation_sub_parts));
        }

        // d. Else if p is equal to "plusSign", then
        else if (part == "plusSign"sv) {
            // i. Let plusSignSymbol be the ILND String representing the plus sign.
            auto plus_sign_symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::PlusSign).value_or("+"sv);
            // ii. Append a new Record { [[Type]]: "plusSign", [[Value]]: plusSignSymbol } as the last element of result.
            result.append({ "plusSign"sv, MUST(String::from_utf8(plus_sign_symbol)) });
        }

        // e. Else if p is equal to "minusSign", then
        else if (part == "minusSign"sv) {
            // i. Let minusSignSymbol be the ILND String representing the minus sign.
            auto minus_sign_symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::MinusSign).value_or("-"sv);
            // ii. Append a new Record { [[Type]]: "minusSign", [[Value]]: minusSignSymbol } as the last element of result.
            result.append({ "minusSign"sv, MUST(String::from_utf8(minus_sign_symbol)) });
        }

        // f. Else if p is equal to "percentSign" and numberFormat.[[Style]] is "percent", then
        else if ((part == "percentSign"sv) && (number_format.style() == NumberFormat::Style::Percent)) {
            // i. Let percentSignSymbol be the ILND String representing the percent sign.
            auto percent_sign_symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::PercentSign).value_or("%"sv);
            // ii. Append a new Record { [[Type]]: "percentSign", [[Value]]: percentSignSymbol } as the last element of result.
            result.append({ "percentSign"sv, MUST(String::from_utf8(percent_sign_symbol)) });
        }

        // g. Else if p is equal to "unitPrefix" and numberFormat.[[Style]] is "unit", then
        // h. Else if p is equal to "unitSuffix" and numberFormat.[[Style]] is "unit", then
        else if ((part.starts_with("unitIdentifier:"sv)) && (number_format.style() == NumberFormat::Style::Unit)) {
            // Note: Our implementation combines "unitPrefix" and "unitSuffix" into one field, "unitIdentifier".

            auto identifier_index = part.substring_view("unitIdentifier:"sv.length()).to_number<unsigned>();
            VERIFY(identifier_index.has_value());

            // i. Let unit be numberFormat.[[Unit]].
            // ii. Let unitDisplay be numberFormat.[[UnitDisplay]].
            // iii. Let mu be an ILD String value representing unit before x in unitDisplay form, which may depend on x in languages having different plural forms.
            auto unit_identifier = found_pattern.identifiers[*identifier_index];

            // iv. Append a new Record { [[Type]]: "unit", [[Value]]: mu } as the last element of result.
            result.append({ "unit"sv, MUST(String::from_utf8(unit_identifier)) });
        }

        // i. Else if p is equal to "currencyCode" and numberFormat.[[Style]] is "currency", then
        // j. Else if p is equal to "currencyPrefix" and numberFormat.[[Style]] is "currency", then
        // k. Else if p is equal to "currencySuffix" and numberFormat.[[Style]] is "currency", then
        //
        // Note: Our implementation manipulates the format string to inject/remove spacing around the
        //       currency code during GetNumberFormatPattern so that we do not have to do currency
        //       display / plurality lookups more than once.
        else if ((part == "currency"sv) && (number_format.style() == NumberFormat::Style::Currency)) {
            auto currency = number_format.resolve_currency_display();
            result.append({ "currency"sv, MUST(String::from_utf8(currency)) });
        }

        // l. Else,
        else {
            // i. Let unknown be an ILND String based on x and p.
            // ii. Append a new Record { [[Type]]: "unknown", [[Value]]: unknown } as the last element of result.

            // LibUnicode doesn't generate any "unknown" patterns.
            VERIFY_NOT_REACHED();
        }
    }

    // 10. Return result.
    return result;
}

static Vector<String> separate_integer_into_groups(::Locale::NumberGroupings const& grouping_sizes, String integer, NumberFormat::UseGrouping use_grouping)
{
    auto utf8_integer = integer.code_points();
    if (utf8_integer.length() <= grouping_sizes.primary_grouping_size)
        return { move(integer) };

    size_t index = utf8_integer.length() - grouping_sizes.primary_grouping_size;

    switch (use_grouping) {
    case NumberFormat::UseGrouping::Min2:
        if (utf8_integer.length() < 5)
            return { move(integer) };
        break;

    case NumberFormat::UseGrouping::Auto:
        if (index < grouping_sizes.minimum_grouping_digits)
            return { move(integer) };
        break;

    case NumberFormat::UseGrouping::Always:
        break;

    default:
        VERIFY_NOT_REACHED();
    }

    Vector<String> groups;

    auto add_group = [&](size_t index, size_t length) {
        length = utf8_integer.unicode_substring_view(index, length).byte_length();
        index = utf8_integer.byte_offset_of(index);

        auto group = MUST(integer.substring_from_byte_offset_with_shared_superstring(index, length));
        groups.prepend(move(group));
    };

    add_group(index, grouping_sizes.primary_grouping_size);

    while (index > grouping_sizes.secondary_grouping_size) {
        index -= grouping_sizes.secondary_grouping_size;
        add_group(index, grouping_sizes.secondary_grouping_size);
    }

    if (index > 0)
        add_group(0, index);

    return groups;
}

// 15.5.5 PartitionNotationSubPattern ( numberFormat, x, n, exponent ), https://tc39.es/ecma402/#sec-partitionnotationsubpattern
Vector<PatternPartition> partition_notation_sub_pattern(NumberFormat& number_format, MathematicalValue const& number, String formatted_string, int exponent)
{
    // 1. Let result be a new empty List.
    Vector<PatternPartition> result;

    auto grouping_sizes = ::Locale::get_number_system_groupings(number_format.data_locale(), number_format.numbering_system());
    if (!grouping_sizes.has_value())
        return {};

    // 2. If x is not-a-number, then
    if (number.is_nan()) {
        // a. Append a new Record { [[Type]]: "nan", [[Value]]: n } as the last element of result.
        result.append({ "nan"sv, move(formatted_string) });
    }
    // 3. Else if x is positive-infinity or negative-infinity, then
    else if (number.is_positive_infinity() || number.is_negative_infinity()) {
        // a. Append a new Record { [[Type]]: "infinity", [[Value]]: n } as the last element of result.
        result.append({ "infinity"sv, move(formatted_string) });
    }
    // 4. Else,
    else {
        // a. Let notationSubPattern be GetNotationSubPattern(numberFormat, exponent).
        auto notation_sub_pattern = get_notation_sub_pattern(number_format, exponent);
        if (!notation_sub_pattern.has_value())
            return {};

        // b. Let patternParts be PartitionPattern(notationSubPattern).
        auto pattern_parts = partition_pattern(*notation_sub_pattern);

        // c. For each Record { [[Type]], [[Value]] } patternPart of patternParts, do
        for (auto& pattern_part : pattern_parts) {
            // i. Let p be patternPart.[[Type]].
            auto part = pattern_part.type;

            // ii. If p is "literal", then
            if (part == "literal"sv) {
                // 1. Append a new Record { [[Type]]: "literal", [[Value]]: patternPart.[[Value]] } as the last element of result.
                result.append({ "literal"sv, move(pattern_part.value) });
            }
            // iii. Else if p is equal to "number", then
            else if (part == "number"sv) {
                // 1. If the numberFormat.[[NumberingSystem]] matches one of the values in the "Numbering System" column of Table 14 below, then
                //     a. Let digits be a List whose elements are the code points specified in the "Digits" column of the matching row in Table 14.
                //     b. Assert: The length of digits is 10.
                //     c. Let transliterated be the empty String.
                //     d. Let len be the length of n.
                //     e. Let position be 0.
                //     f. Repeat, while position < len,
                //         i. Let c be the code unit at index position within n.
                //         ii. If 0x0030 ≤ c ≤ 0x0039, then
                //             i. NOTE: c is an ASCII digit.
                //             ii. Let i be c - 0x0030.
                //             iii. Set c to CodePointsToString(« digits[i] »).
                //         iii. Set transliterated to the string-concatenation of transliterated and c.
                //         iv. Set position to position + 1.
                //     g. Set n to transliterated.
                // 2. Else use an implementation dependent algorithm to map n to the appropriate representation of n in the given numbering system.
                formatted_string = ::Locale::replace_digits_for_number_system(number_format.numbering_system(), formatted_string);

                // 3. Let decimalSepIndex be StringIndexOf(n, ".", 0).
                auto decimal_sep_index = formatted_string.find_byte_offset('.');

                String integer;
                Optional<String> fraction;

                // 4. If decimalSepIndex > 0, then
                if (decimal_sep_index.has_value() && (*decimal_sep_index > 0)) {
                    // a. Let integer be the substring of n from position 0, inclusive, to position decimalSepIndex, exclusive.
                    integer = MUST(formatted_string.substring_from_byte_offset_with_shared_superstring(0, *decimal_sep_index));

                    // b. Let fraction be the substring of n from position decimalSepIndex, exclusive, to the end of n.
                    fraction = MUST(formatted_string.substring_from_byte_offset_with_shared_superstring(*decimal_sep_index + 1));
                }
                // 5. Else,
                else {
                    // a. Let integer be n.
                    integer = move(formatted_string);
                    // b. Let fraction be undefined.
                }

                // 6. If the numberFormat.[[UseGrouping]] is false, then
                if (number_format.use_grouping() == NumberFormat::UseGrouping::False) {
                    // a. Append a new Record { [[Type]]: "integer", [[Value]]: integer } as the last element of result.
                    result.append({ "integer"sv, move(integer) });
                }
                // 7. Else,
                else {
                    // a. Let groupSepSymbol be the implementation-, locale-, and numbering system-dependent (ILND) String representing the grouping separator.
                    auto group_sep_symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::Group).value_or(","sv);

                    // b. Let groups be a List whose elements are, in left to right order, the substrings defined by ILND set of locations within the integer, which may depend on the value of numberFormat.[[UseGrouping]].
                    auto groups = separate_integer_into_groups(*grouping_sizes, move(integer), number_format.use_grouping());

                    // c. Assert: The number of elements in groups List is greater than 0.
                    VERIFY(!groups.is_empty());

                    // d. Repeat, while groups List is not empty,
                    while (!groups.is_empty()) {
                        // i. Remove the first element from groups and let integerGroup be the value of that element.
                        auto integer_group = groups.take_first();

                        // ii. Append a new Record { [[Type]]: "integer", [[Value]]: integerGroup } as the last element of result.
                        result.append({ "integer"sv, move(integer_group) });

                        // iii. If groups List is not empty, then
                        if (!groups.is_empty()) {
                            // i. Append a new Record { [[Type]]: "group", [[Value]]: groupSepSymbol } as the last element of result.
                            result.append({ "group"sv, MUST(String::from_utf8(group_sep_symbol)) });
                        }
                    }
                }

                // 8. If fraction is not undefined, then
                if (fraction.has_value()) {
                    // a. Let decimalSepSymbol be the ILND String representing the decimal separator.
                    auto decimal_sep_symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::Decimal).value_or("."sv);
                    // b. Append a new Record { [[Type]]: "decimal", [[Value]]: decimalSepSymbol } as the last element of result.
                    result.append({ "decimal"sv, MUST(String::from_utf8(decimal_sep_symbol)) });
                    // c. Append a new Record { [[Type]]: "fraction", [[Value]]: fraction } as the last element of result.
                    result.append({ "fraction"sv, fraction.release_value() });
                }
            }
            // iv. Else if p is equal to "compactSymbol", then
            // v. Else if p is equal to "compactName", then
            else if (part.starts_with("compactIdentifier:"sv)) {
                // Note: Our implementation combines "compactSymbol" and "compactName" into one field, "compactIdentifier".

                auto identifier_index = part.substring_view("compactIdentifier:"sv.length()).to_number<unsigned>();
                VERIFY(identifier_index.has_value());

                // 1. Let compactSymbol be an ILD string representing exponent in short form, which may depend on x in languages having different plural forms. The implementation must be able to provide this string, or else the pattern would not have a "{compactSymbol}" placeholder.
                auto compact_identifier = number_format.compact_format().identifiers[*identifier_index];

                // 2. Append a new Record { [[Type]]: "compact", [[Value]]: compactSymbol } as the last element of result.
                result.append({ "compact"sv, MUST(String::from_utf8(compact_identifier)) });
            }
            // vi. Else if p is equal to "scientificSeparator", then
            else if (part == "scientificSeparator"sv) {
                // 1. Let scientificSeparator be the ILND String representing the exponent separator.
                auto scientific_separator = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::Exponential).value_or("E"sv);
                // 2. Append a new Record { [[Type]]: "exponentSeparator", [[Value]]: scientificSeparator } as the last element of result.
                result.append({ "exponentSeparator"sv, MUST(String::from_utf8(scientific_separator)) });
            }
            // vii. Else if p is equal to "scientificExponent", then
            else if (part == "scientificExponent"sv) {
                // 1. If exponent < 0, then
                if (exponent < 0) {
                    // a. Let minusSignSymbol be the ILND String representing the minus sign.
                    auto minus_sign_symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::MinusSign).value_or("-"sv);

                    // b. Append a new Record { [[Type]]: "exponentMinusSign", [[Value]]: minusSignSymbol } as the last element of result.
                    result.append({ "exponentMinusSign"sv, MUST(String::from_utf8(minus_sign_symbol)) });

                    // c. Let exponent be -exponent.
                    exponent *= -1;
                }

                // 2. Let exponentResult be ToRawFixed(exponent, 0, 0, 1, undefined).
                auto exponent_value = MathematicalValue { static_cast<double>(exponent) };
                auto exponent_result = to_raw_fixed(exponent_value, 0, 0, 1, {});

                // FIXME: The spec does not say to do this, but all of major engines perform this replacement.
                //        Without this, formatting with non-Latin numbering systems will produce non-localized results.
                exponent_result.formatted_string = ::Locale::replace_digits_for_number_system(number_format.numbering_system(), exponent_result.formatted_string);

                // 3. Append a new Record { [[Type]]: "exponentInteger", [[Value]]: exponentResult.[[FormattedString]] } as the last element of result.
                result.append({ "exponentInteger"sv, move(exponent_result.formatted_string) });
            }
            // viii. Else,
            else {
                // 1. Let unknown be an ILND String based on x and p.
                // 2. Append a new Record { [[Type]]: "unknown", [[Value]]: unknown } as the last element of result.

                // LibUnicode doesn't generate any "unknown" patterns.
                VERIFY_NOT_REACHED();
            }
        }
    }

    // 5. Return result.
    return result;
}

// 15.5.6 FormatNumeric ( numberFormat, x ), https://tc39.es/ecma402/#sec-formatnumber
String format_numeric(VM& vm, NumberFormat& number_format, MathematicalValue number)
{
    // 1. Let parts be ? PartitionNumberPattern(numberFormat, x).
    auto parts = partition_number_pattern(vm, number_format, move(number));

    // 2. Let result be the empty String.
    StringBuilder result;

    // 3. For each Record { [[Type]], [[Value]] } part in parts, do
    for (auto& part : parts) {
        // a. Set result to the string-concatenation of result and part.[[Value]].
        result.append(part.value);
    }

    // 4. Return result.
    return MUST(result.to_string());
}

// 15.5.7 FormatNumericToParts ( numberFormat, x ), https://tc39.es/ecma402/#sec-formatnumbertoparts
NonnullGCPtr<Array> format_numeric_to_parts(VM& vm, NumberFormat& number_format, MathematicalValue number)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionNumberPattern(numberFormat, x).
    auto parts = partition_number_pattern(vm, number_format, move(number));

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

static String cut_trailing_zeroes(StringView string, int cut)
{
    // These steps are exactly the same between ToRawPrecision and ToRawFixed.

    // Repeat, while cut > 0 and the last code unit of m is 0x0030 (DIGIT ZERO),
    while ((cut > 0) && string.ends_with('0')) {
        // Remove the last code unit from m.
        string = string.substring_view(0, string.length() - 1);

        // Decrease cut by 1.
        --cut;
    }

    // If the last code unit of m is 0x002E (FULL STOP), then
    if (string.ends_with('.')) {
        // Remove the last code unit from m.
        string = string.substring_view(0, string.length() - 1);
    }

    return MUST(String::from_utf8(string));
}

enum class PreferredResult {
    LessThanNumber,
    GreaterThanNumber,
};

struct RawPrecisionResult {
    MathematicalValue number;
    int exponent { 0 };
    MathematicalValue rounded;
};

// ToRawPrecisionFn, https://tc39.es/ecma402/#eqn-ToRawPrecisionFn
static RawPrecisionResult to_raw_precision_function(MathematicalValue const& number, int precision, PreferredResult mode)
{
    RawPrecisionResult result {};
    result.exponent = number.logarithmic_floor();

    if (number.is_number()) {
        result.number = number.divided_by_power(result.exponent - precision + 1);

        switch (mode) {
        case PreferredResult::LessThanNumber:
            result.number = MathematicalValue { floor(result.number.as_number()) };
            break;
        case PreferredResult::GreaterThanNumber:
            result.number = MathematicalValue { ceil(result.number.as_number()) };
            break;
        }
    } else {
        // NOTE: In order to round the BigInt to the proper precision, this computation is initially off by a
        //       factor of 10. This lets us inspect the ones digit and then round up if needed.
        result.number = number.divided_by_power(result.exponent - precision);

        // FIXME: Can we do this without string conversion?
        auto digits = result.number.to_string();
        auto digit = digits.bytes_as_string_view().substring_view(digits.bytes_as_string_view().length() - 1);

        result.number = result.number.divided_by(10);

        if (mode == PreferredResult::GreaterThanNumber && digit.to_number<unsigned>().value() != 0)
            result.number = result.number.plus(1);
    }

    result.rounded = result.number.multiplied_by_power(result.exponent - precision + 1);
    return result;
}

// 15.5.8 ToRawPrecision ( x, minPrecision, maxPrecision ), https://tc39.es/ecma402/#sec-torawprecision
RawFormatResult to_raw_precision(MathematicalValue const& number, int min_precision, int max_precision, NumberFormat::UnsignedRoundingMode unsigned_rounding_mode)
{
    RawFormatResult result {};

    // 1. Let p be maxPrecision.
    int precision = max_precision;
    int exponent = 0;

    // 2. If x = 0, then
    if (number.is_zero()) {
        // a. Let m be the String consisting of p occurrences of the code unit 0x0030 (DIGIT ZERO).
        result.formatted_string = MUST(String::repeated('0', precision));

        // b. Let e be 0.
        exponent = 0;

        // c. Let xFinal be 0.
        result.rounded_number = MathematicalValue { 0.0 };
    }
    // 3. Else,
    else {
        // a. Let n1 and e1 each be an integer and r1 a mathematical value, with r1 = ToRawPrecisionFn(n1, e1, p), such that r1 ≤ x and r1 is maximized.
        auto [number1, exponent1, rounded1] = to_raw_precision_function(number, precision, PreferredResult::LessThanNumber);

        // b. Let n2 and e2 each be an integer and r2 a mathematical value, with r2 = ToRawPrecisionFn(n2, e2, p), such that r2 ≥ x and r2 is minimized.
        auto [number2, exponent2, rounded2] = to_raw_precision_function(number, precision, PreferredResult::GreaterThanNumber);

        // c. Let r be ApplyUnsignedRoundingMode(x, r1, r2, unsignedRoundingMode).
        auto rounded = apply_unsigned_rounding_mode(number, rounded1, rounded2, unsigned_rounding_mode);

        MathematicalValue n;

        // d. If r is r1, then
        if (rounded == RoundingDecision::LowerValue) {
            // i. Let n be n1.
            n = move(number1);

            // ii. Let e be e1.
            exponent = exponent1;

            // iii. Let xFinal be r1.
            result.rounded_number = move(rounded1);
        }
        // e. Else,
        else {
            // i. Let n be n2.
            n = move(number2);

            // ii. Let e be e2.
            exponent = exponent2;

            // iii. Let xFinal be r2.
            result.rounded_number = move(rounded2);
        }

        // f. Let m be the String consisting of the digits of the decimal representation of n (in order, with no leading zeroes).
        result.formatted_string = n.to_string();
    }

    // 4. If e ≥ (p – 1), then
    if (exponent >= (precision - 1)) {
        // a. Set m to the string-concatenation of m and e - p + 1 occurrences of the code unit 0x0030 (DIGIT ZERO).
        result.formatted_string = MUST(String::formatted(
            "{}{}",
            result.formatted_string,
            MUST(String::repeated('0', exponent - precision + 1))));

        // b. Let int be e + 1.
        result.digits = exponent + 1;
    }
    // 5. Else if e ≥ 0, then
    else if (exponent >= 0) {
        // a. Set m to the string-concatenation of the first e + 1 code units of m, the code unit 0x002E (FULL STOP), and the remaining p - (e + 1) code units of m.
        result.formatted_string = MUST(String::formatted(
            "{}.{}",
            result.formatted_string.bytes_as_string_view().substring_view(0, exponent + 1),
            result.formatted_string.bytes_as_string_view().substring_view(exponent + 1)));

        // b. Let int be e + 1.
        result.digits = exponent + 1;
    }
    // 6. Else,
    else {
        // a. Assert: e < 0.
        // b. Set m to the string-concatenation of "0.", -(e + 1) occurrences of the code unit 0x0030 (DIGIT ZERO), and m.
        result.formatted_string = MUST(String::formatted(
            "0.{}{}",
            MUST(String::repeated('0', -1 * (exponent + 1))),
            result.formatted_string));

        // c. Let int be 1.
        result.digits = 1;
    }

    // 7. If m contains the code unit 0x002E (FULL STOP) and maxPrecision > minPrecision, then
    if (result.formatted_string.contains('.') && (max_precision > min_precision)) {
        // a. Let cut be maxPrecision – minPrecision.
        int cut = max_precision - min_precision;

        // Steps 8b-8c are implemented by cut_trailing_zeroes.
        result.formatted_string = cut_trailing_zeroes(result.formatted_string, cut);
    }

    // 8. Return the Record { [[FormattedString]]: m, [[RoundedNumber]]: xFinal, [[IntegerDigitsCount]]: int, [[RoundingMagnitude]]: e–p+1 }.
    result.rounding_magnitude = exponent - precision + 1;
    return result;
}

struct RawFixedResult {
    MathematicalValue number;
    MathematicalValue rounded;
};

// ToRawFixedFn, https://tc39.es/ecma402/#eqn-ToRawFixedFn
static RawFixedResult to_raw_fixed_function(MathematicalValue const& number, int fraction, int rounding_increment, PreferredResult mode)
{
    RawFixedResult result {};

    if (number.is_number()) {
        result.number = number.multiplied_by_power(fraction);

        switch (mode) {
        case PreferredResult::LessThanNumber:
            result.number = MathematicalValue { floor(result.number.as_number()) };
            break;
        case PreferredResult::GreaterThanNumber:
            result.number = MathematicalValue { ceil(result.number.as_number()) };
            break;
        }
    } else {
        // NOTE: In order to round the BigInt to the proper precision, this computation is initially off by a
        //       factor of 10. This lets us inspect the ones digit and then round up if needed.
        result.number = number.multiplied_by_power(fraction - 1);

        // FIXME: Can we do this without string conversion?
        auto digits = result.number.to_string();
        auto digit = digits.bytes_as_string_view().substring_view(digits.bytes_as_string_view().length() - 1);

        result.number = result.number.multiplied_by(10);

        if (mode == PreferredResult::GreaterThanNumber && digit.to_number<unsigned>().value() != 0)
            result.number = result.number.plus(1);
    }

    while (!result.number.modulo_is_zero(rounding_increment)) {
        switch (mode) {
        case PreferredResult::LessThanNumber:
            result.number = result.number.minus(1);
            break;
        case PreferredResult::GreaterThanNumber:
            result.number = result.number.plus(1);
            break;
        }
    }

    result.rounded = result.number.divided_by_power(fraction);
    return result;
}

// 15.5.9 ToRawFixed ( x, minInteger, minFraction, maxFraction ), https://tc39.es/ecma402/#sec-torawfixed
RawFormatResult to_raw_fixed(MathematicalValue const& number, int min_fraction, int max_fraction, int rounding_increment, NumberFormat::UnsignedRoundingMode unsigned_rounding_mode)
{
    RawFormatResult result {};

    // 1. Let f be maxFraction.
    int fraction = max_fraction;

    // 2. Let n1 be an integer and r1 a mathematical value, with r1 = ToRawFixedFn(n1, f), such that n1 modulo roundingIncrement = 0, r1 ≤ x, and r1 is maximized.
    auto [number1, rounded1] = to_raw_fixed_function(number, fraction, rounding_increment, PreferredResult::LessThanNumber);

    // 3. Let n2 be an integer and r2 a mathematical value, with r2 = ToRawFixedFn(n2, f), such that n2 modulo roundingIncrement = 0, r2 ≥ x, and r2 is minimized.
    auto [number2, rounded2] = to_raw_fixed_function(number, fraction, rounding_increment, PreferredResult::GreaterThanNumber);

    // 4. Let r be ApplyUnsignedRoundingMode(x, r1, r2, unsignedRoundingMode).
    auto rounded = apply_unsigned_rounding_mode(number, rounded1, rounded2, unsigned_rounding_mode);

    MathematicalValue n;

    // 5. If r is r1, then
    if (rounded == RoundingDecision::LowerValue) {
        // a. Let n be n1.
        n = move(number1);

        // b. Let xFinal be r1.
        result.rounded_number = move(rounded1);
    }
    // 6. Else,
    else {
        // a. Let n be n2.
        n = move(number2);

        // b. Let xFinal be r2.
        result.rounded_number = move(rounded2);
    }

    // 7. If n = 0, let m be "0". Otherwise, let m be the String consisting of the digits of the decimal representation of n (in order, with no leading zeroes).
    result.formatted_string = n.is_zero() ? "0"_string : n.to_string();

    // 8. If f ≠ 0, then
    if (fraction != 0) {
        // a. Let k be the length of m.
        auto decimals = result.formatted_string.bytes_as_string_view().length();

        // b. If k ≤ f, then
        if (decimals <= static_cast<size_t>(fraction)) {
            // i. Let z be the String value consisting of f + 1 - k occurrences of the code unit 0x0030 (DIGIT ZERO).
            auto zeroes = MUST(String::repeated('0', fraction + 1 - decimals));

            // ii. Let m be the string-concatenation of z and m.
            result.formatted_string = MUST(String::formatted("{}{}", zeroes, result.formatted_string));

            // iii. Let k be f + 1.
            decimals = fraction + 1;
        }

        // c. Let a be the first k - f code units of m, and let b be the remaining f code units of m.
        auto a = result.formatted_string.bytes_as_string_view().substring_view(0, decimals - fraction);
        auto b = result.formatted_string.bytes_as_string_view().substring_view(decimals - fraction, fraction);

        // d. Let m be the string-concatenation of a, ".", and b.
        result.formatted_string = MUST(String::formatted("{}.{}", a, b));

        // e. Let int be the length of a.
        result.digits = a.length();
    }
    // 9. Else, let int be the length of m.
    else {
        result.digits = result.formatted_string.bytes_as_string_view().length();
    }

    // 10. Let cut be maxFraction – minFraction.
    int cut = max_fraction - min_fraction;

    // Steps 11-12 are implemented by cut_trailing_zeroes.
    result.formatted_string = cut_trailing_zeroes(result.formatted_string, cut);

    // 13. Return the Record { [[FormattedString]]: m, [[RoundedNumber]]: xFinal, [[IntegerDigitsCount]]: int, [[RoundingMagnitude]]: –f }.
    result.rounding_magnitude = -fraction;
    return result;
}

enum class NumberCategory {
    NegativeNonZero,
    NegativeZero,
    PositiveNonZero,
    PositiveZero,
};

// 15.5.11 GetNumberFormatPattern ( numberFormat, x ), https://tc39.es/ecma402/#sec-getnumberformatpattern
Optional<Variant<StringView, String>> get_number_format_pattern(VM& vm, NumberFormat& number_format, MathematicalValue const& number, ::Locale::NumberFormat& found_pattern)
{
    // 1. Let localeData be %NumberFormat%.[[LocaleData]].
    // 2. Let dataLocale be numberFormat.[[DataLocale]].
    // 3. Let dataLocaleData be localeData.[[<dataLocale>]].
    // 4. Let patterns be dataLocaleData.[[patterns]].
    // 5. Assert: patterns is a Record (see 15.3.3).
    Optional<::Locale::NumberFormat> patterns;

    // 6. Let style be numberFormat.[[Style]].
    switch (number_format.style()) {
    // 7. If style is "percent", then
    case NumberFormat::Style::Percent:
        // a. Let patterns be patterns.[[percent]].
        patterns = ::Locale::get_standard_number_system_format(number_format.data_locale(), number_format.numbering_system(), ::Locale::StandardNumberFormatType::Percent);
        break;

    // 8. Else if style is "unit", then
    case NumberFormat::Style::Unit: {
        // a. Let unit be numberFormat.[[Unit]].
        // b. Let unitDisplay be numberFormat.[[UnitDisplay]].
        // c. Let patterns be patterns.[[unit]].
        // d. If patterns doesn't have a field [[<unit>]], then
        //     i. Let unit be "fallback".
        // e. Let patterns be patterns.[[<unit>]].
        // f. Let patterns be patterns.[[<unitDisplay>]].
        auto formats = ::Locale::get_unit_formats(number_format.data_locale(), number_format.unit(), number_format.unit_display());
        auto plurality = resolve_plural(number_format, ::Locale::PluralForm::Cardinal, number.to_value(vm));

        if (auto it = formats.find_if([&](auto& p) { return p.plurality == plurality.plural_category; }); it != formats.end())
            patterns = move(*it);

        break;
    }

    // 9. Else if style is "currency", then
    case NumberFormat::Style::Currency:
        // a. Let currency be numberFormat.[[Currency]].
        // b. Let currencyDisplay be numberFormat.[[CurrencyDisplay]].
        // c. Let currencySign be numberFormat.[[CurrencySign]].
        // d. Let patterns be patterns.[[currency]].
        // e. If patterns doesn't have a field [[<currency>]], then
        //     i. Let currency be "fallback".
        // f. Let patterns be patterns.[[<currency>]].
        // g. Let patterns be patterns.[[<currencyDisplay>]].
        // h. Let patterns be patterns.[[<currencySign>]].

        // Handling of other [[CurrencyDisplay]] options will occur after [[SignDisplay]].
        if (number_format.currency_display() == NumberFormat::CurrencyDisplay::Name) {
            auto formats = ::Locale::get_compact_number_system_formats(number_format.data_locale(), number_format.numbering_system(), ::Locale::CompactNumberFormatType::CurrencyUnit);
            auto plurality = resolve_plural(number_format, ::Locale::PluralForm::Cardinal, number.to_value(vm));

            if (auto it = formats.find_if([&](auto& p) { return p.plurality == plurality.plural_category; }); it != formats.end()) {
                patterns = move(*it);
                break;
            }
        }

        switch (number_format.currency_sign()) {
        case NumberFormat::CurrencySign::Standard:
            patterns = ::Locale::get_standard_number_system_format(number_format.data_locale(), number_format.numbering_system(), ::Locale::StandardNumberFormatType::Currency);
            break;
        case NumberFormat::CurrencySign::Accounting:
            patterns = ::Locale::get_standard_number_system_format(number_format.data_locale(), number_format.numbering_system(), ::Locale::StandardNumberFormatType::Accounting);
            break;
        }

        break;

    // 10. Else,
    case NumberFormat::Style::Decimal:
        // a. Assert: style is "decimal".
        // b. Let patterns be patterns.[[decimal]].
        patterns = ::Locale::get_standard_number_system_format(number_format.data_locale(), number_format.numbering_system(), ::Locale::StandardNumberFormatType::Decimal);
        break;

    default:
        VERIFY_NOT_REACHED();
    }

    if (!patterns.has_value())
        return {};

    NumberCategory category;

    // 11. If x is negative-infinity, then
    if (number.is_negative_infinity()) {
        // a. Let category be negative-nonzero.
        category = NumberCategory::NegativeNonZero;
    }
    // 12. Else if x is negative-zero, then
    else if (number.is_negative_zero()) {
        // a. Let category be negative-zero.
        category = NumberCategory::NegativeZero;
    }
    // 13. Else if x is not-a-number, then
    else if (number.is_nan()) {
        // a. Let category be positive-zero.
        category = NumberCategory::PositiveZero;
    }
    // 14. Else if x is positive-infinity, then
    else if (number.is_positive_infinity()) {
        // a. Let category be positive-nonzero.
        category = NumberCategory::PositiveNonZero;
    }
    // 15. Else,
    else {
        // a. Assert: x is a mathematical value.
        VERIFY(number.is_mathematical_value());

        // b. If x < 0, then
        if (number.is_negative()) {
            // i. Let category be negative-nonzero.
            category = NumberCategory::NegativeNonZero;
        }
        // c. Else if x > 0, then
        else if (number.is_positive()) {
            // i. Let category be positive-nonzero.
            category = NumberCategory::PositiveNonZero;
        }
        // d. Else,
        else {
            // i. Let category be positive-zero.
            category = NumberCategory::PositiveZero;
        }
    }

    StringView pattern;

    // 16. Let signDisplay be numberFormat.[[SignDisplay]].
    switch (number_format.sign_display()) {
    // 17. If signDisplay is "never", then
    case NumberFormat::SignDisplay::Never:
        // a. Let pattern be patterns.[[zeroPattern]].
        pattern = patterns->zero_format;
        break;

    // 18. Else if signDisplay is "auto", then
    case NumberFormat::SignDisplay::Auto:
        // a. If category is positive-nonzero or positive-zero, then
        if (category == NumberCategory::PositiveNonZero || category == NumberCategory::PositiveZero) {
            // i. Let pattern be patterns.[[zeroPattern]].
            pattern = patterns->zero_format;
        }
        // b. Else,
        else {
            // i. Let pattern be patterns.[[negativePattern]].
            pattern = patterns->negative_format;
        }
        break;

    // 19. Else if signDisplay is "always", then
    case NumberFormat::SignDisplay::Always:
        // a. If category is positive-nonzero or positive-zero, then
        if (category == NumberCategory::PositiveNonZero || category == NumberCategory::PositiveZero) {
            // i. Let pattern be patterns.[[positivePattern]].
            pattern = patterns->positive_format;
        }
        // b. Else,
        else {
            // i. Let pattern be patterns.[[negativePattern]].
            pattern = patterns->negative_format;
        }
        break;

    // 20. Else if signDisplay is "exceptZero", then
    case NumberFormat::SignDisplay::ExceptZero:
        // a. If category is positive-zero or negative-zero, then
        if (category == NumberCategory::PositiveZero || category == NumberCategory::NegativeZero) {
            // i. Let pattern be patterns.[[zeroPattern]].
            pattern = patterns->zero_format;
        }
        // b. Else if category is positive-nonzero, then
        else if (category == NumberCategory::PositiveNonZero) {
            // i. Let pattern be patterns.[[positivePattern]].
            pattern = patterns->positive_format;
        }
        // c. Else,
        else {
            // i. Let pattern be patterns.[[negativePattern]].
            pattern = patterns->negative_format;
        }
        break;

    // 21. Else,
    case NumberFormat::SignDisplay::Negative:
        // a. Assert: signDisplay is "negative".
        // b. If category is negative-nonzero, then
        if (category == NumberCategory::NegativeNonZero) {
            // i. Let pattern be patterns.[[negativePattern]].
            pattern = patterns->negative_format;
        }
        // c. Else,
        else {
            // i. Let pattern be patterns.[[zeroPattern]].
            pattern = patterns->zero_format;
        }
        break;

    default:
        VERIFY_NOT_REACHED();
    }

    found_pattern = patterns.release_value();

    // Handling of steps 9b/9g: Depending on the currency display and the format pattern found above,
    // we might need to mutate the format pattern to inject a space between the currency display and
    // the currency number.
    if (number_format.style() == NumberFormat::Style::Currency) {
        auto modified_pattern = ::Locale::augment_currency_format_pattern(number_format.resolve_currency_display(), pattern);
        if (modified_pattern.has_value())
            return modified_pattern.release_value();
    }

    // 22. Return pattern.
    return pattern;
}

// 15.5.12 GetNotationSubPattern ( numberFormat, exponent ), https://tc39.es/ecma402/#sec-getnotationsubpattern
Optional<StringView> get_notation_sub_pattern(NumberFormat& number_format, int exponent)
{
    // 1. Let localeData be %NumberFormat%.[[LocaleData]].
    // 2. Let dataLocale be numberFormat.[[DataLocale]].
    // 3. Let dataLocaleData be localeData.[[<dataLocale>]].
    // 4. Let notationSubPatterns be dataLocaleData.[[notationSubPatterns]].
    // 5. Assert: notationSubPatterns is a Record (see 15.3.3).

    // 6. Let notation be numberFormat.[[Notation]].
    auto notation = number_format.notation();

    // 7. If notation is "scientific" or notation is "engineering", then
    if ((notation == NumberFormat::Notation::Scientific) || (notation == NumberFormat::Notation::Engineering)) {
        // a. Return notationSubPatterns.[[scientific]].
        auto notation_sub_patterns = ::Locale::get_standard_number_system_format(number_format.data_locale(), number_format.numbering_system(), ::Locale::StandardNumberFormatType::Scientific);
        if (!notation_sub_patterns.has_value())
            return {};

        return notation_sub_patterns->zero_format;
    }
    // 8. Else if exponent is not 0, then
    else if (exponent != 0) {
        // a. Assert: notation is "compact".
        VERIFY(notation == NumberFormat::Notation::Compact);

        // b. Let compactDisplay be numberFormat.[[CompactDisplay]].
        // c. Let compactPatterns be notationSubPatterns.[[compact]].[[<compactDisplay>]].
        // d. Return compactPatterns.[[<exponent>]].
        if (number_format.has_compact_format())
            return number_format.compact_format().zero_format;
    }

    // 9. Else,
    //     a. Return "{number}".
    return "{number}"sv;
}

// 15.5.13 ComputeExponent ( numberFormat, x ), https://tc39.es/ecma402/#sec-computeexponent
int compute_exponent(NumberFormat& number_format, MathematicalValue number)
{
    // 1. If x = 0, then
    if (number.is_zero()) {
        // a. Return 0.
        return 0;
    }

    // 2. If x < 0, then
    if (number.is_negative()) {
        // a. Let x = -x.
        number.negate();
    }

    // 3. Let magnitude be the base 10 logarithm of x rounded down to the nearest integer.
    int magnitude = number.logarithmic_floor();

    // 4. Let exponent be ComputeExponentForMagnitude(numberFormat, magnitude).
    int exponent = compute_exponent_for_magnitude(number_format, magnitude);

    // 5. Let x be x × 10^(-exponent).
    number = number.multiplied_by_power(-exponent);

    // 6. Let formatNumberResult be FormatNumericToString(numberFormat, x).
    auto format_number_result = format_numeric_to_string(number_format, move(number));

    // 7. If formatNumberResult.[[RoundedNumber]] = 0, then
    if (format_number_result.rounded_number.is_zero()) {
        // a. Return exponent.
        return exponent;
    }

    // 8. Let newMagnitude be the base 10 logarithm of formatNumberResult.[[RoundedNumber]] rounded down to the nearest integer.
    int new_magnitude = format_number_result.rounded_number.logarithmic_floor();

    // 9. If newMagnitude is magnitude - exponent, then
    if (new_magnitude == magnitude - exponent) {
        // a. Return exponent.
        return exponent;
    }

    // 10. Return ComputeExponentForMagnitude(numberFormat, magnitude + 1).
    return compute_exponent_for_magnitude(number_format, magnitude + 1);
}

// 15.5.14 ComputeExponentForMagnitude ( numberFormat, magnitude ), https://tc39.es/ecma402/#sec-computeexponentformagnitude
int compute_exponent_for_magnitude(NumberFormat& number_format, int magnitude)
{
    // 1. Let notation be numberFormat.[[Notation]].
    switch (number_format.notation()) {
    // 2. If notation is "standard", then
    case NumberFormat::Notation::Standard:
        // a. Return 0.
        return 0;

    // 3. Else if notation is "scientific", then
    case NumberFormat::Notation::Scientific:
        // a. Return magnitude.
        return magnitude;

    // 4. Else if notation is "engineering", then
    case NumberFormat::Notation::Engineering: {
        // a. Let thousands be the greatest integer that is not greater than magnitude / 3.
        double thousands = floor(static_cast<double>(magnitude) / 3.0);

        // b. Return thousands × 3.
        return static_cast<int>(thousands) * 3;
    }

    // 5. Else,
    case NumberFormat::Notation::Compact: {
        // a. Assert: notation is "compact".
        VERIFY(number_format.has_compact_display());

        // b. Let exponent be an implementation- and locale-dependent (ILD) integer by which to scale a number of the given magnitude in compact notation for the current locale.
        // c. Return exponent.
        auto compact_format_type = number_format.compact_display() == NumberFormat::CompactDisplay::Short || number_format.style() == NumberFormat::Style::Currency
            ? ::Locale::CompactNumberFormatType::DecimalShort
            : ::Locale::CompactNumberFormatType::DecimalLong;

        auto format_rules = ::Locale::get_compact_number_system_formats(number_format.data_locale(), number_format.numbering_system(), compact_format_type);
        ::Locale::NumberFormat const* best_number_format = nullptr;

        for (auto const& format_rule : format_rules) {
            if (format_rule.magnitude > magnitude)
                break;
            best_number_format = &format_rule;
        }

        if (best_number_format == nullptr)
            return 0;

        number_format.set_compact_format(*best_number_format);
        return best_number_format->exponent;
    }

    default:
        VERIFY_NOT_REACHED();
    }
}

// 15.5.16 ToIntlMathematicalValue ( value ), https://tc39.es/ecma402/#sec-tointlmathematicalvalue
ThrowCompletionOr<MathematicalValue> to_intl_mathematical_value(VM& vm, Value value)
{
    // 1. Let primValue be ? ToPrimitive(value, number).
    auto primitive_value = TRY(value.to_primitive(vm, Value::PreferredType::Number));

    // 2. If Type(primValue) is BigInt, return the mathematical value of primValue.
    if (primitive_value.is_bigint())
        return primitive_value.as_bigint().big_integer();

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
    return mathematical_value;
}

// 15.5.17 GetUnsignedRoundingMode ( roundingMode, isNegative ), https://tc39.es/ecma402/#sec-getunsignedroundingmode
NumberFormat::UnsignedRoundingMode get_unsigned_rounding_mode(NumberFormat::RoundingMode rounding_mode, bool is_negative)
{
    // 1. If isNegative is true, return the specification type in the third column of Table 15 where the first column is roundingMode and the second column is "negative".
    // 2. Else, return the specification type in the third column of Table 15 where the first column is roundingMode and the second column is "positive".

    // Table 15: Conversion from rounding mode to unsigned rounding mode, https://tc39.es/ecma402/#table-intl-unsigned-rounding-modes
    switch (rounding_mode) {
    case NumberFormat::RoundingMode::Ceil:
        return is_negative ? NumberFormat::UnsignedRoundingMode::Zero : NumberFormat::UnsignedRoundingMode::Infinity;
    case NumberFormat::RoundingMode::Floor:
        return is_negative ? NumberFormat::UnsignedRoundingMode::Infinity : NumberFormat::UnsignedRoundingMode::Zero;
    case NumberFormat::RoundingMode::Expand:
        return NumberFormat::UnsignedRoundingMode::Infinity;
    case NumberFormat::RoundingMode::Trunc:
        return NumberFormat::UnsignedRoundingMode::Zero;
    case NumberFormat::RoundingMode::HalfCeil:
        return is_negative ? NumberFormat::UnsignedRoundingMode::HalfZero : NumberFormat::UnsignedRoundingMode::HalfInfinity;
    case NumberFormat::RoundingMode::HalfFloor:
        return is_negative ? NumberFormat::UnsignedRoundingMode::HalfInfinity : NumberFormat::UnsignedRoundingMode::HalfZero;
    case NumberFormat::RoundingMode::HalfExpand:
        return NumberFormat::UnsignedRoundingMode::HalfInfinity;
    case NumberFormat::RoundingMode::HalfTrunc:
        return NumberFormat::UnsignedRoundingMode::HalfZero;
    case NumberFormat::RoundingMode::HalfEven:
        return NumberFormat::UnsignedRoundingMode::HalfEven;
    default:
        VERIFY_NOT_REACHED();
    };
}

// 15.5.18 ApplyUnsignedRoundingMode ( x, r1, r2, unsignedRoundingMode ), https://tc39.es/ecma402/#sec-applyunsignedroundingmode
RoundingDecision apply_unsigned_rounding_mode(MathematicalValue const& x, MathematicalValue const& r1, MathematicalValue const& r2, NumberFormat::UnsignedRoundingMode unsigned_rounding_mode)
{
    // 1. If x is equal to r1, return r1.
    if (x.is_equal_to(r1))
        return RoundingDecision::LowerValue;

    // FIXME: We skip this assertion due floating point inaccuracies. For example, entering "1.2345"
    //        in the JS REPL results in "1.234499999999999", and may cause this assertion to fail.
    //
    //        This should be resolved when the "Intl mathematical value" is implemented to support
    //        arbitrarily precise decimals.
    //        https://tc39.es/ecma402/#intl-mathematical-value
    // 2. Assert: r1 < x < r2.

    // 3. Assert: unsignedRoundingMode is not undefined.

    // 4. If unsignedRoundingMode is zero, return r1.
    if (unsigned_rounding_mode == NumberFormat::UnsignedRoundingMode::Zero)
        return RoundingDecision::LowerValue;

    // 5. If unsignedRoundingMode is infinity, return r2.
    if (unsigned_rounding_mode == NumberFormat::UnsignedRoundingMode::Infinity)
        return RoundingDecision::HigherValue;

    // 6. Let d1 be x – r1.
    auto d1 = x.minus(r1);

    // 7. Let d2 be r2 – x.
    auto d2 = r2.minus(x);

    // 8. If d1 < d2, return r1.
    if (d1.is_less_than(d2))
        return RoundingDecision::LowerValue;

    // 9. If d2 < d1, return r2.
    if (d2.is_less_than(d1))
        return RoundingDecision::HigherValue;

    // 10. Assert: d1 is equal to d2.
    VERIFY(d1.is_equal_to(d2));

    // 11. If unsignedRoundingMode is half-zero, return r1.
    if (unsigned_rounding_mode == NumberFormat::UnsignedRoundingMode::HalfZero)
        return RoundingDecision::LowerValue;

    // 12. If unsignedRoundingMode is half-infinity, return r2.
    if (unsigned_rounding_mode == NumberFormat::UnsignedRoundingMode::HalfInfinity)
        return RoundingDecision::HigherValue;

    // 13. Assert: unsignedRoundingMode is half-even.
    VERIFY(unsigned_rounding_mode == NumberFormat::UnsignedRoundingMode::HalfEven);

    // 14. Let cardinality be (r1 / (r2 – r1)) modulo 2.
    auto cardinality = r1.divided_by(r2.minus(r1));

    // 15. If cardinality is 0, return r1.
    if (cardinality.modulo_is_zero(2))
        return RoundingDecision::LowerValue;

    // 16. Return r2.
    return RoundingDecision::HigherValue;
}

// 15.5.19 PartitionNumberRangePattern ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-partitionnumberrangepattern
ThrowCompletionOr<Vector<PatternPartitionWithSource>> partition_number_range_pattern(VM& vm, NumberFormat& number_format, MathematicalValue start, MathematicalValue end)
{
    // 1. If x is NaN or y is NaN, throw a RangeError exception.
    if (start.is_nan())
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "start"sv);
    if (end.is_nan())
        return vm.throw_completion<RangeError>(ErrorType::NumberIsNaN, "end"sv);

    // 2. Let result be a new empty List.
    Vector<PatternPartitionWithSource> result;

    // 3. Let xResult be ? PartitionNumberPattern(numberFormat, x).
    auto raw_start_result = partition_number_pattern(vm, number_format, move(start));
    auto start_result = PatternPartitionWithSource::create_from_parent_list(move(raw_start_result));

    // 4. Let yResult be ? PartitionNumberPattern(numberFormat, y).
    auto raw_end_result = partition_number_pattern(vm, number_format, move(end));
    auto end_result = PatternPartitionWithSource::create_from_parent_list(move(raw_end_result));

    // 5. If ! FormatNumeric(numberFormat, x) is equal to ! FormatNumeric(numberFormat, y), then
    auto formatted_start = format_numeric(vm, number_format, start);
    auto formatted_end = format_numeric(vm, number_format, end);

    if (formatted_start == formatted_end) {
        // a. Let appxResult be ? FormatApproximately(numberFormat, xResult).
        auto approximate_result = format_approximately(number_format, move(start_result));

        // b. For each r in appxResult, do
        for (auto& result : approximate_result) {
            // i. Set r.[[Source]] to "shared".
            result.source = "shared"sv;
        }

        // c. Return appxResult.
        return approximate_result;
    }

    // 6. For each element r in xResult, do
    result.ensure_capacity(start_result.size());

    for (auto& start_part : start_result) {
        // a. Append a new Record { [[Type]]: r.[[Type]], [[Value]]: r.[[Value]], [[Source]]: "startRange" } as the last element of result.
        PatternPartitionWithSource part;
        part.type = start_part.type;
        part.value = move(start_part.value);
        part.source = "startRange"sv;

        result.unchecked_append(move(part));
    }

    // 7. Let rangeSeparator be an ILND String value used to separate two numbers.
    auto range_separator_symbol = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::RangeSeparator).value_or("-"sv);
    auto range_separator = ::Locale::augment_range_pattern(range_separator_symbol, result.last().value, end_result[0].value);

    // 8. Append a new Record { [[Type]]: "literal", [[Value]]: rangeSeparator, [[Source]]: "shared" } element to result.
    PatternPartitionWithSource part;
    part.type = "literal"sv;
    part.value = range_separator.has_value()
        ? range_separator.release_value()
        : MUST(String::from_utf8(range_separator_symbol));
    part.source = "shared"sv;
    result.append(move(part));

    // 9. For each element r in yResult, do
    result.ensure_capacity(result.size() + end_result.size());

    for (auto& end_part : end_result) {
        // a. Append a new Record { [[Type]]: r.[[Type]], [[Value]]: r.[[Value]], [[Source]]: "endRange" } as the last element of result.
        PatternPartitionWithSource part;
        part.type = end_part.type;
        part.value = move(end_part.value);
        part.source = "endRange"sv;

        result.unchecked_append(move(part));
    }

    // 10. Return ! CollapseNumberRange(result).
    return collapse_number_range(move(result));
}

// 15.5.20 FormatApproximately ( numberFormat, result ), https://tc39.es/ecma402/#sec-formatapproximately
Vector<PatternPartitionWithSource> format_approximately(NumberFormat& number_format, Vector<PatternPartitionWithSource> result)
{
    // 1. Let approximatelySign be an ILND String value used to signify that a number is approximate.
    auto approximately_sign = ::Locale::get_number_system_symbol(number_format.data_locale(), number_format.numbering_system(), ::Locale::NumericSymbol::ApproximatelySign);

    // 2. If approximatelySign is not empty, insert a new Record { [[Type]]: "approximatelySign", [[Value]]: approximatelySign } at an ILND index in result. For example, if numberFormat has [[Locale]] "en-US" and [[NumberingSystem]] "latn" and [[Style]] "decimal", the new Record might be inserted before the first element of result.
    if (approximately_sign.has_value() && !approximately_sign->is_empty()) {
        PatternPartitionWithSource partition;
        partition.type = "approximatelySign"sv;
        partition.value = MUST(String::from_utf8(*approximately_sign));

        result.insert_before_matching(move(partition), [](auto const& part) {
            return part.type.is_one_of("integer"sv, "decimal"sv, "plusSign"sv, "minusSign"sv, "percentSign"sv, "currency"sv);
        });
    }

    // 3. Return result.
    return result;
}

// 15.5.21 CollapseNumberRange ( result ), https://tc39.es/ecma402/#sec-collapsenumberrange
Vector<PatternPartitionWithSource> collapse_number_range(Vector<PatternPartitionWithSource> result)
{
    // Returning result unmodified is guaranteed to be a correct implementation of CollapseNumberRange.
    return result;
}

// 15.5.22 FormatNumericRange ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-formatnumericrange
ThrowCompletionOr<String> format_numeric_range(VM& vm, NumberFormat& number_format, MathematicalValue start, MathematicalValue end)
{
    // 1. Let parts be ? PartitionNumberRangePattern(numberFormat, x, y).
    auto parts = TRY(partition_number_range_pattern(vm, number_format, move(start), move(end)));

    // 2. Let result be the empty String.
    StringBuilder result;

    // 3. For each part in parts, do
    for (auto& part : parts) {
        // a. Set result to the string-concatenation of result and part.[[Value]].
        result.append(part.value);
    }

    // 4. Return result.
    return MUST(result.to_string());
}

// 15.5.23 FormatNumericRangeToParts ( numberFormat, x, y ), https://tc39.es/ecma402/#sec-formatnumericrangetoparts
ThrowCompletionOr<NonnullGCPtr<Array>> format_numeric_range_to_parts(VM& vm, NumberFormat& number_format, MathematicalValue start, MathematicalValue end)
{
    auto& realm = *vm.current_realm();

    // 1. Let parts be ? PartitionNumberRangePattern(numberFormat, x, y).
    auto parts = TRY(partition_number_range_pattern(vm, number_format, move(start), move(end)));

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
