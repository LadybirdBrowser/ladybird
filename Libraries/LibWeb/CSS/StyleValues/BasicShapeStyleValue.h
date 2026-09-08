/*
 * Copyright (c) 2024, MacDue <macdue@dueutil.tech>
 * Copyright (c) 2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class BasicShapeStyleValue final : public StyleValueWithDefaultOperators<BasicShapeStyleValue> {
private:
    friend class StyleValue;

    explicit BasicShapeStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::BasicShape, data)
    {
    }
};

}
