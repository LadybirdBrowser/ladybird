/*
 * Copyright (c) 2018-2025, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextAreaBox.h>

namespace Web::Layout {

TextAreaBox::TextAreaBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, element, move(style))
{
}

CSS::SizeWithAspectRatio TextAreaBox::compute_intrinsic_content_box_size() const
{
    return {
        .width = CSS::Length(dom_node().cols(), CSS::LengthUnit::Ch).to_px(*this),
        .height = CSS::Length(dom_node().rows(), CSS::LengthUnit::Lh).to_px(*this),
        .aspect_ratio = {}
    };
}

}
