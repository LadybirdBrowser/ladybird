/*
 * Copyright (c) 2026, Tim Ledbetter <timledbetter@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/RangeInputBox.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(RangeInputBox);

RangeInputBox::RangeInputBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, element, move(style))
{
}

CSS::SizeWithAspectRatio RangeInputBox::compute_auto_content_box_size() const
{
    // AD-HOC: A slider has no in-flow content to size itself from, so provide a default content-box size for when
    //         its `width` or `height` is `auto`.
    // NB: We only support horizontal sliders, so the default size is not adjusted for the writing mode.
    auto width = CSS::Length(20, CSS::LengthUnit::Ch).to_px(*this);
    auto height = CSS::Length(16, CSS::LengthUnit::Px).to_px(*this);
    return { width, height, {} };
}

}
