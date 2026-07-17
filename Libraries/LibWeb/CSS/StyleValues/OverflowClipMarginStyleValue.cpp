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
    : StyleValueWithDefaultOperators(Type::OverflowClipMargin)
    , m_value(StyleValueFFI::rust_style_value_create_overflow_clip_margin(visual_box.has_value(), visual_box.has_value() ? to_underlying(*visual_box) : 0, &offset.leak_ref()))
{
}

OverflowClipMarginStyleValue::~OverflowClipMarginStyleValue() = default;

// https://drafts.csswg.org/css-overflow-4/#overflow-clip-margin
void OverflowClipMarginStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    bool has_explicit_box = visual_box().has_value();
    bool is_default_box = has_explicit_box && *visual_box() == BackgroundBox::PaddingBox;
    bool is_zero_offset = offset().is_length() && offset().as_length().length().raw_value() == 0;

    if (!has_explicit_box || is_default_box) {
        offset().serialize(builder, mode);
    } else if (is_zero_offset) {
        builder.append(CSS::to_string(*visual_box()));
    } else {
        builder.append(CSS::to_string(*visual_box()));
        builder.append(' ');
        offset().serialize(builder, mode);
    }
}

ValueComparingNonnullRefPtr<StyleValue const> OverflowClipMarginStyleValue::absolutized(ComputationContext const& context) const
{
    auto new_offset = offset().absolutized(context);
    if (new_offset->equals(offset()))
        return *this;
    return create(visual_box(), move(new_offset));
}

bool OverflowClipMarginStyleValue::properties_equal(OverflowClipMarginStyleValue const& other) const
{
    return visual_box() == other.visual_box()
        && offset() == other.offset();
}

}
