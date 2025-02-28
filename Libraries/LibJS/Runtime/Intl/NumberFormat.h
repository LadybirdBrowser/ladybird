/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
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
#include <LibUnicode/Locale.h>
#include <LibUnicode/NumberFormat.h>

namespace JS::Intl {

class NumberFormatBase : public Object {
    JS_OBJECT(NumberFormatBase, Object);
    GC_DECLARE_ALLOCATOR(NumberFormatBase);

public:
    enum class ComputedRoundingPriority {
        Auto,
        MorePrecision,
        LessPrecision,
        Invalid,
    };

    virtual ~NumberFormatBase() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

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

    Unicode::RoundingType rounding_type() const { return m_rounding_type; }
    StringView rounding_type_string() const { return Unicode::rounding_type_to_string(m_rounding_type); }
    void set_rounding_type(Unicode::RoundingType rounding_type) { m_rounding_type = rounding_type; }

    ComputedRoundingPriority computed_rounding_priority() const { return m_computed_rounding_priority; }
    StringView computed_rounding_priority_string() const;
    void set_computed_rounding_priority(ComputedRoundingPriority computed_rounding_priority) { m_computed_rounding_priority = computed_rounding_priority; }

    Unicode::RoundingMode rounding_mode() const { return m_rounding_mode; }
    StringView rounding_mode_string() const { return Unicode::rounding_mode_to_string(m_rounding_mode); }
    void set_rounding_mode(StringView rounding_mode) { m_rounding_mode = Unicode::rounding_mode_from_string(rounding_mode); }

    int rounding_increment() const { return m_rounding_increment; }
    void set_rounding_increment(int rounding_increment) { m_rounding_increment = rounding_increment; }

    Unicode::TrailingZeroDisplay trailing_zero_display() const { return m_trailing_zero_display; }
    StringView trailing_zero_display_string() const { return Unicode::trailing_zero_display_to_string(m_trailing_zero_display); }
    void set_trailing_zero_display(StringView trailing_zero_display) { m_trailing_zero_display = Unicode::trailing_zero_display_from_string(trailing_zero_display); }

    Unicode::RoundingOptions rounding_options() const;

    Unicode::NumberFormat const& formatter() const { return *m_formatter; }
    void set_formatter(NonnullOwnPtr<Unicode::NumberFormat> formatter) { m_formatter = move(formatter); }

protected:
    explicit NumberFormatBase(Object& prototype);

private:
    String m_locale;                                                                             // [[Locale]]
    int m_min_integer_digits { 0 };                                                              // [[MinimumIntegerDigits]]
    Optional<int> m_min_fraction_digits {};                                                      // [[MinimumFractionDigits]]
    Optional<int> m_max_fraction_digits {};                                                      // [[MaximumFractionDigits]]
    Optional<int> m_min_significant_digits {};                                                   // [[MinimumSignificantDigits]]
    Optional<int> m_max_significant_digits {};                                                   // [[MaximumSignificantDigits]]
    Unicode::RoundingType m_rounding_type;                                                       // [[RoundingType]]
    ComputedRoundingPriority m_computed_rounding_priority { ComputedRoundingPriority::Invalid }; // [[ComputedRoundingPriority]]
    Unicode::RoundingMode m_rounding_mode;                                                       // [[RoundingMode]]
    int m_rounding_increment { 1 };                                                              // [[RoundingIncrement]]
    Unicode::TrailingZeroDisplay m_trailing_zero_display;                                        // [[TrailingZeroDisplay]]

    // Non-standard. Stores the ICU number formatter for the Intl object's formatting options.
    OwnPtr<Unicode::NumberFormat> m_formatter;
};

class NumberFormat final : public NumberFormatBase {
    JS_OBJECT(NumberFormat, NumberFormatBase);
    GC_DECLARE_ALLOCATOR(NumberFormat);

public:
    static constexpr auto relevant_extension_keys()
    {
        // 16.2.3 Internal slots, https://tc39.es/ecma402/#sec-intl.numberformat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        return AK::Array { "nu"sv };
    }

    virtual ~NumberFormat() override = default;

    String const& numbering_system() const { return m_numbering_system; }
    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }

    Unicode::NumberFormatStyle style() const { return m_style; }
    StringView style_string() const { return Unicode::number_format_style_to_string(m_style); }
    void set_style(StringView style) { m_style = Unicode::number_format_style_from_string(style); }

    bool has_currency() const { return m_currency.has_value(); }
    String const& currency() const { return m_currency.value(); }
    void set_currency(String currency) { m_currency = move(currency); }

    bool has_currency_display() const { return m_currency_display.has_value(); }
    Unicode::CurrencyDisplay currency_display() const { return *m_currency_display; }
    StringView currency_display_string() const { return Unicode::currency_display_to_string(*m_currency_display); }
    void set_currency_display(StringView currency_display) { m_currency_display = Unicode::currency_display_from_string(currency_display); }

    bool has_currency_sign() const { return m_currency_sign.has_value(); }
    Unicode::CurrencySign currency_sign() const { return *m_currency_sign; }
    StringView currency_sign_string() const { return Unicode::currency_sign_to_string(*m_currency_sign); }
    void set_currency_sign(StringView currency_sign) { m_currency_sign = Unicode::currency_sign_from_string(currency_sign); }

    bool has_unit() const { return m_unit.has_value(); }
    String const& unit() const { return m_unit.value(); }
    void set_unit(String unit) { m_unit = move(unit); }

    bool has_unit_display() const { return m_unit_display.has_value(); }
    Unicode::Style unit_display() const { return *m_unit_display; }
    StringView unit_display_string() const { return Unicode::style_to_string(*m_unit_display); }
    void set_unit_display(StringView unit_display) { m_unit_display = Unicode::style_from_string(unit_display); }

    Unicode::Grouping use_grouping() const { return m_use_grouping; }
    Value use_grouping_to_value(VM&) const;
    void set_use_grouping(StringOrBoolean const& use_grouping);

    Unicode::Notation notation() const { return m_notation; }
    StringView notation_string() const { return Unicode::notation_to_string(m_notation); }
    void set_notation(StringView notation) { m_notation = Unicode::notation_from_string(notation); }

    bool has_compact_display() const { return m_compact_display.has_value(); }
    Unicode::CompactDisplay compact_display() const { return *m_compact_display; }
    StringView compact_display_string() const { return Unicode::compact_display_to_string(*m_compact_display); }
    void set_compact_display(StringView compact_display) { m_compact_display = Unicode::compact_display_from_string(compact_display); }

    Unicode::SignDisplay sign_display() const { return m_sign_display; }
    StringView sign_display_string() const { return Unicode::sign_display_to_string(m_sign_display); }
    void set_sign_display(StringView sign_display) { m_sign_display = Unicode::sign_display_from_string(sign_display); }

    NativeFunction* bound_format() const { return m_bound_format; }
    void set_bound_format(NativeFunction* bound_format) { m_bound_format = bound_format; }

    Unicode::DisplayOptions display_options() const;

private:
    explicit NumberFormat(Object& prototype);

    virtual void visit_edges(Visitor&) override;

    String m_locale;                                               // [[Locale]]
    String m_numbering_system;                                     // [[NumberingSystem]]
    Unicode::NumberFormatStyle m_style;                            // [[Style]]
    Optional<String> m_currency;                                   // [[Currency]]
    Optional<Unicode::CurrencyDisplay> m_currency_display;         // [[CurrencyDisplay]]
    Optional<Unicode::CurrencySign> m_currency_sign;               // [[CurrencySign]]
    Optional<String> m_unit;                                       // [[Unit]]
    Optional<Unicode::Style> m_unit_display;                       // [[UnitDisplay]]
    Unicode::Grouping m_use_grouping { Unicode::Grouping::False }; // [[UseGrouping]]
    Unicode::Notation m_notation;                                  // [[Notation]]
    Optional<Unicode::CompactDisplay> m_compact_display;           // [[CompactDisplay]]
    Unicode::SignDisplay m_sign_display;                           // [[SignDisplay]]
    GC::Ptr<NativeFunction> m_bound_format;                        // [[BoundFormat]]
};

int currency_digits(StringView currency);
String format_numeric_to_string(NumberFormatBase const& intl_object, MathematicalValue const& number);
Vector<Unicode::NumberFormat::Partition> partition_number_pattern(NumberFormat const&, MathematicalValue const& number);
String format_numeric(NumberFormat const&, MathematicalValue const& number);
GC::Ref<Array> format_numeric_to_parts(VM&, NumberFormat const&, MathematicalValue const& number);
ThrowCompletionOr<MathematicalValue> to_intl_mathematical_value(VM&, Value value);
ThrowCompletionOr<Vector<Unicode::NumberFormat::Partition>> partition_number_range_pattern(VM&, NumberFormat const&, MathematicalValue const& start, MathematicalValue const& end);
ThrowCompletionOr<String> format_numeric_range(VM&, NumberFormat const&, MathematicalValue const& start, MathematicalValue const& end);
ThrowCompletionOr<GC::Ref<Array>> format_numeric_range_to_parts(VM&, NumberFormat const&, MathematicalValue const& start, MathematicalValue const& end);

}
