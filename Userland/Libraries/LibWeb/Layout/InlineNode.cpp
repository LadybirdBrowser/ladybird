/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Painting/InlinePaintable.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(InlineNode);

InlineNode::InlineNode(DOM::Document& document, DOM::Element* element, NonnullRefPtr<CSS::StyleProperties> style)
    : Layout::NodeWithStyleAndBoxModelMetrics(document, element, move(style))
{
}

InlineNode::~InlineNode() = default;

JS::GCPtr<Painting::Paintable> InlineNode::create_paintable() const
{
    return Painting::InlinePaintable::create(*this);
}

}
