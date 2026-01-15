/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class FontVariantAlternatesFunctionStyleValue final : public StyleValueWithDefaultOperators<FontVariantAlternatesFunctionStyleValue> {
public:
    static NonnullRefPtr<FontVariantAlternatesFunctionStyleValue> create(FontFeatureValueType type, StyleValueVector names)
    {
        return adopt_ref(*new FontVariantAlternatesFunctionStyleValue(type, move(names)));
    }

    virtual void serialize(StringBuilder&, SerializationMode) const override;

    FontFeatureValueType function_type() const { return m_function_type; }
    StyleValueVector const& names() const { return m_names; }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    bool properties_equal(FontVariantAlternatesFunctionStyleValue const& other) const
    {
        return m_function_type == other.m_function_type && m_names == other.m_names;
    }

private:
    FontVariantAlternatesFunctionStyleValue(FontFeatureValueType function_type, StyleValueVector names)
        : StyleValueWithDefaultOperators(Type::FontVariantAlternatesFunction)
        , m_function_type(function_type)
        , m_names(move(names))
    {
    }

    virtual ~FontVariantAlternatesFunctionStyleValue() override = default;

    FontFeatureValueType m_function_type;
    StyleValueVector m_names;
};

}
