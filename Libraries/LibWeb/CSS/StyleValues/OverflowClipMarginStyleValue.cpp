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
    , m_visual_box(visual_box)
    , m_offset(move(offset))
{
}

OverflowClipMarginStyleValue::~OverflowClipMarginStyleValue() = default;

// https://drafts.csswg.org/css-overflow-4/#overflow-clip-margin
void OverflowClipMarginStyleValue::serialize(StringBuilder& builder, SerializationMode mode) const
{
    bool has_explicit_box = m_visual_box.has_value();
    bool is_default_box = has_explicit_box && *m_visual_box == BackgroundBox::PaddingBox;
    bool is_zero_offset = m_offset->is_length() && m_offset->as_length().length().raw_value() == 0;

    if (!has_explicit_box || is_default_box) {
        m_offset->serialize(builder, mode);
    } else if (is_zero_offset) {
        builder.append(CSS::to_string(*m_visual_box));
    } else {
        builder.append(CSS::to_string(*m_visual_box));
        builder.append(' ');
        m_offset->serialize(builder, mode);
    }
}

ValueComparingNonnullRefPtr<StyleValue const> OverflowClipMarginStyleValue::absolutized(ComputationContext const& context) const
{
    auto new_offset = m_offset->absolutized(context);
    if (new_offset->equals(m_offset))
        return *this;
    return create(m_visual_box, move(new_offset));
}

bool OverflowClipMarginStyleValue::properties_equal(OverflowClipMarginStyleValue const& other) const
{
    return m_visual_box == other.m_visual_box
        && m_offset == other.m_offset;
}

}
