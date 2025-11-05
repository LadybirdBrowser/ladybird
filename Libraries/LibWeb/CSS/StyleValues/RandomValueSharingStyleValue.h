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
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue(fixed_value, false, {}, false));
    }

    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_auto(FlyString name, bool element_shared)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue({}, true, move(name), element_shared));
    }

    static ValueComparingNonnullRefPtr<RandomValueSharingStyleValue const> create_dashed_ident(FlyString name, bool element_shared)
    {
        return adopt_ref(*new (nothrow) RandomValueSharingStyleValue({}, false, move(name), element_shared));
    }

    virtual ~RandomValueSharingStyleValue() override = default;

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    double random_base_value() const;

    virtual String to_string(SerializationMode serialization_mode) const override;

    bool properties_equal(RandomValueSharingStyleValue const& other) const
    {
        return m_fixed_value == other.m_fixed_value
            && m_is_auto == other.m_is_auto
            && m_name == other.m_name
            && m_element_shared == other.m_element_shared;
    }

private:
    explicit RandomValueSharingStyleValue(RefPtr<StyleValue const> fixed_value, bool is_auto, Optional<FlyString> name, bool element_shared)
        : StyleValueWithDefaultOperators(Type::RandomValueSharing)
        , m_fixed_value(move(fixed_value))
        , m_is_auto(is_auto)
        , m_name(move(name))
        , m_element_shared(element_shared)
    {
    }

    ValueComparingRefPtr<StyleValue const> m_fixed_value;
    bool m_is_auto;
    Optional<FlyString> m_name;
    bool m_element_shared;
};

}
