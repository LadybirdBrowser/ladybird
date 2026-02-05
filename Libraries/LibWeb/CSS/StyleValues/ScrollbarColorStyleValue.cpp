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

void ScrollbarColorStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    m_thumb_color->serialize(builder, mode);
    builder.append(' ');
    m_track_color->serialize(builder, mode);
}

}
