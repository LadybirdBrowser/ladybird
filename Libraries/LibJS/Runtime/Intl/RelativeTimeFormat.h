/*
 * Copyright (c) 2022-2025, Tim Flynn <trflynn89@ladybird.org>
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
#include <LibUnicode/Locale.h>
#include <LibUnicode/NumberFormat.h>
#include <LibUnicode/RelativeTimeFormat.h>

namespace JS::Intl {

class RelativeTimeFormat final : public Object {
    JS_OBJECT(RelativeTimeFormat, Object);
    GC_DECLARE_ALLOCATOR(RelativeTimeFormat);

public:
    static constexpr auto relevant_extension_keys()
    {
        // 18.2.3 Internal slots, https://tc39.es/ecma402/#sec-Intl.RelativeTimeFormat-internal-slots
        // The value of the [[RelevantExtensionKeys]] internal slot is « "nu" ».
        return AK::Array { "nu"sv };
    }

    virtual ~RelativeTimeFormat() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    String const& numbering_system() const { return m_numbering_system; }
    void set_numbering_system(String numbering_system) { m_numbering_system = move(numbering_system); }

    Unicode::Style style() const { return m_style; }
    void set_style(StringView style) { m_style = Unicode::style_from_string(style); }
    StringView style_string() const { return Unicode::style_to_string(m_style); }

    Unicode::NumericDisplay numeric() const { return m_numeric; }
    void set_numeric(StringView numeric) { m_numeric = Unicode::numeric_display_from_string(numeric); }
    StringView numeric_string() const { return Unicode::numeric_display_to_string(m_numeric); }

    Unicode::RelativeTimeFormat const& formatter() const { return *m_formatter; }
    void set_formatter(NonnullOwnPtr<Unicode::RelativeTimeFormat> formatter) { m_formatter = move(formatter); }

private:
    explicit RelativeTimeFormat(Object& prototype);

    String m_locale;                                                       // [[Locale]]
    String m_numbering_system;                                             // [[NumberingSystem]]
    Unicode::Style m_style { Unicode::Style::Long };                       // [[Style]]
    Unicode::NumericDisplay m_numeric { Unicode::NumericDisplay::Always }; // [[Numeric]]

    // Non-standard. Stores the ICU relative-time formatter for the Intl object's formatting options.
    OwnPtr<Unicode::RelativeTimeFormat> m_formatter;
};

ThrowCompletionOr<Unicode::TimeUnit> singular_relative_time_unit(VM&, StringView unit);
ThrowCompletionOr<Vector<Unicode::RelativeTimeFormat::Partition>> partition_relative_time_pattern(VM&, RelativeTimeFormat&, double value, StringView unit);
ThrowCompletionOr<String> format_relative_time(VM&, RelativeTimeFormat&, double value, StringView unit);
ThrowCompletionOr<GC::Ref<Array>> format_relative_time_to_parts(VM&, RelativeTimeFormat&, double value, StringView unit);

}
