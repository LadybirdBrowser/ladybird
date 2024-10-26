/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/InlineFormattingContext.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(BreakNode);

BreakNode::BreakNode(DOM::Document& document, HTML::HTMLBRElement& element, CSS::StyleProperties style)
    : Layout::NodeWithStyleAndBoxModelMetrics(document, &element, move(style))
{
}

BreakNode::~BreakNode() = default;

}
