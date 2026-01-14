/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class AddFunctionStyleValue : public StyleValueWithDefaultOperators<AddFunctionStyleValue> {
public:
    static NonnullRefPtr<AddFunctionStyleValue> create(NonnullRefPtr<StyleValue const> value)
    {
        return adopt_ref(*new AddFunctionStyleValue(move(value)));
    }

    NonnullRefPtr<StyleValue const> value() const { return m_value; }
    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(AddFunctionStyleValue const& other) const { return m_value == other.m_value; }

private:
    AddFunctionStyleValue(NonnullRefPtr<StyleValue const> value)
        : StyleValueWithDefaultOperators(Type::AddFunction)
        , m_value(move(value))
    {
    }

    virtual ~AddFunctionStyleValue() override = default;

    ValueComparingNonnullRefPtr<StyleValue const> m_value;
};

}
