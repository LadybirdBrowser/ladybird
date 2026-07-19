/*
 * Copyright (c) 2024-present, the Ladybird developers.
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

    ScrollbarGutter value() const { return static_cast<ScrollbarGutter>(m_value->scrollbar_gutter.value); }

    void serialize(StringBuilder& builder, SerializationMode) const
    {
        switch (value()) {
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

    bool properties_equal(ScrollbarGutterStyleValue const& other) const { return value() == other.value(); }

private:
    ScrollbarGutterStyleValue(ScrollbarGutter value)
        : StyleValueWithDefaultOperators(Type::ScrollbarGutter, StyleValueFFI::rust_style_value_create_scrollbar_gutter(to_underlying(value)))
    {
    }
};

}
