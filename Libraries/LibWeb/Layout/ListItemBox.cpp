/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Painting/InlinePaintable.h>

namespace Web::Layout {

ListItemBox::ListItemBox(DOM::Document& document, GC::Ptr<DOM::Element> element, CSS::LayoutStyle style)
    : Layout::BlockContainer(document, element, style)
{
}

ListItemBox::~ListItemBox() = default;

RefPtr<Painting::Paintable> ListItemBox::create_paintable() const
{
    if (is_fragmented_inline())
        return Painting::InlinePaintable::create(*this);
    return BlockContainer::create_paintable();
}

}
