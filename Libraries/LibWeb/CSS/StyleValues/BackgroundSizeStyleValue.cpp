/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "BackgroundSizeStyleValue.h"

namespace Web::CSS {

BackgroundSizeStyleValue::BackgroundSizeStyleValue(ValueComparingNonnullRefPtr<StyleValue const> size_x, ValueComparingNonnullRefPtr<StyleValue const> size_y)
    : StyleValueWithDefaultOperators(Type::BackgroundSize, StyleValueFFI::rust_style_value_create_background_size(StyleValueFFI::rust_style_value_retain(size_x->rust_style_value_data()), StyleValueFFI::rust_style_value_retain(size_y->rust_style_value_data())))
{
}

BackgroundSizeStyleValue::~BackgroundSizeStyleValue() = default;

}
