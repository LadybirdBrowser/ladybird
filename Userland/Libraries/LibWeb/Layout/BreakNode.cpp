/*
 * Copyright (c) 2018-2020, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/InlineFormattingContext.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(BreakNode);

BreakNode::BreakNode(DOM::Document& document, HTML::HTMLBRElement& element, NonnullRefPtr<CSS::StyleProperties> style)
    : Layout::NodeWithStyleAndBoxModelMetrics(document, &element, move(style))
{
}

BreakNode::~BreakNode() = default;

}
