/*
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/DimensionStyleValue.h>

namespace Web::CSS {

class ResolutionStyleValue : public DimensionStyleValue {
public:
    static ValueComparingNonnullRefPtr<ResolutionStyleValue const> create(Resolution resolution)
    {
        return adopt_ref(*new (nothrow) ResolutionStyleValue(move(resolution)));
    }
    virtual ~ResolutionStyleValue() override = default;

    Resolution resolution() const { return Resolution(m_value->resolution.value, static_cast<ResolutionUnit>(m_value->resolution.unit)); }
    virtual double raw_value() const override { return m_value->resolution.value; }
    virtual Utf16FlyString unit_name() const override { return resolution().unit_name(); }

    virtual ValueComparingNonnullRefPtr<StyleValue const> absolutized(ComputationContext const&) const override;

    virtual void serialize(StringBuilder& builder, SerializationMode mode) const override { resolution().serialize(builder, mode); }
    virtual void serialize(Utf16StringBuilder& builder, SerializationMode mode) const override { resolution().serialize(builder, mode); }

    virtual bool is_computationally_independent() const override { return true; }

    bool equals(StyleValue const& other) const override;

private:
    explicit ResolutionStyleValue(Resolution resolution)
        : DimensionStyleValue(Type::Resolution, StyleValueFFI::rust_style_value_create_resolution(resolution.raw_value(), to_underlying(resolution.unit())))
    {
    }
};

}
