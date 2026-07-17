/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ScrollbarColorStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<ScrollbarColorStyleValue const> ScrollbarColorStyleValue::create(NonnullRefPtr<StyleValue const> thumb_color, NonnullRefPtr<StyleValue const> track_color)
{
    return adopt_ref(*new ScrollbarColorStyleValue(move(thumb_color), move(track_color)));
}

ValueComparingNonnullRefPtr<StyleValue const> ScrollbarColorStyleValue::absolutized(ComputationContext const& computation_context) const
{
    auto absolutized_thumb_color = thumb_color()->absolutized(computation_context);
    auto absolutized_track_color = track_color()->absolutized(computation_context);
    return create(absolutized_thumb_color, absolutized_track_color);
}

void ScrollbarColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    thumb_color()->serialize(builder, mode);
    builder.append(' ');
    track_color()->serialize(builder, mode);
}

}
