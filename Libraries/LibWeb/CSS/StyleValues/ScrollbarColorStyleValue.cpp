/*
 * Copyright (c) 2025, Tim Ledbetter <tim.ledbetter@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ScrollbarColorStyleValue.h"

namespace Web::CSS {

ValueComparingNonnullRefPtr<ScrollbarColorStyleValue const> ScrollbarColorStyleValue::create(NonnullRefPtr<CSSStyleValue const> thumb_color, NonnullRefPtr<CSSStyleValue const> track_color)
{
    return adopt_ref(*new ScrollbarColorStyleValue(move(thumb_color), move(track_color)));
}

String ScrollbarColorStyleValue::to_string(SerializationMode mode) const
{
    return MUST(String::formatted("{} {}", m_thumb_color->to_string(mode), m_track_color->to_string(mode)));
}

}
