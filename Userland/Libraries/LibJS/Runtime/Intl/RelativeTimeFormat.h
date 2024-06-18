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
    static constexpr auto relevant_extension_keys()
    {
        // 17.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        return AK::Array { "nu"sv };
    }

    virtual ~RelativeTimeFormat() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    String const& numbering_system() const { return m_numbering_system; }
    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }

    ::Locale::Style style() const { return m_style; }
    void set_style(StringView style) { m_style = ::Locale::style_from_string(style); }
    StringView style_string() const { return ::Locale::style_to_string(m_style); }

    ::Locale::NumericDisplay numeric() const { return m_numeric; }
    void set_numeric(StringView numeric) { m_numeric = ::Locale::numeric_display_from_string(numeric); }
    StringView numeric_string() const { return ::Locale::numeric_display_to_string(m_numeric); }

    ::Locale::RelativeTimeFormat const& formatter() const { return *m_formatter; }
    void set_formatter(NonnullOwnPtr<::Locale::RelativeTimeFormat> formatter) { m_formatter = move(formatter); }

private:
    explicit RelativeTimeFormat(Object& prototype);

    String m_locale;                                                         // [[Locale]]
    String m_numbering_system;                                               // [[NumberingSystem]]
    ::Locale::Style m_style { ::Locale::Style::Long };                       // [[Style]]
    ::Locale::NumericDisplay m_numeric { ::Locale::NumericDisplay::Always }; // [[Numeric]]

    // Non-standard. Stores the ICU relative-time formatter for the Intl object's formatting options.
    OwnPtr<::Locale::RelativeTimeFormat> m_formatter;
};

ThrowCompletionOr<::Locale::TimeUnit> singular_relative_time_unit(VM&, StringView unit);
ThrowCompletionOr<Vector<::Locale::RelativeTimeFormat::Partition>> partition_relative_time_pattern(VM&, RelativeTimeFormat&, double value, StringView unit);
ThrowCompletionOr<String> format_relative_time(VM&, RelativeTimeFormat&, double value, StringView unit);
ThrowCompletionOr<NonnullGCPtr<Array>> format_relative_time_to_parts(VM&, RelativeTimeFormat&, double value, StringView unit);

}
