/*
 * Copyright (c) 2022, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibWeb/DOM/ShadowRoot.h>
#include <LibWeb/HTML/FormAssociatedElement.h>
#include <LibWeb/Layout/BreakNode.h>
#include <LibWeb/Layout/InlineFormattingContext.h>
#include <LibWeb/Layout/InlineLevelIterator.h>
#include <LibWeb/Layout/InlineNode.h>
#include <LibWeb/Layout/ListItemMarkerBox.h>
#include <LibWeb/Layout/ReplacedBox.h>

namespace Web::Layout {

InlineLevelIterator::InlineLevelIterator(Layout::InlineFormattingContext& inline_formatting_context, Layout::LayoutState& layout_state, Layout::BlockContainer const& containing_block, LayoutState::UsedValues const& containing_block_used_values, LayoutMode layout_mode)
    : m_inline_formatting_context(inline_formatting_context)
    , m_layout_state(layout_state)
    , m_containing_block(containing_block)
    , m_containing_block_used_values(containing_block_used_values)
    , m_next_node(containing_block.first_child())
    , m_layout_mode(layout_mode)
{
    skip_to_next();
    generate_all_items();
}

void InlineLevelIterator::generate_all_items()
{
    for (;;) {
        auto item = generate_next_item();
        if (!item.has_value())
            break;

        // Track accumulated width for tab calculations.
        // Reset on forced breaks since tabs measure from line start.
        if (item->type == Item::Type::ForcedBreak) {
            m_accumulated_width_for_tabs = 0;
        } else {
            m_accumulated_width_for_tabs += item->border_box_width();
        }

        m_items.append(item.release_value());
    }
}

void InlineLevelIterator::enter_node_with_box_model_metrics(Layout::NodeWithStyleAndBoxModelMetrics const& node)
{
    if (!m_extra_leading_metrics.has_value())
        m_extra_leading_metrics = ExtraBoxMetrics {};

    // FIXME: It's really weird that *this* is where we assign box model metrics for these layout nodes..

    auto& used_values = m_layout_state.get_mutable(node);
    auto const& computed_values = node.computed_values();

    used_values.margin_top = computed_values.margin().top().to_px_or_zero(node, m_containing_block_used_values.content_width());
    used_values.margin_bottom = computed_values.margin().bottom().to_px_or_zero(node, m_containing_block_used_values.content_width());

    used_values.margin_left = computed_values.margin().left().to_px_or_zero(node, m_containing_block_used_values.content_width());
    used_values.border_left = computed_values.border_left().width;
    used_values.padding_left = computed_values.padding().left().to_px_or_zero(node, m_containing_block_used_values.content_width());

    used_values.margin_right = computed_values.margin().right().to_px_or_zero(node, m_containing_block_used_values.content_width());
    used_values.border_right = computed_values.border_right().width;
    used_values.padding_right = computed_values.padding().right().to_px_or_zero(node, m_containing_block_used_values.content_width());

    used_values.border_top = computed_values.border_top().width;
    used_values.border_bottom = computed_values.border_bottom().width;
    used_values.padding_bottom = computed_values.padding().bottom().to_px_or_zero(node, m_containing_block_used_values.content_width());
    used_values.padding_top = computed_values.padding().top().to_px_or_zero(node, m_containing_block_used_values.content_width());

    m_extra_leading_metrics->margin += used_values.margin_left;
    m_extra_leading_metrics->border += used_values.border_left;
    m_extra_leading_metrics->padding += used_values.padding_left;

    // Now's our chance to resolve the inset properties for this node.
    m_inline_formatting_context.compute_inset(node, m_inline_formatting_context.content_box_rect(m_containing_block_used_values).size());

    m_box_model_node_stack.append(node);
}

void InlineLevelIterator::exit_node_with_box_model_metrics()
{
    if (!m_extra_trailing_metrics.has_value())
        m_extra_trailing_metrics = ExtraBoxMetrics {};

    auto& node = m_box_model_node_stack.last();
    auto& used_values = m_layout_state.get_mutable(node);

    m_extra_trailing_metrics->margin += used_values.margin_right;
    m_extra_trailing_metrics->border += used_values.border_right;
    m_extra_trailing_metrics->padding += used_values.padding_right;

    m_box_model_node_stack.take_last();
}

// This is similar to Layout::Node::next_in_pre_order() but will not descend into inline-block nodes.
Layout::Node const* InlineLevelIterator::next_inline_node_in_pre_order(Layout::Node const& current, Layout::Node const* stay_within)
{
    if (current.first_child()
        && current.first_child()->display().is_inline_outside()
        && current.display().is_flow_inside()
        && !current.is_replaced_box()) {
        if (!current.is_box() || !static_cast<Box const&>(current).is_out_of_flow(m_inline_formatting_context))
            return current.first_child();
    }

    Layout::Node const* node = &current;
    Layout::Node const* next = nullptr;
    while (!(next = node->next_sibling())) {
        node = node->parent();

        // If node is the last node on the "box model node stack", pop it off.
        if (!m_box_model_node_stack.is_empty()
            && m_box_model_node_stack.last() == node) {
            exit_node_with_box_model_metrics();
        }
        if (!node || node == stay_within)
            return nullptr;
    }

    // If node is the last node on the "box model node stack", pop it off.
    if (!m_box_model_node_stack.is_empty()
        && m_box_model_node_stack.last() == node) {
        exit_node_with_box_model_metrics();
    }

    return next;
}

void InlineLevelIterator::compute_next()
{
    if (m_next_node == nullptr)
        return;
    do {
        m_next_node = next_inline_node_in_pre_order(*m_next_node, m_containing_block);
        if (m_next_node && m_next_node->is_svg_mask_box()) {
            // NOTE: It is possible to encounter SVGMaskBox nodes while doing layout of formatting context established by <foreignObject> with a mask.
            //       We should skip and let SVGFormattingContext take care of them.
            m_next_node = m_next_node->next_sibling();
        }
    } while (m_next_node && (!m_next_node->is_inline() && !m_next_node->is_out_of_flow(m_inline_formatting_context)));
}

void InlineLevelIterator::skip_to_next()
{
    if (m_next_node
        && is<Layout::NodeWithStyleAndBoxModelMetrics>(*m_next_node)
        && m_next_node->display().is_flow_inside()
        && !m_next_node->is_out_of_flow(m_inline_formatting_context)
        && !m_next_node->is_replaced_box())
        enter_node_with_box_model_metrics(static_cast<Layout::NodeWithStyleAndBoxModelMetrics const&>(*m_next_node));

    m_current_node = m_next_node;
    compute_next();
}

Optional<InlineLevelIterator::Item> InlineLevelIterator::next()
{
    if (m_next_item_index >= m_items.size())
        return {};
    return m_items[m_next_item_index++];
}

CSSPixels InlineLevelIterator::next_non_whitespace_sequence_width()
{
    CSSPixels next_width = 0;
    for (size_t i = m_next_item_index; i < m_items.size(); ++i) {
        auto const& next_item = m_items[i];
        if (next_item.type == InlineLevelIterator::Item::Type::ForcedBreak)
            break;
        if (next_item.node->computed_values().text_wrap_mode() == CSS::TextWrapMode::Wrap) {
            if (next_item.type != InlineLevelIterator::Item::Type::Text)
                break;
            if (next_item.is_collapsible_whitespace)
                break;
            auto const& next_text_node = as<Layout::TextNode>(*(next_item.node));
            auto next_view = next_text_node.text_for_rendering().substring_view(next_item.offset_in_node, next_item.length_in_node);
            if (next_view.is_ascii_whitespace())
                break;
        }
        next_width += next_item.border_box_width();
    }
    return next_width;
}

Gfx::GlyphRun::TextType InlineLevelIterator::resolve_text_direction_from_context()
{
    VERIFY(m_text_node_context.has_value());

    // Search forward in the pre-generated chunks array to find the next chunk with known direction.
    // Since chunks are pre-generated, this is just O(1) array access per iteration.
    Optional<Gfx::GlyphRun::TextType> next_known_direction;
    for (size_t i = m_text_node_context->next_chunk_index; i < m_text_node_context->chunks.size(); ++i) {
        auto const& chunk = m_text_node_context->chunks[i];
        if (chunk.text_type == Gfx::GlyphRun::TextType::Ltr || chunk.text_type == Gfx::GlyphRun::TextType::Rtl) {
            next_known_direction = chunk.text_type;
            break;
        }
    }

    auto last_known_direction = m_text_node_context->last_known_direction;

    if (last_known_direction.has_value() && next_known_direction.has_value() && *last_known_direction != *next_known_direction) {
        switch (m_containing_block->computed_values().direction()) {
        case CSS::Direction::Ltr:
            return Gfx::GlyphRun::TextType::Ltr;
        case CSS::Direction::Rtl:
            return Gfx::GlyphRun::TextType::Rtl;
        }
    }

    if (last_known_direction.has_value())
        return *last_known_direction;
    if (next_known_direction.has_value())
        return *next_known_direction;

    return Gfx::GlyphRun::TextType::ContextDependent;
}

Optional<InlineLevelIterator::Item> InlineLevelIterator::generate_next_item()
{
    if (!m_current_node)
        return {};

    if (auto* text_node = as_if<Layout::TextNode>(*m_current_node)) {
        if (!m_text_node_context.has_value())
            enter_text_node(*text_node);

        // Track chunk position locally
        bool is_first_chunk = (m_text_node_context->next_chunk_index == 0);

        // Get the next chunk from the pre-generated array
        Optional<TextNode::Chunk> chunk_opt;
        if (m_text_node_context->next_chunk_index < m_text_node_context->chunks.size()) {
            chunk_opt = m_text_node_context->chunks[m_text_node_context->next_chunk_index++];
        }

        bool is_last_chunk = (m_text_node_context->next_chunk_index >= m_text_node_context->chunks.size());

        auto is_empty_editable = false;
        if (!chunk_opt.has_value()) {
            auto const is_only_chunk = is_first_chunk && is_last_chunk;
            if (is_only_chunk && text_node->text_for_rendering().is_empty()) {
                if (auto const* shadow_root = as_if<DOM::ShadowRoot>(text_node->dom_node().root()))
                    if (auto const* form_associated_element = as_if<HTML::FormAssociatedTextControlElement>(shadow_root->host()))
                        is_empty_editable = form_associated_element->is_mutable();
                is_empty_editable |= text_node->dom_node().parent() && text_node->dom_node().parent()->is_editing_host();
            }

            if (is_empty_editable) {
                // Create an empty chunk for editable empty text fields
                chunk_opt = TextNode::Chunk {
                    .view = {},
                    .font = text_node->computed_values().font_list().first(),
                    .is_all_whitespace = true,
                    .text_type = Gfx::GlyphRun::TextType::Common,
                };
                // Advance the index so the next call will move to the next node
                m_text_node_context->next_chunk_index = 1;
            } else {
                m_text_node_context = {};
                m_previous_chunk_can_break_after = false;
                skip_to_next();
                return generate_next_item();
            }
        }

        auto& chunk = chunk_opt.value();
        auto text_type = chunk.text_type;
        if (text_type == Gfx::GlyphRun::TextType::Ltr || text_type == Gfx::GlyphRun::TextType::Rtl) {
            m_text_node_context->last_known_direction = text_type;
        }

        auto do_respect_linebreak = m_text_node_context->should_respect_linebreaks;
        if (do_respect_linebreak && chunk.has_breaking_newline) {
            is_last_chunk = true;
            if (chunk.is_all_whitespace)
                text_type = Gfx::GlyphRun::TextType::EndPadding;
        }

        if (text_type == Gfx::GlyphRun::TextType::ContextDependent)
            text_type = resolve_text_direction_from_context();

        if (do_respect_linebreak && chunk.has_breaking_newline)
            return Item { .type = Item::Type::ForcedBreak };

        auto letter_spacing = text_node->computed_values().letter_spacing();
        // FIXME: We should apply word spacing to all word-separator characters not just breaking tabs
        auto word_spacing = text_node->computed_values().word_spacing();

        auto x = 0.0f;
        if (chunk.has_breaking_tab) {
            // Use the accumulated width we've been tracking during pre-generation.
            // This accounts for items that would appear before this tab on the same line.
            CSSPixels accumulated_width = m_accumulated_width_for_tabs;

            // https://drafts.csswg.org/css-text/#tab-size-property
            auto tab_width = text_node->computed_values().tab_size().visit(
                [&](CSS::Length const& length) -> CSSPixels {
                    return length.absolute_length_to_px();
                },
                [&](double tab_number) -> CSSPixels {
                    return CSSPixels::nearest_value_for(tab_number * (chunk.font->glyph_width(' ') + word_spacing.to_float() + letter_spacing.to_float()));
                });

            // https://drafts.csswg.org/css-text/#white-space-phase-2
            // if fragments have added to the width, calculate the net distance to the next tab stop, otherwise the shift will just be the tab width
            auto tab_stop_dist = accumulated_width > 0 ? (ceil((accumulated_width / tab_width)) * tab_width) - accumulated_width : tab_width;
            auto ch_width = chunk.font->glyph_width('0');

            // If this distance is less than 0.5ch, then the subsequent tab stop is used instead
            if (tab_stop_dist < ch_width * 0.5)
                tab_stop_dist += tab_width;

            // account for consecutive tabs
            auto num_of_tabs = 0;
            for (auto code_point : chunk.view) {
                if (code_point != '\t')
                    break;
                num_of_tabs++;
            }
            tab_stop_dist = tab_stop_dist * num_of_tabs;

            // remove tabs, we don't want to render them when we shape the text
            chunk.view = chunk.view.substring_view(num_of_tabs);
            x = tab_stop_dist.to_float();
        }

        auto glyph_run = Gfx::shape_text({ x, 0 }, letter_spacing.to_float(), chunk.view, chunk.font, text_type);

        CSSPixels chunk_width = CSSPixels::nearest_value_for(glyph_run->width() + x);

        // NOTE: We never consider `content: ""` to be collapsible whitespace.
        bool is_generated_empty_string = is_empty_editable || (text_node->is_generated_for_pseudo_element() && chunk.length == 0);
        auto collapse_whitespace = m_text_node_context->should_collapse_whitespace;

        Item item {
            .type = Item::Type::Text,
            .node = text_node,
            .glyph_run = move(glyph_run),
            .offset_in_node = chunk.start,
            .length_in_node = chunk.length,
            .width = chunk_width,
            .is_collapsible_whitespace = collapse_whitespace && chunk.is_all_whitespace && !is_generated_empty_string,
            .can_break_before = m_previous_chunk_can_break_after,
        };

        m_previous_chunk_can_break_after = chunk.can_break_after;

        add_extra_box_model_metrics_to_item(item, is_first_chunk, is_last_chunk);
        return item;
    }

    if (m_current_node->is_absolutely_positioned()) {
        auto const& node = *m_current_node;
        skip_to_next();
        return Item {
            .type = Item::Type::AbsolutelyPositionedElement,
            .node = &node,
        };
    }

    if (m_current_node->is_floating()) {
        auto const& node = *m_current_node;
        skip_to_next();
        return Item {
            .type = Item::Type::FloatingElement,
            .node = &node,
        };
    }

    if (is<Layout::BreakNode>(*m_current_node)) {
        auto const& node = *m_current_node;
        skip_to_next();
        return Item {
            .type = Item::Type::ForcedBreak,
            .node = &node,
        };
    }

    if (is<Layout::ListItemMarkerBox>(*m_current_node)) {
        skip_to_next();
        return generate_next_item();
    }

    if (!is<Layout::Box>(*m_current_node)) {
        skip_to_next();
        return generate_next_item();
    }

    auto const& box = as<Layout::Box>(*m_current_node);
    auto const& box_state = m_layout_state.get(box);
    m_inline_formatting_context.dimension_box_on_line(box, m_layout_mode);

    auto item = Item {
        .type = Item::Type::Element,
        .node = &box,
        .offset_in_node = 0,
        .length_in_node = 0,
        .width = box_state.content_width(),
        .padding_start = box_state.padding_left,
        .padding_end = box_state.padding_right,
        .border_start = box_state.border_left,
        .border_end = box_state.border_right,
        .margin_start = box_state.margin_left,
        .margin_end = box_state.margin_right,
    };
    add_extra_box_model_metrics_to_item(item, true, true);
    skip_to_next();
    return item;
}

void InlineLevelIterator::enter_text_node(Layout::TextNode const& text_node)
{
    auto white_space_collapse = text_node.computed_values().white_space_collapse();
    auto text_wrap_mode = text_node.computed_values().text_wrap_mode();

    // https://drafts.csswg.org/css-text-4/#collapse
    bool do_wrap_lines = text_wrap_mode == CSS::TextWrapMode::Wrap;
    bool do_respect_linebreaks = first_is_one_of(white_space_collapse, CSS::WhiteSpaceCollapse::Preserve, CSS::WhiteSpaceCollapse::PreserveBreaks, CSS::WhiteSpaceCollapse::BreakSpaces);

    // Pre-generate all chunks for this text node upfront.
    // This allows O(1) peek() and next() operations instead of lazy generation with O(n) queue operations.
    TextNode::ChunkIterator chunk_iterator { text_node, do_wrap_lines, do_respect_linebreaks };
    Vector<TextNode::Chunk> chunks;
    while (true) {
        auto chunk = chunk_iterator.next();
        if (!chunk.has_value())
            break;
        chunks.append(chunk.release_value());
    }

    m_text_node_context = TextNodeContext {
        .chunks = move(chunks),
        .next_chunk_index = 0,
        .should_collapse_whitespace = chunk_iterator.should_collapse_whitespace(),
        .should_wrap_lines = do_wrap_lines,
        .should_respect_linebreaks = do_respect_linebreaks,
    };
}

void InlineLevelIterator::add_extra_box_model_metrics_to_item(Item& item, bool add_leading_metrics, bool add_trailing_metrics)
{
    if (add_leading_metrics && m_extra_leading_metrics.has_value()) {
        item.margin_start += m_extra_leading_metrics->margin;
        item.border_start += m_extra_leading_metrics->border;
        item.padding_start += m_extra_leading_metrics->padding;
        m_extra_leading_metrics = {};
    }

    if (add_trailing_metrics && m_extra_trailing_metrics.has_value()) {
        item.margin_end += m_extra_trailing_metrics->margin;
        item.border_end += m_extra_trailing_metrics->border;
        item.padding_end += m_extra_trailing_metrics->padding;
        m_extra_trailing_metrics = {};
    }
}

}
