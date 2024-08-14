/*
 * Copyright (c) 2022-2024, Sam Atkins <sam@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/Resolution.h>
#include <LibWeb/CSS/StyleValues/CSSUnitValue.h>

namespace Web::CSS {

class ResolutionStyleValue : public CSSUnitValue {
public:
    static ValueComparingNonnullRefPtr<ResolutionStyleValue> create(Resolution resolution)
    {
        return adopt_ref(*new (nothrow) ResolutionStyleValue(move(resolution)));
    }
    virtual ~ResolutionStyleValue() override = default;

    Resolution const& resolution() const { return m_resolution; }
    virtual double value() const override { return m_resolution.raw_value(); }
    virtual StringView unit() const override { return m_resolution.unit_name(); }

    virtual String to_string() const override { return m_resolution.to_string(); }

    bool equals(CSSStyleValue const& other) const override
    {
        if (type() != other.type())
            return false;
        auto const& other_resolution = other.as_resolution();
        return m_resolution == other_resolution.m_resolution;
    }

private:
    explicit ResolutionStyleValue(Resolution resolution)
        : CSSUnitValue(Type::Resolution)
        , m_resolution(move(resolution))
    {
    }

    Resolution m_resolution;
};

}
