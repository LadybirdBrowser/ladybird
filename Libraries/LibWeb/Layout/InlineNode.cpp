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

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(InlineNode);

InlineNode::InlineNode(DOM::Document& document, DOM::Element* element, GC::Ref<CSS::ComputedProperties> style)
    : Layout::NodeWithStyleAndBoxModelMetrics(document, element, move(style))
{
}

InlineNode::~InlineNode() = default;

GC::Ptr<Painting::PaintableWithLines> InlineNode::create_paintable_for_line_with_index(size_t line_index) const
{
    for (auto const& paintable : paintables()) {
        if (is<Painting::PaintableWithLines>(paintable)) {
            auto const& paintable_with_lines = static_cast<Painting::PaintableWithLines const&>(paintable);
            if (paintable_with_lines.line_index() == line_index) {
                return const_cast<Painting::PaintableWithLines&>(paintable_with_lines);
            }
        }
    }
    return Painting::PaintableWithLines::create(*this, line_index);
}

}
