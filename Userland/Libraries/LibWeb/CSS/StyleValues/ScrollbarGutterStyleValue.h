/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class ScrollbarGutterStyleValue final : public StyleValueWithDefaultOperators<ScrollbarGutterStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ScrollbarGutterStyleValue> create(ScrollbarGutter value)
    {
        return adopt_ref(*new (nothrow) ScrollbarGutterStyleValue(value));
    }
    virtual ~ScrollbarGutterStyleValue() override = default;

    ScrollbarGutter value() const { return m_value; }

    virtual String to_string() const override
    {
        switch (m_value) {
        case ScrollbarGutter::Auto:
            return "auto"_string;
        case ScrollbarGutter::Stable:
            return "stable"_string;
        case ScrollbarGutter::BothEdges:
            return "stable both-edges"_string;
        default:
            VERIFY_NOT_REACHED();
        }
    }

    bool properties_equal(ScrollbarGutterStyleValue const& other) const { return m_value == other.m_value; }

private:
    ScrollbarGutterStyleValue(ScrollbarGutter value)
        : StyleValueWithDefaultOperators(Type::ScrollbarGutter)
        , m_value(value)
    {
    }

    ScrollbarGutter m_value;
};

}
