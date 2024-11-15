/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/HTMLInputElement.h>
#include <LibWeb/Layout/CheckBox.h>
#include <LibWeb/Painting/CheckBoxPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(CheckBox);

CheckBox::CheckBox(DOM::Document& document, HTML::HTMLInputElement& element, CSS::StyleProperties style)
    : FormAssociatedLabelableNode(document, element, move(style))
{
    set_natural_width(13);
    set_natural_height(13);
}

CheckBox::~CheckBox() = default;

GC::Ptr<Painting::Paintable> CheckBox::create_paintable() const
{
    return Painting::CheckBoxPaintable::create(*this);
}

}
