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
    : StyleValueWithDefaultOperators(Type::BackgroundSize)
    , m_value(StyleValueFFI::rust_style_value_create_background_size(&size_x.leak_ref(), &size_y.leak_ref()))
{
}

BackgroundSizeStyleValue::~BackgroundSizeStyleValue() = default;

void BackgroundSizeStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    if (size_x()->has_auto() && size_y()->has_auto()) {
        builder.append("auto"sv);
        return;
    }
    size_x()->serialize(builder, mode);
    builder.append(' ');
    size_y()->serialize(builder, mode);
}

ValueComparingNonnullRefPtr<StyleValue const> BackgroundSizeStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_size_x = size_x()->absolutized(computation_context);
    auto absolutized_size_y = size_y()->absolutized(computation_context);

    if (absolutized_size_x == size_x() && absolutized_size_y == size_y())
        return *this;

    return BackgroundSizeStyleValue::create(absolutized_size_x, absolutized_size_y);
}

}
