/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextInputBox.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(TextInputBox);

TextInputBox::TextInputBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, element, move(style))
{
}

CSS::SizeWithAspectRatio TextInputBox::compute_auto_content_box_size() const
{
    auto width = CSS::Length(dom_node().size(), CSS::LengthUnit::Ch).to_px(*this);
    auto height = computed_values().line_height() + CSSPixels(2);
    // AD-HOC: 2px is inline shadow DOM padding in HTMLInputElement::create_text_input_shadow_tree()

    if (this->computed_values().writing_mode() != CSS::WritingMode::HorizontalTb)
        swap(width, height);

    return { width, height, {} };
}

}
