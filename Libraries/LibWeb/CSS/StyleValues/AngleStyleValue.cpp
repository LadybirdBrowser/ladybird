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

AngleStyleValue::AngleStyleValue(double value, AngleUnit unit)
    : DimensionStyleValue(Type::Angle, StyleValueFFI::rust_style_value_create_angle(value, to_underlying(unit)))
{
}

AngleStyleValue::~AngleStyleValue() = default;

}
