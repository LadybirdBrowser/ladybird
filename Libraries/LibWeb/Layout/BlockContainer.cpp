/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/PaintableBox.h>

namespace Web::Layout {

BlockContainer::BlockContainer(DOM::Document& document, DOM::Node* node, CSS::StyleProperties style)
    : Box(document, node, move(style))
{
}

BlockContainer::BlockContainer(DOM::Document& document, DOM::Node* node, NonnullOwnPtr<CSS::ComputedValues> computed_values)
    : Box(document, node, move(computed_values))
{
}

BlockContainer::~BlockContainer() = default;

Painting::PaintableWithLines const* BlockContainer::paintable_with_lines() const
{
    return static_cast<Painting::PaintableWithLines const*>(Box::paintable_box());
}

JS::GCPtr<Painting::Paintable> BlockContainer::create_paintable() const
{
    return Painting::PaintableWithLines::create(*this);
}

}
