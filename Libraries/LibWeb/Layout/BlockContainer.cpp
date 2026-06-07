/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Layout {

BlockContainer::BlockContainer(DOM::Document& document, DOM::Node* node, CSS::ComputedProperties const& style)
    : Box(document, node, style)
{
}

BlockContainer::BlockContainer(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : Box(document, node, move(computed_values))
{
}

BlockContainer::~BlockContainer() = default;

RefPtr<Painting::PaintableWithLines const> BlockContainer::paintable_with_lines() const
{
    auto paintable_box = Box::paintable_box();
    return as_if<Painting::PaintableWithLines>(paintable_box.ptr());
}

RefPtr<Painting::Paintable> BlockContainer::create_paintable() const
{
    return Painting::PaintableWithLines::create(*this);
}

}
