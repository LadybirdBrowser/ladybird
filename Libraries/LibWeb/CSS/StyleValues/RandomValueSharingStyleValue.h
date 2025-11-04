/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class RandomValueSharingStyleValue : public StyleValueWithDefaultOperators<RandomValueSharingStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_fixed(NonnullRefPtr<StyleValue const> const& fixed_value)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue(fixed_value));
    }

    virtual ~RandomValueSharingStyleValue() override = default;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    double random_base_value() const;

    virtual String to_string(SerializationMode serialization_mode) const override;

    bool properties_equal(RandomValueSharingStyleValue const& other) const
    {
        return m_fixed_value == other.m_fixed_value;
    }

private:
    explicit RandomValueSharingStyleValue(RefPtr<StyleValue const> fixed_value)
        : StyleValueWithDefaultOperators(Type::RandomValueSharing)
        , m_fixed_value(move(fixed_value))
    {
    }

    ValueComparingRefPtr<StyleValue const> m_fixed_value;
};

}
