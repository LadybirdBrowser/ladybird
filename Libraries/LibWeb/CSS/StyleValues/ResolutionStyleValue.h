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

    Resolution const& resolution() const { return m_resolution; }
    virtual double raw_value() const override { return m_resolution.raw_value(); }
    virtual FlyString unit_name() const override { return m_resolution.unit_name(); }

    virtual String to_string(SerializationMode serialization_mode) const override { return m_resolution.to_string(serialization_mode); }

    bool equals(StyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_resolution = other.as_resolution();
        return m_resolution == other_resolution.m_resolution;
    }

private:
    explicit ResolutionStyleValue(Resolution resolution)
        : DimensionStyleValue(Type::Resolution)
        , m_resolution(move(resolution))
    {
    }

    Resolution m_resolution;
};

}
