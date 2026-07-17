/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2023, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "RectStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<RectStyleValue const> RectStyleValue::create(NonnullRefPtr<StyleValue const> top, NonnullRefPtr<StyleValue const> right, NonnullRefPtr<StyleValue const> bottom, NonnullRefPtr<StyleValue const> left)
{
    return adopt_ref(*new (nothrow) RectStyleValue(move(top), move(right), move(bottom), move(left)));
}

ValueComparingNonnullRefPtr<StyleValue const> RectStyleValue::absolutized(ComputationContext const& context) const
{
    auto top_absolutized = top()->absolutized(context);
    auto right_absolutized = right()->absolutized(context);
    auto bottom_absolutized = bottom()->absolutized(context);
    auto left_absolutized = left()->absolutized(context);

    if (top_absolutized == top() && right_absolutized == right() && bottom_absolutized == bottom() && left_absolutized == left())
        return *this;

    return RectStyleValue::create(top_absolutized, right_absolutized, bottom_absolutized, left_absolutized);
}

void RectStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    builder.append("rect("sv);
    top()->serialize(builder, mode);
    builder.append(", "sv);
    right()->serialize(builder, mode);
    builder.append(", "sv);
    bottom()->serialize(builder, mode);
    builder.append(", "sv);
    left()->serialize(builder, mode);
    builder.append(")"sv);
}

}
