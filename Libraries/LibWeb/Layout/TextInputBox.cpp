/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextInputBox.h>

namespace Web::Layout {

TextInputBox::TextInputBox(DOM::Document& document, GC::Ptr<DOM::Element> element, NonnullRefPtr<CSS::ComputedValues const> style)
    : BlockContainer(document, element, style)
{
}

CSS::SizeWithAspectRatio TextInputBox::compute_auto_content_box_size() const
{
    return default_preferred_size_for_text_control(dom_node(), *this);
}

CSS::SizeWithAspectRatio TextInputBox::default_preferred_size_for_text_control(HTML::HTMLInputElement const& input_element, Box const& box)
{
    // https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-text-entry-widget
    // An input element whose type attribute is in one of the above states is an element with default preferred size,
    // and user agents are expected to apply the 'field-sizing' CSS property to the element. User agents are expected
    // to determine the inline size of its intrinsic size by the following steps:
    // [...] If the element has a size attribute, and parsing that attribute's value using the rules for parsing
    // non-negative integers doesn't generate an error, return the value obtained from applying the converting a
    // character width to pixels algorithm to the value of the attribute. Otherwise, return the value obtained from
    // applying the converting a character width to pixels algorithm to the number 20.
    //
    // FIXME: Implement the specified "converting a character width to pixels" algorithm. The size attribute should
    //        also only affect the text entry types (text, search, tel, url, email, password). The other types using
    //        this box are domain-specific widgets that should ignore it.
    auto width = CSS::Length(input_element.size(), CSS::LengthUnit::Ch).to_px(box);

    // FIXME: HTML does not yet detail the primitive appearance of text inputs. Use one line for the default preferred
    //        block size, matching the native appearance described by HTML and the behavior of other engines.
    auto height = box.computed_values().line_height();

    if (box.computed_values().writing_mode() != CSS::WritingMode::HorizontalTb)
        swap(width, height);

    return { width, height, {} };
}

}
