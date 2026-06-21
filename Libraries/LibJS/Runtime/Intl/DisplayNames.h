/*
 * Copyright (c) 2021-2025, Tim Flynn <trflynn89@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/StringView.h>
#include <AK/Utf16String.h>
#include <AK/Utf16View.h>
#include <LibJS/Runtime/Intl/IntlObject.h>
#include <LibUnicode/DisplayNames.h>
#include <LibUnicode/Locale.h>

namespace JS::Intl {

class DisplayNames final : public IntlObject {
    JS_OBJECT(DisplayNames, IntlObject);
    GC_DECLARE_ALLOCATOR(DisplayNames);

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

    virtual ReadonlySpan<Utf16View> relevant_extension_keys() const override;
    virtual ReadonlySpan<ResolutionOptionDescriptor> resolution_option_descriptors(VM&) const override;

    Utf16String const& locale() const { return m_locale; }
    void set_locale(Utf16String locale) { m_locale = move(locale); }

    Utf16String const& icu_locale() const { return m_icu_locale; }
    void set_icu_locale(Utf16String icu_locale) { m_icu_locale = move(icu_locale); }

    Unicode::Style style() const { return m_style; }
    void set_style(Utf16View style) { m_style = Unicode::style_from_string(style); }
    Utf16String style_string() const { return Unicode::style_to_string(m_style); }

    Type type() const { return m_type; }
    void set_type(Utf16View type);
    Utf16String type_string() const;

    Fallback fallback() const { return m_fallback; }
    void set_fallback(Utf16View fallback);
    Utf16String fallback_string() const;

    bool has_language_display() const { return m_language_display.has_value(); }
    Unicode::LanguageDisplay language_display() const { return *m_language_display; }
    void set_language_display(Utf16View language_display) { m_language_display = Unicode::language_display_from_string(language_display); }
    Utf16String language_display_string() const { return Unicode::language_display_to_string(*m_language_display); }

private:
    explicit DisplayNames(Object& prototype);

    Utf16String m_locale;                                  // [[Locale]]
    Unicode::Style m_style { Unicode::Style::Long };       // [[Style]]
    Type m_type { Type::Invalid };                         // [[Type]]
    Fallback m_fallback { Fallback::Invalid };             // [[Fallback]]
    Optional<Unicode::LanguageDisplay> m_language_display; // [[LanguageDisplay]]

    // Non-standard. Stores the ICU locale for display-name lookups.
    Utf16String m_icu_locale;
};

ThrowCompletionOr<Value> canonical_code_for_display_names(VM&, DisplayNames::Type, Utf16View code);
bool is_valid_date_time_field_code(Utf16View field);

}
