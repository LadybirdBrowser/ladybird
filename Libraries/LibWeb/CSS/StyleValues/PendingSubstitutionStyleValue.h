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
    Vector<Parser::ComponentValue> tokenize() const
    {
        // Not sure what to do here, but this isn't valid so returning GIV seems the most correct.
        return { Parser::ComponentValue { Parser::GuaranteedInvalidValue {} } };
    }

    ValueComparingNonnullRefPtr<StyleValue const> original_shorthand_value() const { return wrap_rust_child(m_value->pending_substitution.original_shorthand_value); }

    // NB: Pending-substitution values never compare equal (their value is unknown);
    //     StyleValue::equals special-cases them.
private:
    friend class StyleValue;

    explicit PendingSubstitutionStyleValue(StyleValue const& original_shorthand_value)
        : StyleValueWithDefaultOperators(Type::PendingSubstitution, make_pending_substitution_data(original_shorthand_value))
    {
    }

    explicit PendingSubstitutionStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::PendingSubstitution, data)
    {
    }

    static StyleValueFFI::StyleValueData const* make_pending_substitution_data(StyleValue const& original_shorthand_value)
    {
        return StyleValueFFI::rust_style_value_create_pending_substitution(StyleValueFFI::rust_style_value_retain(original_shorthand_value.rust_style_value_data()));
    }
};

}
