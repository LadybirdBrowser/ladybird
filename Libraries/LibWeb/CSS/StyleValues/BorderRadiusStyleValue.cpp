/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BorderRadiusStyleValue.h"

namespace Web::CSS {

void BorderRadiusStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    auto horizontal_radius = this->horizontal_radius();
    auto vertical_radius = this->vertical_radius();
    if (horizontal_radius == vertical_radius) {
        horizontal_radius->serialize(builder, mode);
        return;
    }
    horizontal_radius->serialize(builder, mode);
    builder.append(' ');
    vertical_radius->serialize(builder, mode);
}

ValueComparingNonnullRefPtr<StyleValue const> BorderRadiusStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto horizontal_radius = this->horizontal_radius();
    auto vertical_radius = this->vertical_radius();
    auto absolutized_horizontal_radius = horizontal_radius->absolutized(computation_context);
    auto absolutized_vertical_radius = vertical_radius->absolutized(computation_context);

    if (absolutized_vertical_radius == vertical_radius && absolutized_horizontal_radius == horizontal_radius)
        return *this;

    return BorderRadiusStyleValue::create(absolutized_horizontal_radius, absolutized_vertical_radius);
}

}
