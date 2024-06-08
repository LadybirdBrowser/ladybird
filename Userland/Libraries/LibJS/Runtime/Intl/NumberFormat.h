/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Intl/MathematicalValue.h>
#include <LibJS/Runtime/Object.h>
#include <LibLocale/Locale.h>
#include <LibLocale/NumberFormat.h>

namespace JS::Intl {

class NumberFormatBase : public Object {
    JS_OBJECT(NumberFormatBase, Object);
    JS_DECLARE_ALLOCATOR(NumberFormatBase);

public:
    enum class RoundingType {
        Invalid,
        SignificantDigits,
        FractionDigits,
        MorePrecision,
        LessPrecision,
    };

    enum class ComputedRoundingPriority {
        Invalid,
        Auto,
        MorePrecision,
        LessPrecision,
    };

    enum class RoundingMode {
        Invalid,
        Ceil,
        Expand,
        Floor,
        HalfCeil,
        HalfEven,
        HalfExpand,
        HalfFloor,
        HalfTrunc,
        Trunc,
    };

    enum class UnsignedRoundingMode {
        HalfEven,
        HalfInfinity,
        HalfZero,
        Infinity,
        Zero,
    };

    enum class TrailingZeroDisplay {
        Invalid,
        Auto,
        StripIfInteger,
    };

    virtual ~NumberFormatBase() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    String const& data_locale() const { return m_data_locale; }
    void set_data_locale(String data_locale) { m_data_locale = move(data_locale); }

    int min_integer_digits() const { return m_min_integer_digits; }
    void set_min_integer_digits(int min_integer_digits) { m_min_integer_digits = min_integer_digits; }

    bool has_min_fraction_digits() const { return m_min_fraction_digits.has_value(); }
    int min_fraction_digits() const { return *m_min_fraction_digits; }
    void set_min_fraction_digits(int min_fraction_digits) { m_min_fraction_digits = min_fraction_digits; }

    bool has_max_fraction_digits() const { return m_max_fraction_digits.has_value(); }
    int max_fraction_digits() const { return *m_max_fraction_digits; }
    void set_max_fraction_digits(int max_fraction_digits) { m_max_fraction_digits = max_fraction_digits; }

    bool has_min_significant_digits() const { return m_min_significant_digits.has_value(); }
    int min_significant_digits() const { return *m_min_significant_digits; }
    void set_min_significant_digits(int min_significant_digits) { m_min_significant_digits = min_significant_digits; }

    bool has_max_significant_digits() const { return m_max_significant_digits.has_value(); }
    int max_significant_digits() const { return *m_max_significant_digits; }
    void set_max_significant_digits(int max_significant_digits) { m_max_significant_digits = max_significant_digits; }

    RoundingType rounding_type() const { return m_rounding_type; }
    StringView rounding_type_string() const;
    void set_rounding_type(RoundingType rounding_type) { m_rounding_type = rounding_type; }

    ComputedRoundingPriority computed_rounding_priority() const { return m_computed_rounding_priority; }
    StringView computed_rounding_priority_string() const;
    void set_computed_rounding_priority(ComputedRoundingPriority computed_rounding_priority) { m_computed_rounding_priority = computed_rounding_priority; }

    RoundingMode rounding_mode() const { return m_rounding_mode; }
    StringView rounding_mode_string() const;
    void set_rounding_mode(StringView rounding_mode);

    int rounding_increment() const { return m_rounding_increment; }
    void set_rounding_increment(int rounding_increment) { m_rounding_increment = rounding_increment; }

    TrailingZeroDisplay trailing_zero_display() const { return m_trailing_zero_display; }
    StringView trailing_zero_display_string() const;
    void set_trailing_zero_display(StringView trailing_zero_display);

protected:
    explicit NumberFormatBase(Object& prototype);

private:
    String m_locale;                                                                             // [[Locale]]
    String m_data_locale;                                                                        // [[DataLocale]]
    int m_min_integer_digits { 0 };                                                              // [[MinimumIntegerDigits]]
    Optional<int> m_min_fraction_digits {};                                                      // [[MinimumFractionDigits]]
    Optional<int> m_max_fraction_digits {};                                                      // [[MaximumFractionDigits]]
    Optional<int> m_min_significant_digits {};                                                   // [[MinimumSignificantDigits]]
    Optional<int> m_max_significant_digits {};                                                   // [[MaximumSignificantDigits]]
    RoundingType m_rounding_type { RoundingType::Invalid };                                      // [[RoundingType]]
    ComputedRoundingPriority m_computed_rounding_priority { ComputedRoundingPriority::Invalid }; // [[ComputedRoundingPriority]]
    RoundingMode m_rounding_mode { RoundingMode::Invalid };                                      // [[RoundingMode]]
    int m_rounding_increment { 1 };                                                              // [[RoundingIncrement]]
    TrailingZeroDisplay m_trailing_zero_display { TrailingZeroDisplay::Invalid };                // [[TrailingZeroDisplay]]
};

class NumberFormat final : public NumberFormatBase {
    JS_OBJECT(NumberFormat, NumberFormatBase);
    JS_DECLARE_ALLOCATOR(NumberFormat);

public:
    enum class Style {
        Invalid,
        Decimal,
        Percent,
        Currency,
        Unit,
    };

    enum class CurrencyDisplay {
        Code,
        Symbol,
        NarrowSymbol,
        Name,
    };

    enum class CurrencySign {
        Standard,
        Accounting,
    };

    enum class Notation {
        Invalid,
        Standard,
        Scientific,
        Engineering,
        Compact,
    };

    enum class CompactDisplay {
        Short,
        Long,
    };

    enum class SignDisplay {
        Invalid,
        Auto,
        Never,
        Always,
        ExceptZero,
        Negative,
    };

    enum class UseGrouping {
        Invalid,
        Always,
        Auto,
        Min2,
        False,
    };

    static constexpr auto relevant_extension_keys()
    {
        // 15.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.numberformat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        return AK::Array { "nu"sv };
    }

    virtual ~NumberFormat() override = default;

    String const& numbering_system() const { return m_numbering_system; }
    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }

    Style style() const { return m_style; }
    StringView style_string() const;
    void set_style(StringView style);

    bool has_currency() const { return m_currency.has_value(); }
    String const& currency() const { return m_currency.value(); }
    void set_currency(String currency) { m_currency = move(currency); }

    bool has_currency_display() const { return m_currency_display.has_value(); }
    CurrencyDisplay currency_display() const { return *m_currency_display; }
    StringView currency_display_string() const;
    void set_currency_display(StringView currency_display);
    StringView resolve_currency_display();

    bool has_currency_sign() const { return m_currency_sign.has_value(); }
    CurrencySign currency_sign() const { return *m_currency_sign; }
    StringView currency_sign_string() const;
    void set_currency_sign(StringView set_currency_sign);

    bool has_unit() const { return m_unit.has_value(); }
    String const& unit() const { return m_unit.value(); }
    void set_unit(String unit) { m_unit = move(unit); }

    bool has_unit_display() const { return m_unit_display.has_value(); }
    ::Locale::Style unit_display() const { return *m_unit_display; }
    StringView unit_display_string() const { return ::Locale::style_to_string(*m_unit_display); }
    void set_unit_display(StringView unit_display) { m_unit_display = ::Locale::style_from_string(unit_display); }

    UseGrouping use_grouping() const { return m_use_grouping; }
    Value use_grouping_to_value(VM&) const;
    void set_use_grouping(StringOrBoolean const& use_grouping);

    Notation notation() const { return m_notation; }
    StringView notation_string() const;
    void set_notation(StringView notation);

    bool has_compact_display() const { return m_compact_display.has_value(); }
    CompactDisplay compact_display() const { return *m_compact_display; }
    StringView compact_display_string() const;
    void set_compact_display(StringView compact_display);

    SignDisplay sign_display() const { return m_sign_display; }
    StringView sign_display_string() const;
    void set_sign_display(StringView sign_display);

    NativeFunction* bound_format() const { return m_bound_format; }
    void set_bound_format(NativeFunction* bound_format) { m_bound_format = bound_format; }

    bool has_compact_format() const { return m_compact_format.has_value(); }
    void set_compact_format(::Locale::NumberFormat compact_format) { m_compact_format = compact_format; }
    ::Locale::NumberFormat compact_format() const { return *m_compact_format; }

private:
    explicit NumberFormat(Object& prototype);

    virtual void visit_edges(Visitor&) override;

    String m_locale;                                     // [[Locale]]
    String m_data_locale;                                // [[DataLocale]]
    String m_numbering_system;                           // [[NumberingSystem]]
    Style m_style { Style::Invalid };                    // [[Style]]
    Optional<String> m_currency {};                      // [[Currency]]
    Optional<CurrencyDisplay> m_currency_display {};     // [[CurrencyDisplay]]
    Optional<CurrencySign> m_currency_sign {};           // [[CurrencySign]]
    Optional<String> m_unit {};                          // [[Unit]]
    Optional<::Locale::Style> m_unit_display {};         // [[UnitDisplay]]
    UseGrouping m_use_grouping { UseGrouping::False };   // [[UseGrouping]]
    Notation m_notation { Notation::Invalid };           // [[Notation]]
    Optional<CompactDisplay> m_compact_display {};       // [[CompactDisplay]]
    SignDisplay m_sign_display { SignDisplay::Invalid }; // [[SignDisplay]]
    GCPtr<NativeFunction> m_bound_format;                // [[BoundFormat]]

    // Non-standard. Stores the resolved currency display string based on [[Locale]], [[Currency]], and [[CurrencyDisplay]].
    Optional<String> m_resolved_currency_display;

    // Non-standard. Stores the resolved compact number format based on [[Locale]], [[Notation], [[Style]], and [[CompactDisplay]].
    Optional<::Locale::NumberFormat> m_compact_format;
};

struct FormatResult {
    String formatted_string;                  // [[FormattedString]]
    MathematicalValue rounded_number { 0.0 }; // [[RoundedNumber]]
};

struct RawFormatResult : public FormatResult {
    int digits { 0 };             // [[IntegerDigitsCount]]
    int rounding_magnitude { 0 }; // [[RoundingMagnitude]]
};

enum class RoundingDecision {
    LowerValue,
    HigherValue,
};

int currency_digits(StringView currency);
FormatResult format_numeric_to_string(NumberFormatBase const& intl_object, MathematicalValue number);
Vector<PatternPartition> partition_number_pattern(VM&, NumberFormat&, MathematicalValue number);
Vector<PatternPartition> partition_notation_sub_pattern(NumberFormat&, MathematicalValue const& number, String formatted_string, int exponent);
String format_numeric(VM&, NumberFormat&, MathematicalValue number);
NonnullGCPtr<Array> format_numeric_to_parts(VM&, NumberFormat&, MathematicalValue number);
RawFormatResult to_raw_precision(MathematicalValue const& number, int min_precision, int max_precision, NumberFormat::UnsignedRoundingMode unsigned_rounding_mode);
RawFormatResult to_raw_fixed(MathematicalValue const& number, int min_fraction, int max_fraction, int rounding_increment, NumberFormat::UnsignedRoundingMode unsigned_rounding_mode);
Optional<Variant<StringView, String>> get_number_format_pattern(VM&, NumberFormat&, MathematicalValue const& number, ::Locale::NumberFormat& found_pattern);
Optional<StringView> get_notation_sub_pattern(NumberFormat&, int exponent);
int compute_exponent(NumberFormat&, MathematicalValue number);
int compute_exponent_for_magnitude(NumberFormat&, int magnitude);
ThrowCompletionOr<MathematicalValue> to_intl_mathematical_value(VM&, Value value);
NumberFormat::UnsignedRoundingMode get_unsigned_rounding_mode(NumberFormat::RoundingMode, bool is_negative);
RoundingDecision apply_unsigned_rounding_mode(MathematicalValue const& x, MathematicalValue const& r1, MathematicalValue const& r2, NumberFormat::UnsignedRoundingMode unsigned_rounding_mode);
ThrowCompletionOr<Vector<PatternPartitionWithSource>> partition_number_range_pattern(VM&, NumberFormat&, MathematicalValue start, MathematicalValue end);
Vector<PatternPartitionWithSource> format_approximately(NumberFormat&, Vector<PatternPartitionWithSource> result);
Vector<PatternPartitionWithSource> collapse_number_range(Vector<PatternPartitionWithSource> result);
ThrowCompletionOr<String> format_numeric_range(VM&, NumberFormat&, MathematicalValue start, MathematicalValue end);
ThrowCompletionOr<NonnullGCPtr<Array>> format_numeric_range_to_parts(VM&, NumberFormat&, MathematicalValue start, MathematicalValue end);

}
