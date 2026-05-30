/*
 * Copyright (c) 2018-2023, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/CharacterTypes.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/DOM/Range.h>
#include <LibWeb/Dump.h>
#include <LibWeb/Layout/TextNode.h>
#include <LibWeb/Layout/Viewport.h>
#include <LibWeb/Painting/PaintableBox.h>
#include <LibWeb/Painting/StackingContext.h>
#include <LibWeb/Painting/ViewportPaintable.h>

namespace Web::Layout {

GC_DEFINE_ALLOCATOR(Viewport);

Viewport::Viewport(DOM::Document& document, GC::Ref<CSS::ComputedProperties> style)
    : BlockContainer(document, &document, move(style))
{
}

Viewport::~Viewport() = default;

DOM::Document const& Viewport::dom_node() const
{
    return static_cast<DOM::Document const&>(*Node::dom_node());
}

RefPtr<Painting::Paintable> Viewport::create_paintable() const
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
    StringBuilder builder(StringBuilder::Mode::UTF16);
    Vector<TextPosition> text_positions;
    Vector<TextBlock> text_blocks;

    DOM::Text* pending_space_dom_node = nullptr;
    DOM::Text const* current_dom_node = nullptr;
    size_t pending_space_dom_offset = 0;
    size_t expected_dom_offset = 0;
    size_t builder_length_in_code_units = 0;

    auto flush_block = [&] {
        if (!builder.is_empty())
            text_blocks.append({ builder.to_utf16_string(), text_positions });
        text_positions.clear_with_capacity();
        builder.clear();
        builder_length_in_code_units = 0;
        pending_space_dom_node = nullptr;
        current_dom_node = nullptr;
    };

    auto append_code_unit = [&](DOM::Text& dom_node, char16_t code_unit, size_t dom_offset) {
        if (current_dom_node != &dom_node || dom_offset != expected_dom_offset)
            text_positions.empend(dom_node, builder_length_in_code_units, dom_offset);
        builder.append_code_unit(code_unit);
        builder_length_in_code_units++;
        current_dom_node = &dom_node;
        expected_dom_offset = dom_offset + 1;
    };

    for_each_in_inclusive_subtree([&](auto const& layout_node) {
        if (layout_node.display().is_none() || !layout_node.first_paintable() || !layout_node.first_paintable()->is_visible())
            return TraversalDecision::Continue;

        auto const pseudo = layout_node.generated_for_pseudo_element();
        auto const wraps_dom_text = pseudo == CSS::PseudoElement::FirstLetter;

        if (layout_node.is_box() || (pseudo.has_value() && !wraps_dom_text)) {
            flush_block();
            return TraversalDecision::Continue;
        }

        if (auto* text_node = as_if<Layout::TextNode>(layout_node)) {
            // https://html.spec.whatwg.org/multipage/interaction.html#inert-subtrees
            // When a node is inert:
            // - The user agent should ignore the node for the purposes of find-in-page.
            auto& dom_node = const_cast<DOM::Text&>(text_node->dom_node());
            if (dom_node.is_inert())
                return TraversalDecision::Continue;

            auto white_space_collapse = text_node->computed_values().white_space_collapse();
            auto const should_collapse = first_is_one_of(white_space_collapse,
                CSS::WhiteSpaceCollapse::Collapse,
                CSS::WhiteSpaceCollapse::PreserveBreaks);
            auto const dom_start_offset = text_node->dom_start_offset();
            auto const& text = text_node->text_for_rendering();
            auto const text_view = text.utf16_view();

            for (size_t i = 0; i < text_view.length_in_code_units(); ++i) {
                auto const code_unit = text_view.code_unit_at(i);
                auto const dom_offset = dom_start_offset + i;

                if (should_collapse && is_ascii_space(code_unit) && code_unit != '\n') {
                    if (!pending_space_dom_node) {
                        pending_space_dom_node = &dom_node;
                        pending_space_dom_offset = dom_offset;
                    }
                    continue;
                }

                if (pending_space_dom_node) {
                    if (!text_positions.is_empty())
                        append_code_unit(*pending_space_dom_node, ' ', pending_space_dom_offset);
                    pending_space_dom_node = nullptr;
                }

                append_code_unit(dom_node, code_unit, dom_offset);
            }
        }

        return TraversalDecision::Continue;
    });

    flush_block();

    m_text_blocks = move(text_blocks);
}

}
