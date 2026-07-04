/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextInputBox.h>

namespace Web::Layout {

TextInputBox::TextInputBox(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::ComputedProperties const& style)
    : BlockContainer(document, element, style)
{
}

CSS::SizeWithAspectRatio TextInputBox::compute_auto_content_box_size() const
{
    return auto_content_box_size_for_text_control(dom_node(), *this);
}

CSS::SizeWithAspectRatio TextInputBox::auto_content_box_size_for_text_control(HTML::HTMLInputElement const& input_element, Box const& box)
{
    // FIXME: Per https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-text-entry-widget the
    //        size attribute should only affect the width of the text entry types (text, search, tel, url, email,
    //        password). The other types using this box are domain-specific widgets that should ignore it.
    auto width = CSS::Length(input_element.size(), CSS::LengthUnit::Ch).to_px(box);
    auto height = box.computed_values().line_height();

    if (box.computed_values().writing_mode() != CSS::WritingMode::HorizontalTb)
        swap(width, height);

    return { width, height, {} };
}

}
