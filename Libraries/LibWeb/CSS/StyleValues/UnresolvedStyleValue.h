/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/String.h>
#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/SubstitutionFunctionsPresence.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API UnresolvedStyleValue final : public StyleValue {
public:
    enum class SourceTextMode : u8 {
        Trim,
        TrimLeading,
        Preserve,
    };

    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create(Utf16String token_source, Parser::SubstitutionFunctionsPresence, Optional<Utf16String> original_source_text = {}, SourceTextMode = SourceTextMode::Trim, bool contains_attr_tainted_values = false);
    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create_attr_tainted_with_parsed_value(Utf16String token_source, Parser::SubstitutionFunctionsPresence, Optional<Utf16String> original_source_text, SourceTextMode, NonnullRefPtr<StyleValue const> parsed_value);
    virtual ~UnresolvedStyleValue() override = default;

    Utf16String serialized_components() const;
    Utf16String token_source() const;
    bool contains_arbitrary_substitution_function() const
    {
        auto const& data = m_value->unresolved;
        return data.presence_attr || data.presence_dashed_function || data.presence_env || data.presence_if || data.presence_inherit || data.presence_var;
    }
    bool contains_attr_tainted_values() const { return m_value->unresolved.contains_attr_tainted_values; }
    bool includes_attr_function() const { return m_value->unresolved.presence_attr; }
    bool includes_inherit_function() const { return m_value->unresolved.presence_inherit; }
    bool includes_if_function() const { return m_value->unresolved.presence_if; }
    bool includes_var_function() const { return m_value->unresolved.presence_var; }
    bool includes_dashed_function() const { return m_value->unresolved.presence_dashed_function; }
    RefPtr<StyleValue const> parsed_value() const { return wrap_rust_child_or_null(m_value->unresolved.parsed_value); }

    bool equals(StyleValue const& other) const;

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;

private:
    friend class StyleValue;

    explicit UnresolvedStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValue(Type::Unresolved, data)
    {
    }

    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create_internal(Utf16String token_source, Parser::SubstitutionFunctionsPresence, Optional<Utf16String> original_source_text, SourceTextMode, bool contains_attr_tainted_values, RefPtr<StyleValue const> parsed_value);

    Utf16String comparison_text() const;
    Utf16String serialize_components(u8 mode) const;

    static Utf16String string_from_rust_data(StyleValueFFI::RetainedReadableString const& string)
    {
        if (string.raw != 0)
            return Utf16String::from_raw(string.raw);
        if (string.ascii_units)
            return Utf16String::from_utf8_without_validation({ string.ascii_units, string.length });
        return Utf16String::from_utf16({ reinterpret_cast<char16_t const*>(string.code_units), string.length });
    }

    Utf16String source_text() const { return string_from_rust_data(m_value->unresolved.source_text); }
    Utf16String value_comparison_text() const { return string_from_rust_data(m_value->unresolved.value_comparison_text); }
};

}
