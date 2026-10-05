/*
 * Copyright (c) 2026, mikiubo <michele.uboldi@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "OverflowClipMarginStyleValue.h"
#include <LibWeb/CSS/StyleValues/LengthStyleValue.h>

namespace Web::CSS {

ValueComparingNonnullRefPtr<OverflowClipMarginStyleValue const> OverflowClipMarginStyleValue::create(Optional<BackgroundBox> visual_box, NonnullRefPtr<StyleValue const> offset)
{
    return adopt_ref(*new (nothrow) OverflowClipMarginStyleValue(visual_box, move(offset)));
}

OverflowClipMarginStyleValue::OverflowClipMarginStyleValue(Optional<BackgroundBox> visual_box, NonnullRefPtr<StyleValue const> offset)
    : StyleValueWithDefaultOperators(Type::OverflowClipMargin, StyleValueFFI::rust_style_value_create_overflow_clip_margin(visual_box.has_value(), visual_box.has_value() ? to_underlying(*visual_box) : 0, StyleValueFFI::rust_style_value_retain(offset->rust_style_value_data())))
{
}

OverflowClipMarginStyleValue::~OverflowClipMarginStyleValue() = default;

}
