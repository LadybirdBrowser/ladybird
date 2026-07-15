/*
 * Copyright (c) 2018-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2021, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Element.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Painting/InlinePaintable.h>

namespace Web::Layout {

InlineNode::InlineNode(DOM::Document& document, DOM::Element* element, NonnullRefPtr<CSS::ComputedValues const> style)
    : Layout::NodeWithStyleAndBoxModelMetrics(document, element, style)
{
}

InlineNode::~InlineNode() = default;

RefPtr<Painting::Paintable> InlineNode::create_paintable() const
{
    return Painting::InlinePaintable::create(*this);
}

}
