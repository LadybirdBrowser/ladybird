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

namespace Web::CSS {

class UnresolvedStyleValue final : public StyleValue {
public:
    enum class SourceTextMode : u8 {
        Trim,
        Preserve,
    };

    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence, Optional<String> original_source_text = {}, SourceTextMode = SourceTextMode::Trim, bool contains_attr_tainted_values = false);
    virtual ~UnresolvedStyleValue() override = default;

    virtual void serialize(StringBuilder&, SerializationMode) const override;
    virtual Vector<Parser::ComponentValue> tokenize() const override;

    Vector<Parser::ComponentValue> values() const;
    bool contains_arbitrary_substitution_function() const { return m_substitution_functions_presence.has_any(); }
    bool contains_attr_tainted_values() const { return m_contains_attr_tainted_values; }
    bool includes_attr_function() const { return m_substitution_functions_presence.attr; }
    bool includes_inherit_function() const { return m_substitution_functions_presence.inherit; }
    bool includes_if_function() const { return m_substitution_functions_presence.if_; }
    bool includes_var_function() const { return m_substitution_functions_presence.var; }

    virtual bool equals(StyleValue const& other) const override;

    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, Utf16FlyString const& associated_property) const override;

    virtual bool is_computationally_independent() const override { VERIFY_NOT_REACHED(); }

private:
    UnresolvedStyleValue(String source_text, String value_comparison_text, Parser::SubstitutionFunctionsPresence, bool contains_attr_tainted_values);

    StringView comparison_text() const;

    String m_source_text;
    String m_value_comparison_text;
    Parser::SubstitutionFunctionsPresence m_substitution_functions_presence {};
    bool m_contains_attr_tainted_values { false };
};

}
