/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ViewFunctionStyleValue final : public StyleValueWithDefaultOperators<ViewFunctionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ViewFunctionStyleValue const> create(Axis axis, NonnullRefPtr<StyleValue const> inset)
    {
        return adopt_ref(*new ViewFunctionStyleValue(axis, move(inset)));
    }
    virtual ~ViewFunctionStyleValue() override = default;

    virtual String to_string(SerializationMode) const override;
    bool properties_equal(ViewFunctionStyleValue const& other) const { return m_axis == other.m_axis && m_inset == other.m_inset; }
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    Axis axis() const { return m_axis; }
    NonnullRefPtr<StyleValue const> inset() const { return m_inset; }

private:
    explicit ViewFunctionStyleValue(Axis axis, NonnullRefPtr<StyleValue const> inset)
        : StyleValueWithDefaultOperators(Type::ViewFunction)
        , m_axis(axis)
        , m_inset(move(inset))
    {
    }

    Axis m_axis;
    ValueComparingNonnullRefPtr<StyleValue const> m_inset;
};

}
