/*
 * Copyright (c) 2025, Bohdan Sverdlov <freezar92@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/DecodedImageData.h>
#include <LibWeb/Layout/AnonymousImageBox.h>
#include <LibWeb/Painting/AnonymousImagePaintable.h>
#include <LibWeb/Platform/FontPlugin.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(AnonymousImageBox);

AnonymousImageBox::AnonymousImageBox(DOM::Document& document, DOM::Element& element, GC::Ref<CSS::ComputedProperties> style)
    : ReplacedBox(document, element, move(style))
{
}

AnonymousImageBox::~AnonymousImageBox() = default;

void AnonymousImageBox::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
}

GC::Ptr<Painting::Paintable> AnonymousImageBox::create_paintable() const
{
    return Painting::AnonymousImagePaintable::create(*this);
}

}
