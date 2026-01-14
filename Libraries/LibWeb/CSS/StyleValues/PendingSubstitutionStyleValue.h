/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-5/#pending-substitution-value
class PendingSubstitutionStyleValue final : public StyleValueWithDefaultOperators<PendingSubstitutionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<PendingSubstitutionStyleValue> create(StyleValue const& original_shorthand_value)
    {
        return adopt_ref(*new (nothrow) PendingSubstitutionStyleValue(original_shorthand_value));
    }
    virtual ~PendingSubstitutionStyleValue() override = default;
    virtual void serialize(StringBuilder&, SerializationMode) const override { }
    virtual Vector<Parser::ComponentValue> tokenize() const override
    {
        // Not sure what to do here, but this isn't valid so returning GIV seems the most correct.
        return { Parser::ComponentValue { Parser::GuaranteedInvalidValue {} } };
    }

    StyleValue const& original_shorthand_value() const { return *m_original_shorthand_value; }

    // We shouldn't need to compare these, but in case we do: The nature of them is that their value is unknown, so
    // consider them all to be unique.
    bool properties_equal(PendingSubstitutionStyleValue const&) const { return false; }

private:
    explicit PendingSubstitutionStyleValue(StyleValue const& original_shorthand_value)
        : StyleValueWithDefaultOperators(Type::PendingSubstitution)
        , m_original_shorthand_value(original_shorthand_value)
    {
    }

    NonnullRefPtr<StyleValue const> m_original_shorthand_value;
};

}
