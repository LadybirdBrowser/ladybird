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
#include <LibWeb/CSS/Parser/ComponentValue.h>
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

    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence, Optional<String> original_source_text = {}, SourceTextMode = SourceTextMode::Trim, bool contains_attr_tainted_values = false);
    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create_attr_tainted_with_parsed_value(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence, Optional<String> original_source_text, SourceTextMode, NonnullRefPtr<StyleValue const> parsed_value);
    virtual ~UnresolvedStyleValue() override = default;

    void serialize(StringBuilder&, SerializationMode) const;
    Vector<Parser::ComponentValue> tokenize() const;

    Vector<Parser::ComponentValue> values() const;
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
    RefPtr<StyleValue const> parsed_value() const { return m_parsed_value; }

    bool equals(StyleValue const& other) const;

    GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const;

private:
    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create_internal(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence, Optional<String> original_source_text, SourceTextMode, bool contains_attr_tainted_values, RefPtr<StyleValue const> parsed_value);

    UnresolvedStyleValue(String source_text, String value_comparison_text, Parser::SubstitutionFunctionsPresence, bool contains_attr_tainted_values, RefPtr<StyleValue const> parsed_value);

    String comparison_text() const;

    String source_text() const { return String::from_raw(m_value->unresolved.source_text.raw); }
    String value_comparison_text() const { return String::from_raw(m_value->unresolved.value_comparison_text.raw); }

    RefPtr<StyleValue const> m_parsed_value;
};

}
