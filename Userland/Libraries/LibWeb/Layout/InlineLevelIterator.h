/*
 * Copyright (c) 2022, Andreas Kling <kling@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Noncopyable.h>
#include <LibWeb/Layout/BlockContainer.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/LayoutState.h>
#include <LibWeb/Layout/TextNode.h>

namespace Web::Layout {

// This class iterates over all the inline-level objects within an inline formatting context.
// By repeatedly calling next() with the remaining available width on the current line,
// it returns an "Item" representing the next piece of inline-level content to be placed on the line.
class InlineLevelIterator {
    AK_MAKE_NONCOPYABLE(InlineLevelIterator);
    AK_MAKE_NONMOVABLE(InlineLevelIterator);

public:
    struct Item {
        enum class Type {
            Text,
            Element,
            ForcedBreak,
            AbsolutelyPositionedElement,
            FloatingElement,
        };
        Type type {};
        JS::GCPtr<Layout::Node const> node {};
        RefPtr<Gfx::GlyphRun> glyph_run {};
        size_t offset_in_node { 0 };
        size_t length_in_node { 0 };
        CSSPixels width { 0.0f };
        CSSPixels padding_start { 0.0f };
        CSSPixels padding_end { 0.0f };
        CSSPixels border_start { 0.0f };
        CSSPixels border_end { 0.0f };
        CSSPixels margin_start { 0.0f };
        CSSPixels margin_end { 0.0f };
        bool is_collapsible_whitespace { false };

        CSSPixels border_box_width() const
        {
            return border_start + padding_start + width + padding_end + border_end;
        }
    };

    InlineLevelIterator(Layout::InlineFormattingContext&, LayoutState&, Layout::BlockContainer const& containing_block, LayoutState::UsedValues const& containing_block_used_values, LayoutMode);

    Optional<Item> next();
    CSSPixels next_non_whitespace_sequence_width();

private:
    Optional<Item> next_without_lookahead();
    Gfx::GlyphRun::TextType resolve_text_direction_from_context();
    void skip_to_next();
    void compute_next();

    void enter_text_node(Layout::TextNode const&);

    void enter_node_with_box_model_metrics(Layout::NodeWithStyleAndBoxModelMetrics const&);
    void exit_node_with_box_model_metrics();

    void add_extra_box_model_metrics_to_item(Item&, bool add_leading_metrics, bool add_trailing_metrics);

    Layout::Node const* next_inline_node_in_pre_order(Layout::Node const& current, Layout::Node const* stay_within);

    Layout::InlineFormattingContext& m_inline_formatting_context;
    Layout::LayoutState& m_layout_state;
    JS::NonnullGCPtr<BlockContainer const> m_containing_block;
    LayoutState::UsedValues const& m_containing_block_used_values;
    JS::GCPtr<Layout::Node const> m_current_node;
    JS::GCPtr<Layout::Node const> m_next_node;
    LayoutMode const m_layout_mode;

    struct TextNodeContext {
        bool do_collapse {};
        bool do_wrap_lines {};
        bool do_respect_linebreaks {};
        bool is_first_chunk {};
        bool is_last_chunk {};
        TextNode::ChunkIterator chunk_iterator;
        Optional<Gfx::GlyphRun::TextType> last_known_direction {};
    };

    Optional<TextNodeContext> m_text_node_context;

    struct ExtraBoxMetrics {
        CSSPixels margin { 0 };
        CSSPixels border { 0 };
        CSSPixels padding { 0 };
    };

    Optional<ExtraBoxMetrics> m_extra_leading_metrics;
    Optional<ExtraBoxMetrics> m_extra_trailing_metrics;

    Vector<JS::NonnullGCPtr<NodeWithStyleAndBoxModelMetrics const>> m_box_model_node_stack;
    Queue<InlineLevelIterator::Item> m_lookahead_items;
};

}
