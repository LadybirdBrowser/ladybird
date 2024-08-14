/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class MathDepthStyleValue : public StyleValueWithDefaultOperators<MathDepthStyleValue> {
public:
    static ValueComparingNonnullRefPtr<MathDepthStyleValue> create_auto_add();
    static ValueComparingNonnullRefPtr<MathDepthStyleValue> create_add(ValueComparingNonnullRefPtr<CSSStyleValue const> integer_value);
    static ValueComparingNonnullRefPtr<MathDepthStyleValue> create_integer(ValueComparingNonnullRefPtr<CSSStyleValue const> integer_value);
    virtual ~MathDepthStyleValue() override = default;

    bool is_auto_add() const { return m_type == MathDepthType::AutoAdd; }
    bool is_add() const { return m_type == MathDepthType::Add; }
    bool is_integer() const { return m_type == MathDepthType::Integer; }
    auto integer_value() const
    {
        VERIFY(!m_integer_value.is_null());
        return m_integer_value;
    }
    virtual String to_string() const override;

    bool properties_equal(MathDepthStyleValue const& other) const;

private:
    enum class MathDepthType {
        AutoAdd,
        Add,
        Integer,
    };

    MathDepthStyleValue(MathDepthType type, ValueComparingRefPtr<CSSStyleValue const> integer_value = nullptr);

    MathDepthType m_type;
    ValueComparingRefPtr<CSSStyleValue const> m_integer_value;
};

}
