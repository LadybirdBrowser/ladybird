/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 * Copyright (c) 2021-2024, Sam Atkins <sam@ladybird.org>
 * Copyright (c) 2022-2023, MacDue <macdue@dueutil.tech>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "CSSColorValue.h"
#include <LibWeb/CSS/Serialize.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<CSSColorValue> CSSColorValue::create_from_color(Color color)
{
    if (color.value() == 0) {
        static auto transparent = adopt_ref(*new (nothrow) CSSColorValue(color));
        return transparent;
    }

    if (color == Color::from_rgb(0x000000)) {
        static auto black = adopt_ref(*new (nothrow) CSSColorValue(color));
        return black;
    }

    if (color == Color::from_rgb(0xffffff)) {
        static auto white = adopt_ref(*new (nothrow) CSSColorValue(color));
        return white;
    }

    return adopt_ref(*new (nothrow) CSSColorValue(color));
}

String CSSColorValue::to_string() const
{
    return serialize_a_srgb_value(m_color);
}

}
