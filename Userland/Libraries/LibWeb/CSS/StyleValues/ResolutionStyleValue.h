/*
 * Copyright (c) 2022-2023, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/Resolution.h>

namespace Web::CSS {

class ResolutionStyleValue : public StyleValueWithDefaultOperators<ResolutionStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ResolutionStyleValue> create(Resolution resolution)
    {
        return adopt_ref(*new (nothrow) ResolutionStyleValue(move(resolution)));
    }
    virtual ~ResolutionStyleValue() override = default;

    Resolution const& resolution() const { return m_resolution; }

    virtual String to_string() const override { return m_resolution.to_string(); }

    bool properties_equal(ResolutionStyleValue const& other) const { return m_resolution == other.m_resolution; }

private:
    explicit ResolutionStyleValue(Resolution resolution)
        : StyleValueWithDefaultOperators(Type::Resolution)
        , m_resolution(move(resolution))
    {
    }

    Resolution m_resolution;
};

}
