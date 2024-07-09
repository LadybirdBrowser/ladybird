/*
 * Copyright (c) 2018-2023, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/Range.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Layout {

JS_DEFINE_ALLOCATOR(Viewport);

Viewport::Viewport(DOM::Document& document, NonnullRefPtr<CSS::StyleProperties> style)
    : BlockContainer(document, &document, move(style))
{
}

Viewport::~Viewport() = default;

JS::GCPtr<Painting::Paintable> Viewport::create_paintable() const
{
    return Painting::ViewportPaintable::create(*this);
}

void Viewport::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    if (!m_text_blocks.has_value())
        return;

    for (auto& text_block : *m_text_blocks) {
        for (auto& text_position : text_block.positions)
            visitor.visit(text_position.dom_node);
    }
}

Vector<Viewport::TextBlock> const& Viewport::text_blocks()
{
    if (!m_text_blocks.has_value())
        update_text_blocks();

    return *m_text_blocks;
}

void Viewport::update_text_blocks()
{
    StringBuilder builder;
    size_t current_start_position = 0;
    Vector<TextPosition> text_positions;
    Vector<TextBlock> text_blocks;
    for_each_in_inclusive_subtree([&](auto const& layout_node) {
        if (layout_node.display().is_none() || !layout_node.paintable() || !layout_node.paintable()->is_visible())
            return TraversalDecision::Continue;

        if (layout_node.is_box() || layout_node.is_generated()) {
            if (!builder.is_empty()) {
                text_blocks.append({ builder.to_string_without_validation(), text_positions });
                current_start_position = 0;
                text_positions.clear_with_capacity();
                builder.clear();
            }
            return TraversalDecision::Continue;
        }

        if (layout_node.is_text_node()) {
            auto const& text_node = verify_cast<Layout::TextNode>(layout_node);
            auto& dom_node = const_cast<DOM::Text&>(text_node.dom_node());
            if (text_positions.is_empty()) {
                text_positions.empend(dom_node);
            } else {
                text_positions.empend(dom_node, current_start_position);
            }

            auto const& current_node_text = text_node.text_for_rendering();
            current_start_position += current_node_text.bytes_as_string_view().length();
            builder.append(move(current_node_text));
        }

        return TraversalDecision::Continue;
    });

    if (!builder.is_empty())
        text_blocks.append({ builder.to_string_without_validation(), text_positions });

    m_text_blocks = move(text_blocks);
}

}
