/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextInputBox.h>

namespace Web::Layout {

TextInputBox::TextInputBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, element, move(style))
{
}

CSS::SizeWithAspectRatio TextInputBox::compute_intrinsic_content_box_size() const
{
    return {
        .width = CSS::Length(dom_node().size(), CSS::LengthUnit::Ch).to_px(*this),
        .height = computed_values().line_height() + CSSPixels(2), // FIXME - determine where 2px comes from
        .aspect_ratio = {}
    };
}

}
