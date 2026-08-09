/*
 * Copyright (c) 2018-2020, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/BreakNode.h>

namespace Web::Layout {

BreakNode::BreakNode(DOM::Document& document, HTML::HTMLBRElement& element, CSS::LayoutStyle style)
    : Layout::NodeWithStyle(document, &element, style)
{
}

BreakNode::~BreakNode() = default;

}
