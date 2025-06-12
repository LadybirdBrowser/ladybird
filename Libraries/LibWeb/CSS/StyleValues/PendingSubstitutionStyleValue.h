/*
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-values-5/#pending-substitution-value
class PendingSubstitutionStyleValue final : public StyleValueWithDefaultOperators<PendingSubstitutionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<PendingSubstitutionStyleValue> create()
    {
        static ValueComparingNonnullRefPtr<PendingSubstitutionStyleValue> instance = adopt_ref(*new (nothrow) PendingSubstitutionStyleValue());
        return instance;
    }
    virtual ~PendingSubstitutionStyleValue() override = default;
    virtual String to_string(SerializationMode) const override { return {}; }

    // We shouldn't need to compare these, but in case we do: The nature of them is that their value is unknown, so
    // consider them all to be unique.
    bool properties_equal(PendingSubstitutionStyleValue const&) const { return false; }

private:
    PendingSubstitutionStyleValue()
        : StyleValueWithDefaultOperators(Type::PendingSubstitution)
    {
    }
};

}
