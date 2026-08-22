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

    explicit PendingSubstitutionStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::PendingSubstitution, data)
    {
    }
};

}
