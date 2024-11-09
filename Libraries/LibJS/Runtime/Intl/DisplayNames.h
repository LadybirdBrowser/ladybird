/*
 * Copyright (c) 2021-2024, Tim Flynn <trflynn89@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <LibJS/Runtime/Object.h>
#include <LibUnicode/DisplayNames.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

class DisplayNames final : public Object {
    JS_OBJECT(DisplayNames, Object);
    JS_DECLARE_ALLOCATOR(DisplayNames);

    enum class Type {
        Invalid,
        Language,
        Region,
        Script,
        Currency,
        Calendar,
        DateTimeField,
    };

    enum class Fallback {
        Invalid,
        None,
        Code,
    };

public:
    virtual ~DisplayNames() override = default;

    String const& locale() const { return m_locale; }
    void set_locale(String locale) { m_locale = move(locale); }

    Unicode::Style style() const { return m_style; }
    void set_style(StringView style) { m_style = Unicode::style_from_string(style); }
    StringView style_string() const { return Unicode::style_to_string(m_style); }

    Type type() const { return m_type; }
    void set_type(StringView type);
    StringView type_string() const;

    Fallback fallback() const { return m_fallback; }
    void set_fallback(StringView fallback);
    StringView fallback_string() const;

    bool has_language_display() const { return m_language_display.has_value(); }
    Unicode::LanguageDisplay language_display() const { return *m_language_display; }
    void set_language_display(StringView language_display) { m_language_display = Unicode::language_display_from_string(language_display); }
    StringView language_display_string() const { return Unicode::language_display_to_string(*m_language_display); }

private:
    DisplayNames(Object& prototype);

    String m_locale;                                       // [[Locale]]
    Unicode::Style m_style { Unicode::Style::Long };       // [[Style]]
    Type m_type { Type::Invalid };                         // [[Type]]
    Fallback m_fallback { Fallback::Invalid };             // [[Fallback]]
    Optional<Unicode::LanguageDisplay> m_language_display; // [[LanguageDisplay]]
};

ThrowCompletionOr<Value> canonical_code_for_display_names(VM&, DisplayNames::Type, StringView code);
bool is_valid_date_time_field_code(StringView field);

}
