/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "AngleStyleValue.h"

namespace Web::CSS {

AngleStyleValue::AngleStyleValue(Angle angle)
    : DimensionStyleValue(Type::Angle)
    , m_value(StyleValueFFI::rust_style_value_create_angle(angle.raw_value(), to_underlying(angle.unit())))
{
}

AngleStyleValue::~AngleStyleValue() = default;

ValueComparingNonnullRefPtr<StyleValue const> AngleStyleValue::absolutized(ComputationContext const&) const
{
    if (angle().unit() == canonical_angle_unit())
        return *this;
    return create(Angle::make_degrees(angle().to_degrees()));
}

void AngleStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    angle().serialize(builder, mode);
}

bool AngleStyleValue::equals(StyleValue const& other) const
{
    if (type() != other.type())
        return false;
    auto const& other_angle = other.as_angle();
    return angle() == other_angle.angle();
}

}
