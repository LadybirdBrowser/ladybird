/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor
class AnchorStyleValue final : public StyleValue {
public:
    virtual ~AnchorStyleValue() override = default;

private:
    friend class StyleValue;

    explicit AnchorStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValue(Type::Anchor, data)
    {
    }
};

}
