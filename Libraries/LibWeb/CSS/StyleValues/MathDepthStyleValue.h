/*
 * Copyright (c) 2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/StyleValues/StyleValue.h>

namespace Web::CSS {

class MathDepthStyleValue : public StyleValueWithDefaultOperators<MathDepthStyleValue> {
public:
    static ValueComparingNonnullRefPtr<MathDepthStyleValue const> create_auto_add();
    static ValueComparingNonnullRefPtr<MathDepthStyleValue const> create_add(ValueComparingNonnullRefPtr<StyleValue const> integer_value);
    static ValueComparingNonnullRefPtr<MathDepthStyleValue const> create_integer(ValueComparingNonnullRefPtr<StyleValue const> integer_value);
    virtual ~MathDepthStyleValue() override = default;

    bool is_auto_add() const { return m_type == MathDepthType::AutoAdd; }
    bool is_add() const { return m_type == MathDepthType::Add; }
    bool is_integer() const { return m_type == MathDepthType::Integer; }
    auto integer_value() const
    {
        VERIFY(!m_integer_value.is_null());
        return m_integer_value;
    }
    virtual void serialize(StringBuilder&, SerializationMode) const override;

    bool properties_equal(MathDepthStyleValue const& other) const;

private:
    enum class MathDepthType {
        AutoAdd,
        Add,
        Integer,
    };

    MathDepthStyleValue(MathDepthType type, ValueComparingRefPtr<StyleValue const> integer_value = nullptr);

    MathDepthType m_type;
    ValueComparingRefPtr<StyleValue const> m_integer_value;
};

}
