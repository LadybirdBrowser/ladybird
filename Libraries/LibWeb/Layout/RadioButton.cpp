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

RadioButton::RadioButton(DOM::Document& document, HTML::HTMLInputElement& element, GC::Ref<CSS::ComputedProperties> style)
    : FormAssociatedLabelableNode(document, element, move(style))
{
}

RadioButton::~RadioButton() = default;

GC::Ptr<Painting::Paintable> RadioButton::create_paintable() const
{
    return Painting::RadioButtonPaintable::create(*this);
}

}
