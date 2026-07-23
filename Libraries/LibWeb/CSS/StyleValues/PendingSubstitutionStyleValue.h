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
    void serialize(StringBuilder&, SerializationMode) const { }
    Vector<Parser::ComponentValue> tokenize() const
    {
        // Not sure what to do here, but this isn't valid so returning GIV seems the most correct.
        return { Parser::ComponentValue { Parser::GuaranteedInvalidValue {} } };
    }

    StyleValue const& original_shorthand_value() const { return m_original_shorthand_value; }

    // We shouldn't need to compare these, but in case we do: The nature of them is that their value is unknown, so
    // consider them all to be unique.
    bool properties_equal(PendingSubstitutionStyleValue const&) const { return false; }

    // NB: We should never be in a position where we need to check this
private:
    friend class StyleValue;

    explicit PendingSubstitutionStyleValue(StyleValue const& original_shorthand_value)
        : StyleValueWithDefaultOperators(Type::PendingSubstitution, make_pending_substitution_data(original_shorthand_value))
        , m_original_shorthand_value(original_shorthand_value)
    {
    }

    explicit PendingSubstitutionStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::PendingSubstitution, data)
        , m_original_shorthand_value(StyleValue::adopt_rust_style_value_data(StyleValueFFI::rust_style_value_retain(static_cast<StyleValueFFI::StyleValueData const*>(data->pending_substitution.original_shorthand_value.pointer))))
    {
    }

    static StyleValueFFI::StyleValueData const* make_pending_substitution_data(StyleValue const& original_shorthand_value)
    {
        return StyleValueFFI::rust_style_value_create_pending_substitution(StyleValueFFI::rust_style_value_retain(original_shorthand_value.rust_style_value_data()));
    }

    ValueComparingNonnullRefPtr<StyleValue const> m_original_shorthand_value;
};

}
