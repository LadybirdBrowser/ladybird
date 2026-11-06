/*
 * Copyright (c) 2023-2025, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/Serialize.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://www.w3.org/TR/css-values-4/#custom-idents
class CustomIdentStyleValue final : public StyleValueWithDefaultOperators<CustomIdentStyleValue> {
public:
    virtual ~CustomIdentStyleValue() override = default;

    Utf16FlyString custom_ident() const { return css_string_from_rust(&m_value->custom_ident.custom_ident); }

private:
    friend class StyleValue;

    explicit CustomIdentStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::CustomIdent, data)
    {
    }
};

}
