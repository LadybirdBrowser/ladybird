/*
 * Copyright (c) 2021, Tim Flynn <trflynn89@serenityos.org>
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/RadioButton.h>
#include <LibWeb/Painting/RadioButtonPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(RadioButton);

RadioButton::RadioButton(DOM::Document& document, HTML::HTMLInputElement& element, CSS::StyleProperties style)
    : FormAssociatedLabelableNode(document, element, move(style))
{
    set_natural_width(12);
    set_natural_height(12);
    set_natural_aspect_ratio(1);
}

RadioButton::~RadioButton() = default;

GC::Ptr<Painting::Paintable> RadioButton::create_paintable() const
{
    return Painting::RadioButtonPaintable::create(*this);
}

}
