/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Length.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>
#include <LibWeb/Export.h>

namespace Web::CSS {

class WEB_API LengthStyleValue final : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<LengthStyleValue const> create(Length const&);
    virtual ~LengthStyleValue() override = default;

    Length length() const { return Length(m_value->length.value, static_cast<LengthUnit>(m_value->length.unit)); }
    virtual double raw_value() const override { return m_value->length.value; }
    virtual Utf16FlyString unit_name() const override { return length().unit_name(); }

    void serialize(StringBuilder& builder, SerializationMode mode) const { length().serialize(builder, mode); }
    void serialize(Utf16StringBuilder& builder, SerializationMode mode) const { length().serialize(builder, mode); }
    ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const;

    bool equals(StyleValue const& other) const;

private:
    explicit LengthStyleValue(Length const& length)
        : DimensionStyleValue(Type::Length, StyleValueFFI::rust_style_value_create_length(length.raw_value(), to_underlying(length.unit())))
    {
    }
};

}
