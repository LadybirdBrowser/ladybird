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
    : DimensionStyleValue(Type::Angle, StyleValueFFI::rust_style_value_create_angle(angle.raw_value(), to_underlying(angle.unit())))
{
}

AngleStyleValue::~AngleStyleValue() = default;

}
