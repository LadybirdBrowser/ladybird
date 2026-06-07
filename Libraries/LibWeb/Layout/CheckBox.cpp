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

CheckBox::CheckBox(DOM::Document& document, HTML::HTMLInputElement& element, CSS::ComputedProperties const& style)
    : ReplacedBox(document, element, style)
{
}

CheckBox::~CheckBox() = default;

RefPtr<Painting::Paintable> CheckBox::create_paintable() const
{
    return Painting::CheckBoxPaintable::create(*this);
}

}
