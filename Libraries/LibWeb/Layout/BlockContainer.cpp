/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Painting/PaintableWithLines.h>

namespace Web::Layout {

BlockContainer::BlockContainer(DOM::Document& document, GC::Ptr<DOM::Node> node, CSS::LayoutStyle style, RustFFI::NodeKind kind)
    : Box(document, node, move(style), kind)
{
    set_node_kind(kind);
}

BlockContainer::~BlockContainer() = default;

RefPtr<Painting::PaintableWithLines const> BlockContainer::paintable_with_lines() const
{
    auto paintable_box = Box::paintable_box();
    return as_if<Painting::PaintableWithLines>(paintable_box.ptr());
}

}
