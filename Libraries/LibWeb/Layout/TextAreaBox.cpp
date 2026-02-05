/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextAreaBox.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(TextAreaBox);

TextAreaBox::TextAreaBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, element, move(style))
{
}

CSS::SizeWithAspectRatio TextAreaBox::compute_auto_content_box_size() const
{
    auto width = CSS::Length(dom_node().cols(), CSS::LengthUnit::Ch).to_px(*this);
    auto height = CSS::Length(dom_node().rows(), CSS::LengthUnit::Lh).to_px(*this);

    if (this->computed_values().writing_mode() != CSS::WritingMode::HorizontalTb)
        swap(width, height);

    return { width, height, {} };
}

}
