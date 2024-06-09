/*
 * Copyright (c) 2022-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Array.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/Completion.h>
#include <LibJS/Runtime/Intl/AbstractOperations.h>
#include <LibJS/Runtime/Object.h>
#include <LibLocale/Locale.h>
#include <LibLocale/NumberFormat.h>
#include <LibLocale/RelativeTimeFormat.h>

namespace JS::Intl {

class RelativeTimeFormat final : public Object {
    JS_OBJECT(RelativeTimeFormat, Object);
    JS_DECLARE_ALLOCATOR(RelativeTimeFormat);

public:
    enum class Numeric {
        Always,
        Auto,
    };

    static constexpr auto relevant_extension_keys()
    {
        // 17.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        return AK::Array { "nu"sv };
    }

    virtual ~RelativeTimeFormat() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    String const& data_locale() const { return m_data_locale; }
    void set_data_locale(String data_locale) { m_data_locale = move(data_locale); }

    String const& numbering_system() const { return m_numbering_system; }
    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }

    ::Locale::Style style() const { return m_style; }
    void set_style(StringView style) { m_style = ::Locale::style_from_string(style); }
    StringView style_string() const { return ::Locale::style_to_string(m_style); }

    Numeric numeric() const { return m_numeric; }
    void set_numeric(StringView numeric);
    StringView numeric_string() const;

    NumberFormat& number_format() const { return *m_number_format; }
    void set_number_format(NumberFormat* number_format) { m_number_format = number_format; }

    PluralRules& plural_rules() const { return *m_plural_rules; }
    void set_plural_rules(PluralRules* plural_rules) { m_plural_rules = plural_rules; }

private:
    explicit RelativeTimeFormat(Object& prototype);

    virtual void visit_edges(Cell::Visitor&) override;

    String m_locale;                                   // [[Locale]]
    String m_data_locale;                              // [[DataLocale]]
    String m_numbering_system;                         // [[NumberingSystem]]
    ::Locale::Style m_style { ::Locale::Style::Long }; // [[Style]]
    Numeric m_numeric { Numeric::Always };             // [[Numeric]]
    GCPtr<NumberFormat> m_number_format;               // [[NumberFormat]]
    GCPtr<PluralRules> m_plural_rules;                 // [[PluralRules]]
};

struct PatternPartitionWithUnit : public PatternPartition {
    PatternPartitionWithUnit(StringView type, String value, StringView unit_string = {})
        : PatternPartition(type, move(value))
        , unit(unit_string)
    {
    }

    StringView unit;
};

ThrowCompletionOr<::Locale::TimeUnit> singular_relative_time_unit(VM&, StringView unit);
ThrowCompletionOr<Vector<PatternPartitionWithUnit>> partition_relative_time_pattern(VM&, RelativeTimeFormat&, double value, StringView unit);
Vector<PatternPartitionWithUnit> make_parts_list(StringView pattern, StringView unit, Vector<::Locale::NumberFormat::Partition> parts);
ThrowCompletionOr<String> format_relative_time(VM&, RelativeTimeFormat&, double value, StringView unit);
ThrowCompletionOr<NonnullGCPtr<Array>> format_relative_time_to_parts(VM&, RelativeTimeFormat&, double value, StringView unit);

}
