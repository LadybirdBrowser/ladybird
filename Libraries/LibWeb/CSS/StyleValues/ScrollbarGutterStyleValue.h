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
    virtual ~ScrollbarGutterStyleValue() override = default;

    ScrollbarGutter value() const { return static_cast<ScrollbarGutter>(m_value->scrollbar_gutter.value); }

private:
    friend class StyleValue;

    explicit ScrollbarGutterStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::ScrollbarGutter, data)
    {
    }
};

}
