/*
 * Copyright (c) 2026, Callum Law <callumlaw1709@outlook.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/NumberStyleValue.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class OpacityValueStyleValue final : public StyleValueWithDefaultOperators<OpacityValueStyleValue> {
public:
    virtual ~OpacityValueStyleValue() override = default;

    double resolved() const { return value()->as_number().number(); }

    GC::Ref<CSSStyleValue> reify(Utf16FlyString const& associated_property) const;

private:
    friend class StyleValue;

    explicit OpacityValueStyleValue(StyleValueFFI::StyleValueData const* data)
        : StyleValueWithDefaultOperators(Type::OpacityValue, data)
    {
    }

    ValueComparingNonnullRefPtr<StyleValue const> value() const { return wrap_rust_child(m_value->opacity_value.value); }
};

}
