/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Tobias Christiansen <tobyase@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/ListItemBox.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>

namespace Web::Layout {

ListItemBox::ListItemBox(DOM::Document& document, DOM::Element* element, CSS::ComputedProperties const& style)
    : Layout::BlockContainer(document, element, style)
{
}

ListItemBox::~ListItemBox() = default;

void ListItemBox::set_marker(ListItemMarkerBox* marker)
{
    m_marker = marker;
}

RefPtr<Painting::Paintable> ListItemBox::create_paintable() const
{
    // A fragmented inline list item gets one paintable per line it spans, created via
    // create_paintable_for_line_with_index(), instead of a node-level paintable.
    if (is_fragmented_inline())
        return nullptr;
    return BlockContainer::create_paintable();
}

}
