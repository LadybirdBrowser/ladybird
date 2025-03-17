/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <AK/QuickSort.h>
#include <AK/Utf8View.h>
#include <LibUnicode/ICU.h>
#include <LibUnicode/Locale.h>
#include <LibUnicode/NumberFormat.h>
#include <LibUnicode/PartitionRange.h>
#include <math.h>

#include <unicode/numberformatter.h>
#include <unicode/numberrangeformatter.h>
#include <unicode/plurrule.h>

namespace Unicode {

NumberFormatStyle number_format_style_from_string(StringView number_format_style)
{
    if (number_format_style == "decimal"sv)
        return NumberFormatStyle::Decimal;
    if (number_format_style == "percent"sv)
        return NumberFormatStyle::Percent;
    if (number_format_style == "currency"sv)
        return NumberFormatStyle::Currency;
    if (number_format_style == "unit"sv)
        return NumberFormatStyle::Unit;
    VERIFY_NOT_REACHED();
}

StringView number_format_style_to_string(NumberFormatStyle number_format_style)
{
    switch (number_format_style) {
    case NumberFormatStyle::Decimal:
        return "decimal"sv;
    case NumberFormatStyle::Percent:
        return "percent"sv;
    case NumberFormatStyle::Currency:
        return "currency"sv;
    case NumberFormatStyle::Unit:
        return "unit"sv;
    }
    VERIFY_NOT_REACHED();
}

SignDisplay sign_display_from_string(StringView sign_display)
{
    if (sign_display == "auto"sv)
        return SignDisplay::Auto;
    if (sign_display == "never"sv)
        return SignDisplay::Never;
    if (sign_display == "always"sv)
        return SignDisplay::Always;
    if (sign_display == "exceptZero"sv)
        return SignDisplay::ExceptZero;
    if (sign_display == "negative"sv)
        return SignDisplay::Negative;
    VERIFY_NOT_REACHED();
}

StringView sign_display_to_string(SignDisplay sign_display)
{
    switch (sign_display) {
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
    }
    VERIFY_NOT_REACHED();
}

static constexpr UNumberSignDisplay icu_sign_display(SignDisplay sign_display, Optional<CurrencySign> const& currency_sign)
{
    switch (sign_display) {
    case SignDisplay::Auto:
        return currency_sign == CurrencySign::Standard ? UNUM_SIGN_AUTO : UNUM_SIGN_ACCOUNTING;
    case SignDisplay::Never:
        return UNUM_SIGN_NEVER;
    case SignDisplay::Always:
        return currency_sign == CurrencySign::Standard ? UNUM_SIGN_ALWAYS : UNUM_SIGN_ACCOUNTING_ALWAYS;
    case SignDisplay::ExceptZero:
        return currency_sign == CurrencySign::Standard ? UNUM_SIGN_EXCEPT_ZERO : UNUM_SIGN_ACCOUNTING_EXCEPT_ZERO;
    case SignDisplay::Negative:
        return currency_sign == CurrencySign::Standard ? UNUM_SIGN_NEGATIVE : UNUM_SIGN_ACCOUNTING_NEGATIVE;
    }
    VERIFY_NOT_REACHED();
}

Notation notation_from_string(StringView notation)
{
    if (notation == "standard"sv)
        return Notation::Standard;
    if (notation == "scientific"sv)
        return Notation::Scientific;
    if (notation == "engineering"sv)
        return Notation::Engineering;
    if (notation == "compact"sv)
        return Notation::Compact;
    VERIFY_NOT_REACHED();
}

StringView notation_to_string(Notation notation)
{
    switch (notation) {
    case Notation::Standard:
        return "standard"sv;
    case Notation::Scientific:
        return "scientific"sv;
    case Notation::Engineering:
        return "engineering"sv;
    case Notation::Compact:
        return "compact"sv;
    }
    VERIFY_NOT_REACHED();
}

static icu::number::Notation icu_notation(Notation notation, Optional<CompactDisplay> const& compact_display)
{
    switch (notation) {
    case Notation::Standard:
        return icu::number::Notation::simple();
    case Notation::Scientific:
        return icu::number::Notation::scientific();
    case Notation::Engineering:
        return icu::number::Notation::engineering();
    case Notation::Compact:
        switch (*compact_display) {
        case CompactDisplay::Short:
            return icu::number::Notation::compactShort();
        case CompactDisplay::Long:
            return icu::number::Notation::compactLong();
        }
    }
    VERIFY_NOT_REACHED();
}

CompactDisplay compact_display_from_string(StringView compact_display)
{
    if (compact_display == "short"sv)
        return CompactDisplay::Short;
    if (compact_display == "long"sv)
        return CompactDisplay::Long;
    VERIFY_NOT_REACHED();
}

StringView compact_display_to_string(CompactDisplay compact_display)
{
    switch (compact_display) {
    case CompactDisplay::Short:
        return "short"sv;
    case CompactDisplay::Long:
        return "long"sv;
    }
    VERIFY_NOT_REACHED();
}

Grouping grouping_from_string(StringView grouping)
{
    if (grouping == "always"sv)
        return Grouping::Always;
    if (grouping == "auto"sv)
        return Grouping::Auto;
    if (grouping == "min2"sv)
        return Grouping::Min2;
    if (grouping == "false"sv)
        return Grouping::False;
    VERIFY_NOT_REACHED();
}

StringView grouping_to_string(Grouping grouping)
{
    switch (grouping) {
    case Grouping::Always:
        return "always"sv;
    case Grouping::Auto:
        return "auto"sv;
    case Grouping::Min2:
        return "min2"sv;
    case Grouping::False:
        return "false"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UNumberGroupingStrategy icu_grouping_strategy(Grouping grouping)
{
    switch (grouping) {
    case Grouping::Always:
        return UNUM_GROUPING_ON_ALIGNED;
    case Grouping::Auto:
        return UNUM_GROUPING_AUTO;
    case Grouping::Min2:
        return UNUM_GROUPING_MIN2;
    case Grouping::False:
        return UNUM_GROUPING_OFF;
    }
    VERIFY_NOT_REACHED();
}

CurrencyDisplay currency_display_from_string(StringView currency_display)
{
    if (currency_display == "code"sv)
        return CurrencyDisplay::Code;
    if (currency_display == "symbol"sv)
        return CurrencyDisplay::Symbol;
    if (currency_display == "narrowSymbol"sv)
        return CurrencyDisplay::NarrowSymbol;
    if (currency_display == "name"sv)
        return CurrencyDisplay::Name;
    VERIFY_NOT_REACHED();
}

StringView currency_display_to_string(CurrencyDisplay currency_display)
{
    switch (currency_display) {
    case CurrencyDisplay::Code:
        return "code"sv;
    case CurrencyDisplay::Symbol:
        return "symbol"sv;
    case CurrencyDisplay::NarrowSymbol:
        return "narrowSymbol"sv;
    case CurrencyDisplay::Name:
        return "name"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UNumberUnitWidth icu_currency_display(CurrencyDisplay currency_display)
{
    switch (currency_display) {
    case CurrencyDisplay::Code:
        return UNUM_UNIT_WIDTH_ISO_CODE;
    case CurrencyDisplay::Symbol:
        return UNUM_UNIT_WIDTH_SHORT;
    case CurrencyDisplay::NarrowSymbol:
        return UNUM_UNIT_WIDTH_NARROW;
    case CurrencyDisplay::Name:
        return UNUM_UNIT_WIDTH_FULL_NAME;
    }
    VERIFY_NOT_REACHED();
}

CurrencySign currency_sign_from_string(StringView currency_sign)
{
    if (currency_sign == "standard"sv)
        return CurrencySign::Standard;
    if (currency_sign == "accounting"sv)
        return CurrencySign::Accounting;
    VERIFY_NOT_REACHED();
}

StringView currency_sign_to_string(CurrencySign currency_sign)
{
    switch (currency_sign) {
    case CurrencySign::Standard:
        return "standard"sv;
    case CurrencySign::Accounting:
        return "accounting"sv;
    }
    VERIFY_NOT_REACHED();
}

RoundingType rounding_type_from_string(StringView rounding_type)
{
    if (rounding_type == "significantDigits"sv)
        return RoundingType::SignificantDigits;
    if (rounding_type == "fractionDigits"sv)
        return RoundingType::FractionDigits;
    if (rounding_type == "morePrecision"sv)
        return RoundingType::MorePrecision;
    if (rounding_type == "lessPrecision"sv)
        return RoundingType::LessPrecision;
    VERIFY_NOT_REACHED();
}

StringView rounding_type_to_string(RoundingType rounding_type)
{
    switch (rounding_type) {
    case RoundingType::SignificantDigits:
        return "significantDigits"sv;
    case RoundingType::FractionDigits:
        return "fractionDigits"sv;
    case RoundingType::MorePrecision:
        return "morePrecision"sv;
    case RoundingType::LessPrecision:
        return "lessPrecision"sv;
    }
    VERIFY_NOT_REACHED();
}

RoundingMode rounding_mode_from_string(StringView rounding_mode)
{
    if (rounding_mode == "ceil"sv)
        return RoundingMode::Ceil;
    if (rounding_mode == "expand"sv)
        return RoundingMode::Expand;
    if (rounding_mode == "floor"sv)
        return RoundingMode::Floor;
    if (rounding_mode == "halfCeil"sv)
        return RoundingMode::HalfCeil;
    if (rounding_mode == "halfEven"sv)
        return RoundingMode::HalfEven;
    if (rounding_mode == "halfExpand"sv)
        return RoundingMode::HalfExpand;
    if (rounding_mode == "halfFloor"sv)
        return RoundingMode::HalfFloor;
    if (rounding_mode == "halfTrunc"sv)
        return RoundingMode::HalfTrunc;
    if (rounding_mode == "trunc"sv)
        return RoundingMode::Trunc;
    VERIFY_NOT_REACHED();
}

StringView rounding_mode_to_string(RoundingMode rounding_mode)
{
    switch (rounding_mode) {
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
    }
    VERIFY_NOT_REACHED();
}

static constexpr UNumberFormatRoundingMode icu_rounding_mode(RoundingMode rounding_mode)
{
    switch (rounding_mode) {
    case RoundingMode::Ceil:
        return UNUM_ROUND_CEILING;
    case RoundingMode::Expand:
        return UNUM_ROUND_UP;
    case RoundingMode::Floor:
        return UNUM_ROUND_FLOOR;
    case RoundingMode::HalfCeil:
        return UNUM_ROUND_HALF_CEILING;
    case RoundingMode::HalfEven:
        return UNUM_ROUND_HALFEVEN;
    case RoundingMode::HalfExpand:
        return UNUM_ROUND_HALFUP;
    case RoundingMode::HalfFloor:
        return UNUM_ROUND_HALF_FLOOR;
    case RoundingMode::HalfTrunc:
        return UNUM_ROUND_HALFDOWN;
    case RoundingMode::Trunc:
        return UNUM_ROUND_DOWN;
    }
    VERIFY_NOT_REACHED();
}

TrailingZeroDisplay trailing_zero_display_from_string(StringView trailing_zero_display)
{
    if (trailing_zero_display == "auto"sv)
        return TrailingZeroDisplay::Auto;
    if (trailing_zero_display == "stripIfInteger"sv)
        return TrailingZeroDisplay::StripIfInteger;
    VERIFY_NOT_REACHED();
}

StringView trailing_zero_display_to_string(TrailingZeroDisplay trailing_zero_display)
{
    switch (trailing_zero_display) {
    case TrailingZeroDisplay::Auto:
        return "auto"sv;
    case TrailingZeroDisplay::StripIfInteger:
        return "stripIfInteger"sv;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UNumberTrailingZeroDisplay icu_trailing_zero_display(TrailingZeroDisplay trailing_zero_display)
{
    switch (trailing_zero_display) {
    case TrailingZeroDisplay::Auto:
        return UNUM_TRAILING_ZERO_AUTO;
    case TrailingZeroDisplay::StripIfInteger:
        return UNUM_TRAILING_ZERO_HIDE_IF_WHOLE;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UNumberUnitWidth icu_unit_width(Style unit_display)
{
    switch (unit_display) {
    case Style::Long:
        return UNUM_UNIT_WIDTH_FULL_NAME;
    case Style::Short:
        return UNUM_UNIT_WIDTH_SHORT;
    case Style::Narrow:
        return UNUM_UNIT_WIDTH_NARROW;
    }
    VERIFY_NOT_REACHED();
}

static constexpr UPluralType icu_plural_type(PluralForm plural_form)
{
    switch (plural_form) {
    case PluralForm::Cardinal:
        return UPluralType::UPLURAL_TYPE_CARDINAL;
    case PluralForm::Ordinal:
        return UPluralType::UPLURAL_TYPE_ORDINAL;
    }
    VERIFY_NOT_REACHED();
}

static void apply_display_options(icu::number::LocalizedNumberFormatter& formatter, DisplayOptions const& display_options)
{
    UErrorCode status = U_ZERO_ERROR;

    switch (display_options.style) {
    case NumberFormatStyle::Decimal:
        break;

    case NumberFormatStyle::Percent:
        formatter = formatter.unit(icu::MeasureUnit::getPercent()).scale(icu::number::Scale::byDouble(100));
        break;

    case NumberFormatStyle::Currency:
        formatter = formatter.unit(icu::CurrencyUnit(icu_string_piece(*display_options.currency), status));
        formatter = formatter.unitWidth(icu_currency_display(*display_options.currency_display));
        VERIFY(icu_success(status));
        break;

    case NumberFormatStyle::Unit:
        formatter = formatter.unit(icu::MeasureUnit::forIdentifier(icu_string_piece(*display_options.unit), status));
        formatter = formatter.unitWidth(icu_unit_width(*display_options.unit_display));
        VERIFY(icu_success(status));
        break;
    }

    formatter = formatter.sign(icu_sign_display(display_options.sign_display, display_options.currency_sign));
    formatter = formatter.notation(icu_notation(display_options.notation, display_options.compact_display));
    formatter = formatter.grouping(icu_grouping_strategy(display_options.grouping));
}

static void apply_rounding_options(icu::number::LocalizedNumberFormatter& formatter, RoundingOptions const& rounding_options)
{
    auto precision = icu::number::Precision::unlimited();

    if (rounding_options.rounding_increment == 1) {
        switch (rounding_options.type) {
        case RoundingType::SignificantDigits:
            precision = icu::number::Precision::minMaxSignificantDigits(*rounding_options.min_significant_digits, *rounding_options.max_significant_digits);
            break;
        case RoundingType::FractionDigits:
            precision = icu::number::Precision::minMaxFraction(*rounding_options.min_fraction_digits, *rounding_options.max_fraction_digits);
            break;
        case RoundingType::MorePrecision:
            precision = icu::number::Precision::minMaxFraction(*rounding_options.min_fraction_digits, *rounding_options.max_fraction_digits)
                            .withSignificantDigits(*rounding_options.min_significant_digits, *rounding_options.max_significant_digits, UNUM_ROUNDING_PRIORITY_RELAXED);
            break;
        case RoundingType::LessPrecision:
            precision = icu::number::Precision::minMaxFraction(*rounding_options.min_fraction_digits, *rounding_options.max_fraction_digits)
                            .withSignificantDigits(*rounding_options.min_significant_digits, *rounding_options.max_significant_digits, UNUM_ROUNDING_PRIORITY_STRICT);
            break;
        }
    } else {
        auto mantissa = rounding_options.rounding_increment;
        auto magnitude = *rounding_options.max_fraction_digits * -1;

        precision = icu::number::Precision::incrementExact(mantissa, static_cast<i16>(magnitude))
                        .withMinFraction(*rounding_options.min_fraction_digits);
    }

    formatter = formatter.precision(precision.trailingZeroDisplay(icu_trailing_zero_display(rounding_options.trailing_zero_display)));
    formatter = formatter.integerWidth(icu::number::IntegerWidth::zeroFillTo(rounding_options.min_integer_digits));
    formatter = formatter.roundingMode(icu_rounding_mode(rounding_options.mode));
}

static constexpr StringView icu_number_format_field_to_string(i32 field, NumberFormat::Value const& value, bool is_unit)
{
    switch (field) {
    case PartitionRange::LITERAL_FIELD:
        return "literal"sv;
    case UNUM_INTEGER_FIELD:
        if (auto const* number = value.get_pointer<double>()) {
            if (isnan(*number))
                return "nan"sv;
            if (isinf(*number))
                return "infinity"sv;
        }
        return "integer"sv;
    case UNUM_FRACTION_FIELD:
        return "fraction"sv;
    case UNUM_DECIMAL_SEPARATOR_FIELD:
        return "decimal"sv;
    case UNUM_EXPONENT_SYMBOL_FIELD:
        return "exponentSeparator"sv;
    case UNUM_EXPONENT_SIGN_FIELD:
        return "exponentMinusSign"sv;
    case UNUM_EXPONENT_FIELD:
        return "exponentInteger"sv;
    case UNUM_GROUPING_SEPARATOR_FIELD:
        return "group"sv;
    case UNUM_CURRENCY_FIELD:
        return "currency"sv;
    case UNUM_PERCENT_FIELD:
        return is_unit ? "unit"sv : "percentSign"sv;
    case UNUM_SIGN_FIELD: {
        auto is_negative = value.visit(
            [&](double number) { return signbit(number); },
            [&](String const& number) { return number.starts_with('-'); });
        return is_negative ? "minusSign"sv : "plusSign"sv;
    }
    case UNUM_MEASURE_UNIT_FIELD:
        return "unit"sv;
    case UNUM_COMPACT_FIELD:
        return "compact"sv;
    case UNUM_APPROXIMATELY_SIGN_FIELD:
        return "approximatelySign"sv;
    }

    VERIFY_NOT_REACHED();
}

// ICU will give us overlapping partitions, e.g. for the formatted result "1,234", we will get the following parts:
//
//     part=","     type=group    start=1  end=2
//     part="1,234" type=integer  start=0  end=5
//
// We need to massage these partitions into non-overlapping parts for ECMA-402:
//
//     part="1"     type=integer  start=0  end=1
//     part=","     type=group    start=1  end=2
//     part="234"   type=integer  start=2  end=5
static void flatten_partitions(Vector<PartitionRange>& partitions)
{
    if (partitions.size() <= 1)
        return;

    quick_sort(partitions);

    auto subtract_range = [&](auto const& first, auto const& second) -> Vector<PartitionRange> {
        if (second.start > first.end || first.start > second.end)
            return { first };

        Vector<PartitionRange> result;

        if (second.start > first.start)
            result.empend(first.field, first.start, second.start);
        if (second.end < first.end)
            result.empend(first.field, second.end, first.end);

        return result;
    };

    for (size_t i = 0; i < partitions.size(); ++i) {
        for (size_t j = i + 1; j < partitions.size(); ++j) {
            auto& first = partitions[i];
            auto& second = partitions[j];

            auto result = subtract_range(first, second);

            if (result.is_empty()) {
                partitions.remove(i);
                --i;
                break;
            }

            first = result[0];

            if (result.size() == 2)
                partitions.insert(i + 1, result[1]);
        }
    }

    quick_sort(partitions);
}

class NumberFormatImpl : public NumberFormat {
public:
    NumberFormatImpl(icu::Locale& locale, icu::number::LocalizedNumberFormatter formatter, bool is_unit)
        : m_locale(locale)
        , m_formatter(move(formatter))
        , m_is_unit(is_unit)
    {
    }

    virtual ~NumberFormatImpl() override = default;

    virtual String format(Value const& value) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = format_impl(value);
        if (!formatted.has_value())
            return {};

        auto result = formatted->toTempString(status);
        if (icu_failure(status))
            return {};

        return icu_string_to_string(result);
    }

    virtual String format_to_decimal(Value const& value) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = format_impl(value);
        if (!formatted.has_value())
            return {};

        auto result = formatted->toDecimalNumber<StringBuilder>(status);
        if (icu_failure(status))
            return {};

        return MUST(result.to_string());
    }

    virtual Vector<Partition> format_to_parts(Value const& value) const override
    {
        auto formatted = format_impl(value);
        if (!formatted.has_value())
            return {};

        return format_to_parts_impl(formatted, value, value);
    }

    virtual String format_range(Value const& start, Value const& end) const override
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = format_range_impl(start, end);
        if (!formatted.has_value())
            return {};

        auto result = formatted->toTempString(status);
        if (icu_failure(status))
            return {};

        return icu_string_to_string(result);
    }

    virtual Vector<Partition> format_range_to_parts(Value const& start, Value const& end) const override
    {
        auto formatted = format_range_impl(start, end);
        if (!formatted.has_value())
            return {};

        return format_to_parts_impl(formatted, start, end);
    }

    virtual void create_plural_rules(PluralForm plural_form) override
    {
        UErrorCode status = U_ZERO_ERROR;
        VERIFY(!m_plural_rules);

        m_plural_rules = adopt_own(*icu::PluralRules::forLocale(m_locale, icu_plural_type(plural_form), status));
        VERIFY(icu_success(status));
    }

    virtual PluralCategory select_plural(double value) const override
    {
        UErrorCode status = U_ZERO_ERROR;
        VERIFY(m_plural_rules);

        auto formatted = format_impl(value);
        if (!formatted.has_value())
            return PluralCategory::Other;

        auto result = m_plural_rules->select(*formatted, status);
        if (icu_failure(status))
            return PluralCategory::Other;

        return plural_category_from_string(icu_string_to_string(result));
    }

    virtual PluralCategory select_plural_range(double start, double end) const override
    {
        UErrorCode status = U_ZERO_ERROR;
        VERIFY(m_plural_rules);

        auto formatted = format_range_impl(start, end);
        if (!formatted.has_value())
            return PluralCategory::Other;

        auto [formatted_start, formatted_end] = formatted->getDecimalNumbers<StringBuilder>(status);
        if (icu_failure(status))
            return PluralCategory::Other;

        if (formatted_start.string_view() == formatted_end.string_view())
            return select_plural(start);

        auto result = m_plural_rules->select(*formatted, status);
        if (icu_failure(status))
            return PluralCategory::Other;

        return plural_category_from_string(icu_string_to_string(result));
    }

    virtual Vector<PluralCategory> available_plural_categories() const override
    {
        UErrorCode status = U_ZERO_ERROR;
        VERIFY(m_plural_rules);

        auto keywords = adopt_own_if_nonnull(m_plural_rules->getKeywords(status));
        if (icu_failure(status))
            return {};

        Vector<PluralCategory> result;

        while (true) {
            i32 length = 0;
            auto const* category = keywords->next(&length, status);

            if (icu_failure(status) || category == nullptr)
                break;

            result.append(plural_category_from_string({ category, static_cast<size_t>(length) }));
        }

        quick_sort(result);
        return result;
    }

private:
    static icu::Formattable value_to_formattable(Value const& value)
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formattable = value.visit(
            [&](double number) { return icu::Formattable { number }; },
            [&](String const& number) { return icu::Formattable(icu_string_piece(number), status); });
        VERIFY(icu_success(status));

        return formattable;
    }

    Optional<icu::number::FormattedNumber> format_impl(Value const& value) const
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted = value.visit(
            [&](double number) {
                return m_formatter.formatDouble(number, status);
            },
            [&](String const& number) {
                return m_formatter.formatDecimal(icu_string_piece(number), status);
            });

        if (icu_failure(status))
            return {};

        return formatted;
    }

    Optional<icu::number::FormattedNumberRange> format_range_impl(Value const& start, Value const& end) const
    {
        UErrorCode status = U_ZERO_ERROR;

        if (!m_range_formatter.has_value()) {
            auto skeleton = icu::number::NumberFormatter::forSkeleton(m_formatter.toSkeleton(status), status);
            if (icu_failure(status))
                return {};

            auto formatter = icu::number::UnlocalizedNumberRangeFormatter().numberFormatterBoth(move(skeleton)).locale(m_locale);
            if (icu_failure(status))
                return {};

            m_range_formatter = move(formatter);
        }

        auto formattable_start = value_to_formattable(start);
        auto formattable_end = value_to_formattable(end);

        auto formatted = m_range_formatter->formatFormattableRange(formattable_start, formattable_end, status);
        if (icu_failure(status))
            return {};

        return formatted;
    }

    template<typename Formatted>
    Vector<Partition> format_to_parts_impl(Formatted const& formatted, Value const& start, Value const& end) const
    {
        UErrorCode status = U_ZERO_ERROR;

        auto formatted_number = formatted->toTempString(status);
        if (icu_failure(status))
            return {};

        Vector<PartitionRange> ranges;
        ranges.empend(PartitionRange::LITERAL_FIELD, 0, formatted_number.length());

        icu::ConstrainedFieldPosition position;
        Optional<PartitionRange> start_range;
        Optional<PartitionRange> end_range;

        while (static_cast<bool>(formatted->nextPosition(position, status)) && icu_success(status)) {
            if (position.getCategory() == UFIELD_CATEGORY_NUMBER_RANGE_SPAN) {
                auto& range = position.getField() == 0 ? start_range : end_range;
                range = PartitionRange { position.getField(), position.getStart(), position.getLimit() };
            } else {
                ranges.empend(position.getField(), position.getStart(), position.getLimit());
            }
        }

        flatten_partitions(ranges);

        auto apply_to_partition = [&](Partition& partition, auto field, auto index) {
            if (start_range.has_value() && start_range->contains(index)) {
                partition.type = icu_number_format_field_to_string(field, start, m_is_unit);
                partition.source = "startRange"sv;
                return;
            }

            if (end_range.has_value() && end_range->contains(index)) {
                partition.type = icu_number_format_field_to_string(field, end, m_is_unit);
                partition.source = "endRange"sv;
                return;
            }

            partition.type = icu_number_format_field_to_string(field, end, m_is_unit);
            partition.source = "shared"sv;
        };

        Vector<Partition> result;
        result.ensure_capacity(ranges.size());

        for (auto const& range : ranges) {
            auto value = formatted_number.tempSubStringBetween(range.start, range.end);

            Partition partition;
            partition.value = icu_string_to_string(value);
            apply_to_partition(partition, range.field, range.start);

            result.unchecked_append(move(partition));
        }

        return result;
    }

    icu::Locale& m_locale;

    icu::number::LocalizedNumberFormatter m_formatter;
    mutable Optional<icu::number::LocalizedNumberRangeFormatter> m_range_formatter;

    OwnPtr<icu::PluralRules> m_plural_rules;

    bool m_is_unit { false };
};

NonnullOwnPtr<NumberFormat> NumberFormat::create(
    StringView locale,
    DisplayOptions const& display_options,
    RoundingOptions const& rounding_options)
{
    auto locale_data = LocaleData::for_locale(locale);
    VERIFY(locale_data.has_value());

    auto formatter = icu::number::NumberFormatter::withLocale(locale_data->locale());
    apply_display_options(formatter, display_options);
    apply_rounding_options(formatter, rounding_options);

    bool is_unit = display_options.style == NumberFormatStyle::Unit;
    return adopt_own(*new NumberFormatImpl(locale_data->locale(), move(formatter), is_unit));
}

}
