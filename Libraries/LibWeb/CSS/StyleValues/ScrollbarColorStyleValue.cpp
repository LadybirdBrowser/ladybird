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

}
