/*
 * Copyright (c) 2024, Steffen T. Larssen <dudedbz@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/CSS/CSSStyleValue.h>

namespace Web::CSS {

class RotationStyleValue : public StyleValueWithDefaultOperators<RotationStyleValue> {
public:
    static ValueComparingNonnullRefPtr<RotationStyleValue> create(ValueComparingNonnullRefPtr<CSSStyleValue const> angle, ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_x, ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_y, ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_z)
    {
        return adopt_ref(*new (nothrow) RotationStyleValue(move(angle), move(rotation_x), move(rotation_y), move(rotation_z)));
    }

    virtual ~RotationStyleValue() override = default;

    ValueComparingNonnullRefPtr<CSSStyleValue const> const& angle() const { return m_properties.angle; }
    ValueComparingNonnullRefPtr<CSSStyleValue const> const& rotation_x() const { return m_properties.rotation_x; }
    ValueComparingNonnullRefPtr<CSSStyleValue const> const& rotation_y() const { return m_properties.rotation_y; }
    ValueComparingNonnullRefPtr<CSSStyleValue const> const& rotation_z() const { return m_properties.rotation_z; }

    virtual String to_string() const override;

    bool properties_equal(RotationStyleValue const& other) const { return m_properties == other.m_properties; }

private:
    explicit RotationStyleValue(
        ValueComparingNonnullRefPtr<CSSStyleValue const> angle,
        ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_x,
        ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_y,
        ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_z)
        : StyleValueWithDefaultOperators(Type::Rotation)
        , m_properties {
            .angle = move(angle),
            .rotation_x = move(rotation_x),
            .rotation_y = move(rotation_y),
            .rotation_z = move(rotation_z)
        }
    {
    }

    struct Properties {
        ValueComparingNonnullRefPtr<CSSStyleValue const> angle;
        ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_x;
        ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_y;
        ValueComparingNonnullRefPtr<CSSStyleValue const> rotation_z;
        bool operator==(Properties const&) const = default;
    } m_properties;
};

}
