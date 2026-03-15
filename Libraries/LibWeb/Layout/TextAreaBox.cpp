/*
 * Copyright (c) 2025-2026, Jonathan Gamble <gamblej@gmail.com>
 * Copyright (c) 2026, Jack Bodine <jacktaylorbodine@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/TextAreaBox.h>
#include <LibWeb/CSS/ComputedProperties.h>
#include <LibWeb/CSS/PropertyID.h>
#include <LibWeb/CSS/SerializationMode.h>
#include <LibWeb/CSS/StyleValues/StyleValue.h>
#include <LibWeb/DOM/Node.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(TextAreaBox);

TextAreaBox::TextAreaBox(DOM::Document& document, GC::Ptr<DOM::Element> element, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, element, move(style))
{
}

CSS::SizeWithAspectRatio TextAreaBox::compute_auto_content_box_size() const
{
    auto const& style_properties = dom_node().computed_properties();

    // If field-sizing: content, return empty size (auto) to fit the text content.
    if (style_properties->property(CSS::PropertyID::FieldSizing).to_string(CSS::SerializationMode::Normal) == "content") {
        return { {}, {}, {} };
    }

    auto width = CSS::Length(dom_node().cols(), CSS::LengthUnit::Ch).to_px(*this);
    auto height = CSS::Length(dom_node().rows(), CSS::LengthUnit::Lh).to_px(*this);

    if (this->computed_values().writing_mode() != CSS::WritingMode::HorizontalTb)
        swap(width, height);

    return { width, height, {} };
}

}
