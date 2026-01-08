/*
 * Copyright (c) 2024, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class ScrollbarGutterStyleValue final : public StyleValueWithDefaultOperators<ScrollbarGutterStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ScrollbarGutterStyleValue const> create(ScrollbarGutter value)
    {
        return adopt_ref(*new (nothrow) ScrollbarGutterStyleValue(value));
    }
    virtual ~ScrollbarGutterStyleValue() override = default;

    ScrollbarGutter value() const { return m_value; }

    virtual void serialize(StringBuilder& builder, SerializationMode) const override
    {
        switch (m_value) {
        case ScrollbarGutter::Auto:
            builder.append("auto"sv);
            break;
        case ScrollbarGutter::Stable:
            builder.append("stable"sv);
            break;
        case ScrollbarGutter::BothEdges:
            builder.append("stable both-edges"sv);
            break;
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
