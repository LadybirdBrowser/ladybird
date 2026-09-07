/*
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Utf16FlyString.h>
#include <LibWeb/CSS/PercentageOr.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

// https://drafts.csswg.org/css-anchor-position-1/#funcdef-anchor-size
class AnchorSizeStyleValue final : public StyleValueWithDefaultOperators<AnchorSizeStyleValue> {
public:
    virtual ~AnchorSizeStyleValue() override = default;

    Optional<Utf16FlyString> anchor_name() const
    {
        if (!m_value->anchor_size.has_anchor_name)
            return {};
        return css_string_from_rust(&m_value->anchor_size.anchor_name);
    }
    Optional<AnchorSize> anchor_size() const
    {
        if (!m_value->anchor_size.has_anchor_size)
            return {};
        return static_cast<AnchorSize>(m_value->anchor_size.anchor_size);
    }
    ValueComparingRefPtr<StyleValue const> fallback_value() const
    {
        return wrap_rust_child_or_null(m_value->anchor_size.fallback_value);
    }

private:
    friend class StyleValue;

    explicit AnchorSizeStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::AnchorSize, data)
    {
    }
};

}
