/*
 * Copyright (c) 2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>
#include <LibWeb/CSS/PercentageOr.h>

namespace Web::CSS {

class ScaleStyleValue : public StyleValueWithDefaultOperators<ScaleStyleValue> {
public:
    static ValueComparingNonnullRefPtr<ScaleStyleValue> create(NumberPercentage x, NumberPercentage y)
    {
        return adopt_ref(*new (nothrow) ScaleStyleValue(move(x), move(y)));
    }

    virtual ~ScaleStyleValue() override = default;

    NumberPercentage const& x() const { return m_properties.x; }
    NumberPercentage const& y() const { return m_properties.y; }

    virtual String to_string() const override;

    bool properties_equal(ScaleStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    explicit ScaleStyleValue(
        NumberPercentage x,
        NumberPercentage y)
        : StyleValueWithDefaultOperators(Type::Scale)
        , m_properties {
            .x = move(x),
            .y = move(y),
        }
    {
    }

    struct Properties {
        NumberPercentage x;
        NumberPercentage y;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
