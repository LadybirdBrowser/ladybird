/*
 * Copyright (c) 2018-2025, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextAreaBox.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Layout {

TextAreaBox::TextAreaBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, element, move(style))
{
}

Optional<CSSPixels> TextAreaBox::compute_natural_width() const
{
    return CSS::Length(dom_node().cols(), CSS::LengthUnit::Ch).to_px(*this);
}

Optional<CSSPixels> TextAreaBox::compute_natural_height() const
{
    return CSS::Length(dom_node().rows(), CSS::LengthUnit::Lh).to_px(*this);
}

}
