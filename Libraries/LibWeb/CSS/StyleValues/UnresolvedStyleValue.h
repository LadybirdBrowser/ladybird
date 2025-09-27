/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2025, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Vector.h>
#include <LibWeb/CSS/Parser/ComponentValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class UnresolvedStyleValue final : public StyleValue {
public:
    static ValueComparingNonnullRefPtr<UnresolvedStyleValue const> create(Vector<Parser::ComponentValue>&& values, Optional<Parser::SubstitutionFunctionsPresence> = {}, Optional<String> original_source_text = {});
    virtual ~UnresolvedStyleValue() override = default;

    virtual String to_string(SerializationMode) const override;
    virtual Vector<Parser::ComponentValue> tokenize() const override { return m_values; }

    Vector<Parser::ComponentValue> const& values() const { return m_values; }
    bool contains_arbitrary_substitution_function() const { return m_substitution_functions_presence.has_any(); }
    bool includes_attr_function() const { return m_substitution_functions_presence.attr; }
    bool includes_var_function() const { return m_substitution_functions_presence.var; }

    virtual bool equals(StyleValue const& other) const override;

    virtual GC::Ref<CSSStyleValue> reify(JS::Realm&, FlyString const& associated_property) const override;

private:
    UnresolvedStyleValue(Vector<Parser::ComponentValue>&& values, Parser::SubstitutionFunctionsPresence, Optional<String> original_source_text);

    Vector<Parser::ComponentValue> m_values;
    Parser::SubstitutionFunctionsPresence m_substitution_functions_presence {};
    Optional<String> m_original_source_text;
};

}
