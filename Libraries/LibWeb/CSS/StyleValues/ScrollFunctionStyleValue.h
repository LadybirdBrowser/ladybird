/*
 * Copyright (c) 2025, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Enums.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ScrollFunctionStyleValue final : public StyleValueWithDefaultOperators<ScrollFunctionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ScrollFunctionStyleValue const> create(Scroller scroller, Axis axis)
    {
        return adopt_ref(*new ScrollFunctionStyleValue(scroller, axis));
    }
    virtual ~ScrollFunctionStyleValue() override = default;

    virtual String to_string(SerializationMode) const override;
    bool properties_equal(ScrollFunctionStyleValue const& other) const { return m_scroller == other.m_scroller && m_axis == other.m_axis; }

    Scroller scroller() const { return m_scroller; }
    Axis axis() const { return m_axis; }

private:
    explicit ScrollFunctionStyleValue(Scroller scroller, Axis axis)
        : StyleValueWithDefaultOperators(Type::ScrollFunction)
        , m_scroller(scroller)
        , m_axis(axis)
    {
    }

    Scroller m_scroller;
    Axis m_axis;
};

}
