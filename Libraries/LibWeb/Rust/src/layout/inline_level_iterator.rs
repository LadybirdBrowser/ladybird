/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ItemType {
    Text,
    Element,
    BlockLevelBox,
    ForcedBreak,
    AbsolutelyPositionedElement,
    FloatingElement,
}

#[derive(Clone, Debug)]
pub(crate) struct Item {
    pub(crate) type_: ItemType,
    pub(crate) node: Node,
    pub(crate) glyphs: Option<line_box_fragment::GlyphData>,
    pub(crate) offset_in_node: usize,
    pub(crate) length_in_node: usize,
    pub(crate) inline_size: CssPixels,
    pub(crate) min_content_inline_size: Option<CssPixels>,
    pub(crate) padding_start: CssPixels,
    pub(crate) padding_end: CssPixels,
    pub(crate) border_start: CssPixels,
    pub(crate) border_end: CssPixels,
    pub(crate) margin_start: CssPixels,
    pub(crate) margin_end: CssPixels,
    pub(crate) is_collapsible_whitespace: bool,
    pub(crate) can_break_before: bool,
    pub(crate) preceded_by_unattached_inline_start_edges: bool,
    pub(crate) content_baselines: DerivedBaselines,
    pub(crate) trailing_whitespace: line_box_fragment::TrailingWhitespace,
}

impl Item {
    fn new(type_: ItemType, node: Node) -> Self {
        Self {
            type_,
            node,
            glyphs: None,
            offset_in_node: 0,
            length_in_node: 0,
            inline_size: CssPixels::default(),
            min_content_inline_size: None,
            padding_start: CssPixels::default(),
            padding_end: CssPixels::default(),
            border_start: CssPixels::default(),
            border_end: CssPixels::default(),
            margin_start: CssPixels::default(),
            margin_end: CssPixels::default(),
            is_collapsible_whitespace: false,
            can_break_before: false,
            preceded_by_unattached_inline_start_edges: false,
            content_baselines: DerivedBaselines::default(),
            trailing_whitespace: line_box_fragment::TrailingWhitespace::default(),
        }
    }

    pub(crate) fn border_box_inline_size(&self) -> CssPixels {
        self.border_start + self.padding_start + self.inline_size + self.padding_end + self.border_end
    }

    pub(crate) fn has_box_model_metrics(&self) -> bool {
        self.margin_start != CssPixels::default()
            || self.border_start != CssPixels::default()
            || self.padding_start != CssPixels::default()
            || self.padding_end != CssPixels::default()
            || self.border_end != CssPixels::default()
            || self.margin_end != CssPixels::default()
    }

    pub(crate) fn is_ascii_whitespace(&self, context: &inline_formatting_context::InlineFormattingContext<'_>) -> bool {
        assert_eq!(self.type_, ItemType::Text);
        let text = &context.callbacks.text_content(self.node).text;
        text[self.offset_in_node..self.offset_in_node + self.length_in_node]
            .iter()
            .all(|unit| *unit <= 0x7f && (*unit as u8).is_ascii_whitespace())
    }

    pub(crate) fn contains_tab(&self, context: &inline_formatting_context::InlineFormattingContext<'_>) -> bool {
        assert_eq!(self.type_, ItemType::Text);
        let text = &context.callbacks.text_content(self.node).text;
        text[self.offset_in_node..self.offset_in_node + self.length_in_node].contains(&(b'\t' as u16))
    }
}

#[derive(Clone, Copy, Default)]
struct ExtraBoxMetrics {
    margin: CssPixels,
    border: CssPixels,
    padding: CssPixels,
}

#[derive(Clone, Copy)]
struct TextNodeContext {
    chunks: &'static [text_chunker::TextChunk],
    text: &'static [u16],
    next_chunk_index: usize,
    should_collapse_whitespace: bool,
    should_respect_linebreaks: bool,
    last_known_direction: Option<u8>,
}

struct InlineLevelIteratorGenerator<'iterator, 'context> {
    context: &'iterator mut inline_formatting_context::InlineFormattingContext<'context>,
    current_node: Node,
    next_node: Node,
    text_node_context: Option<TextNodeContext>,
    is_unidirectional_left_to_right: bool,
    extra_leading_metrics: Option<ExtraBoxMetrics>,
    extra_trailing_metrics: Option<ExtraBoxMetrics>,
    box_model_node_stack: Vec<Node>,
    visited_fragmented_inlines: Vec<Node>,
    items: Vec<Item>,
    next_item_index: usize,
    accumulated_inline_size_for_tabs: CssPixels,
    previous_chunk_can_break_after: bool,
}

impl<'iterator, 'context> InlineLevelIteratorGenerator<'iterator, 'context> {
    fn generate(
        context: &'iterator mut inline_formatting_context::InlineFormattingContext<'context>,
    ) -> InlineLevelIterator {
        let containing_block = context.containing_block;
        let next_node = context.first_child(containing_block);
        let mut iterator = Self {
            context,
            next_node,
            current_node: NodeSlotId::INVALID,
            text_node_context: None,
            is_unidirectional_left_to_right: false,
            extra_leading_metrics: None,
            extra_trailing_metrics: None,
            box_model_node_stack: Vec::new(),
            visited_fragmented_inlines: Vec::new(),
            items: Vec::new(),
            next_item_index: 0,
            accumulated_inline_size_for_tabs: CssPixels::default(),
            previous_chunk_can_break_after: false,
        };
        iterator.is_unidirectional_left_to_right = iterator.compute_is_unidirectional_left_to_right();
        iterator.skip_to_next();
        iterator.generate_all_items();
        InlineLevelIterator {
            visited_fragmented_inlines: iterator.visited_fragmented_inlines,
            items: iterator.items,
            next_item_index: iterator.next_item_index,
        }
    }

    fn context(&self) -> &inline_formatting_context::InlineFormattingContext<'context> {
        self.context
    }

    fn context_mut(&mut self) -> &mut inline_formatting_context::InlineFormattingContext<'context> {
        self.context
    }

    fn is_out_of_flow(&self, node: Node) -> bool {
        self.context().facts(node).is_floating_or_absolutely_positioned()
    }

    fn compute_is_unidirectional_left_to_right(&mut self) -> bool {
        let containing_block = self.context().containing_block;
        if self.context().style(containing_block).direction() == direction::RTL {
            return false;
        }

        let mut node = self.context().first_child(containing_block);
        while !node.is_invalid() {
            let facts = self.context().facts(node);
            if !facts.is_text_node() {
                let style = self.context().style(node);
                if style.direction() == direction::RTL || style.unicode_bidi() != unicode_bidi::NORMAL {
                    return false;
                }
            } else if self.context().text_may_require_bidi_processing(node) {
                return false;
            }

            loop {
                node = self.next_inline_node_in_pre_order(node, containing_block);
                if !node.is_invalid() && self.context().facts(node).is_svg_mask_box() {
                    node = self.context().next_sibling(node);
                }
                if node.is_invalid() {
                    break;
                }
                let facts = self.context().facts(node);
                if facts.is_inline() || self.is_out_of_flow(node) || facts.is_inline_flow_interrupting_block() {
                    break;
                }
            }
        }
        true
    }

    fn generate_all_items(&mut self) {
        while let Some(item) = self.generate_next_item() {
            if matches!(item.type_, ItemType::ForcedBreak | ItemType::BlockLevelBox) {
                self.accumulated_inline_size_for_tabs = CssPixels::default();
            } else {
                self.accumulated_inline_size_for_tabs += item.border_box_inline_size();
            }
            self.items.push(item);
        }
    }

    fn enter_node_with_box_model_metrics(&mut self, node: Node) {
        if self.context().facts(node).is_fragmented_inline() {
            self.visited_fragmented_inlines.push(node);
        }
        let constraints = self.context().input.containing_block_constraints;
        let used = self.context().create_used_values(node, constraints);
        let style = self.context().style(node);
        let basis = constraints.inline_basis();
        used.margin_top.set(style.margin_top().to_px(basis));
        used.margin_bottom.set(style.margin_bottom().to_px(basis));
        used.margin_left.set(style.margin_left().to_px(basis));
        used.border_left.set(style.border_left_width());
        used.padding_left.set(style.padding_left().to_px(basis));
        used.margin_right.set(style.margin_right().to_px(basis));
        used.border_right.set(style.border_right_width());
        used.padding_right.set(style.padding_right().to_px(basis));
        used.border_top.set(style.border_top_width());
        used.border_bottom.set(style.border_bottom_width());
        used.padding_bottom.set(style.padding_bottom().to_px(basis));
        used.padding_top.set(style.padding_top().to_px(basis));

        let leading = self.extra_leading_metrics.get_or_insert_default();
        leading.margin += used.margin_left.get();
        leading.border += used.border_left.get();
        leading.padding += used.padding_left.get();
        self.context().compute_inset(node);
        if self.context().run.fragments.is_some() {
            formatting_context::place_child(self.context().run, node, FfiCssPixelPoint::default(), None);
        }
        self.box_model_node_stack.push(node);
    }

    fn exit_node_with_box_model_metrics(&mut self) {
        let node = self.box_model_node_stack.pop().unwrap();
        let used = self.context().used(node);
        let margin = used.margin_right.get();
        let border = used.border_right.get();
        let padding = used.padding_right.get();
        let trailing = self.extra_trailing_metrics.get_or_insert_default();
        trailing.margin += margin;
        trailing.border += border;
        trailing.padding += padding;
    }

    fn next_inline_node_in_pre_order(&mut self, current: Node, stay_within: Node) -> Node {
        let first_child = self.context().first_child(current);
        let current_facts = self.context().facts(current);
        if !first_child.is_invalid() {
            let child_facts = self.context().facts(first_child);
            let current_style = self.context().style(current);
            if (child_facts.is_inline()
                || child_facts.is_inline_flow_interrupting_block()
                || self.is_out_of_flow(first_child))
                && current_style.display().is_flow_inside()
                && !current_facts.is_inline_flow_interrupting_block()
                && !current_facts.is_atomic_inline()
                && (!current_facts.is_box() || !self.is_out_of_flow(current))
            {
                return first_child;
            }
        }

        let mut node = current;
        loop {
            let next = self.context().next_sibling(node);
            if !next.is_invalid() {
                if self.box_model_node_stack.last().copied() == Some(node) {
                    self.exit_node_with_box_model_metrics();
                }
                return next;
            }
            node = self.context().parent_node(node);
            if self.box_model_node_stack.last().copied() == Some(node) {
                self.exit_node_with_box_model_metrics();
            }
            if node.is_invalid() || node == stay_within {
                return NodeSlotId::INVALID;
            }
        }
    }

    fn compute_next(&mut self) {
        if self.next_node.is_invalid() {
            return;
        }
        loop {
            self.next_node = self.next_inline_node_in_pre_order(self.next_node, self.context().containing_block);
            if !self.next_node.is_invalid() && self.context().facts(self.next_node).is_svg_mask_box() {
                self.next_node = self.context().next_sibling(self.next_node);
            }
            if self.next_node.is_invalid() {
                break;
            }
            let facts = self.context().facts(self.next_node);
            if facts.is_inline() || self.is_out_of_flow(self.next_node) || facts.is_inline_flow_interrupting_block() {
                break;
            }
        }
    }

    fn skip_to_next(&mut self) {
        if !self.next_node.is_invalid() {
            let facts = self.context().facts(self.next_node);
            if facts.is_inline()
                && facts.has_box_model_metrics()
                && !facts.is_break_node()
                && self.context().style(self.next_node).display().is_flow_inside()
                && !self.is_out_of_flow(self.next_node)
                && !facts.is_atomic_inline()
            {
                self.enter_node_with_box_model_metrics(self.next_node);
            }
        }
        self.current_node = self.next_node;
        self.compute_next();
    }

    fn enter_text_node(&mut self, text_node: Node) {
        let style = self.context().style(self.context().parent_node(text_node));
        let should_wrap_lines = style.text_wrap_mode() == text_wrap_mode::WRAP;
        let should_respect_linebreaks = matches!(
            style.white_space_collapse(),
            white_space_collapse::PRESERVE | white_space_collapse::PRESERVE_BREAKS | white_space_collapse::BREAK_SPACES
        );
        let should_collapse_whitespace = matches!(
            style.white_space_collapse(),
            white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
        );
        let callbacks = self.context().callbacks;
        let chunks = text_chunker::text_chunks(
            &callbacks,
            text_node,
            should_wrap_lines,
            should_respect_linebreaks,
            self.is_unidirectional_left_to_right,
        );
        self.text_node_context = Some(TextNodeContext {
            chunks,
            text: &callbacks.text_content(text_node).text,
            next_chunk_index: 0,
            should_collapse_whitespace,
            should_respect_linebreaks,
            last_known_direction: None,
        });
    }

    fn resolve_text_direction_from_context(&self) -> u8 {
        let context = self.text_node_context.unwrap();
        let next_known_direction = context.chunks[context.next_chunk_index..].iter().find_map(|chunk| {
            matches!(
                chunk.text_type,
                line_box_fragment::GLYPH_TEXT_TYPE_LTR | line_box_fragment::GLYPH_TEXT_TYPE_RTL
            )
            .then_some(chunk.text_type)
        });
        if let (Some(last), Some(next)) = (context.last_known_direction, next_known_direction)
            && last != next
        {
            return match self.context().style(self.context().containing_block).direction() {
                direction::LTR => line_box_fragment::GLYPH_TEXT_TYPE_LTR,
                direction::RTL => line_box_fragment::GLYPH_TEXT_TYPE_RTL,
                _ => unreachable!(),
            };
        }
        context
            .last_known_direction
            .or(next_known_direction)
            .unwrap_or(line_box_fragment::GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT)
    }

    fn shape_text(
        &mut self,
        text: &[u16],
        font: *const c_void,
        text_type: u8,
        baseline_start_x: f32,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> (line_box_fragment::GlyphData, line_box_fragment::TrailingWhitespace) {
        let shaped = font::shape_text_with_font(font, text, text_type, baseline_start_x, letter_spacing, word_spacing);
        let glyph_data = line_box_fragment::GlyphData {
            glyphs: shaped.glyphs,
            font,
            text_type,
            width: shaped.width,
        };
        let trailing_whitespace = line_box_fragment::TrailingWhitespace {
            length_in_code_units: shaped.trailing_whitespace_length_in_code_units,
            inline_size: CssPixels::nearest_value_for_f32(shaped.trailing_whitespace_advance),
        };
        (glyph_data, trailing_whitespace)
    }

    fn add_extra_box_model_metrics_to_item(
        &mut self,
        item: &mut Item,
        add_leading_metrics: bool,
        add_trailing_metrics: bool,
    ) {
        if add_leading_metrics && let Some(extra) = self.extra_leading_metrics.take() {
            item.margin_start += extra.margin;
            item.border_start += extra.border;
            item.padding_start += extra.padding;
        }
        if add_trailing_metrics && let Some(extra) = self.extra_trailing_metrics.take() {
            item.margin_end += extra.margin;
            item.border_end += extra.border;
            item.padding_end += extra.padding;
        }
    }

    fn generate_text_item(&mut self, text_node: Node) -> Option<Item> {
        if self.text_node_context.is_none() {
            self.enter_text_node(text_node);
        }
        let mut text_context = self.text_node_context.unwrap();
        let chunks = text_context.chunks;
        let is_first_chunk = text_context.next_chunk_index == 0;
        let chunk = chunks.get(text_context.next_chunk_index).copied();
        if chunk.is_some() {
            text_context.next_chunk_index += 1;
        }
        let mut is_last_chunk = text_context.next_chunk_index >= chunks.len();
        let synthesize_zero_length_chunk = chunk.is_none()
            && is_first_chunk
            && is_last_chunk
            && text_context.text.is_empty()
            && self.context().facts(text_node).produces_line_box_fragment_when_empty();
        let chunk = if let Some(chunk) = chunk {
            chunk
        } else if synthesize_zero_length_chunk {
            text_context.next_chunk_index = 1;
            let parent_style = self.context().style(self.context().parent_node(text_node));
            text_chunker::TextChunk {
                start: 0,
                length: 0,
                font: parent_style.first_available_font(),
                has_breaking_newline: false,
                has_breaking_tab: false,
                is_all_whitespace: true,
                can_break_after: false,
                text_type: line_box_fragment::GLYPH_TEXT_TYPE_COMMON,
            }
        } else {
            self.text_node_context = None;
            self.previous_chunk_can_break_after = false;
            self.skip_to_next();
            return self.generate_next_item();
        };

        let mut text_type = chunk.text_type;
        if matches!(
            text_type,
            line_box_fragment::GLYPH_TEXT_TYPE_LTR | line_box_fragment::GLYPH_TEXT_TYPE_RTL
        ) {
            text_context.last_known_direction = Some(text_type);
        }
        if text_context.should_respect_linebreaks && chunk.has_breaking_newline {
            is_last_chunk = true;
            if chunk.is_all_whitespace {
                text_type = line_box_fragment::GLYPH_TEXT_TYPE_END_PADDING;
            }
        }
        self.text_node_context = Some(text_context);
        if text_type == line_box_fragment::GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT {
            text_type = self.resolve_text_direction_from_context();
        }
        if text_context.should_respect_linebreaks && chunk.has_breaking_newline {
            return Some(Item::new(ItemType::ForcedBreak, NodeSlotId::INVALID));
        }

        let style = self.context().style(self.context().parent_node(text_node));
        let mut inline_offset = 0.0f32;
        let full_text = text_context.text;
        let mut shaped_start = chunk.start;
        let mut shaped_length = chunk.length;
        let word_spacing = style.word_spacing();
        if chunk.has_breaking_tab {
            let tab_inline_size = if style.tab_size_is_number() {
                let space = font::font_glyph_width(chunk.font, b' ' as u32);
                CssPixels::nearest_value_for(
                    style.tab_size_number()
                        * (space + word_spacing.to_double() as f32 + style.letter_spacing().to_double() as f32) as f64,
                )
            } else {
                style.tab_size()
            };
            let accumulated = self.accumulated_inline_size_for_tabs;
            let mut tab_stop_distance = if accumulated > CssPixels::default() {
                accumulated.div_as_fraction(tab_inline_size).ceil() * tab_inline_size - accumulated
            } else {
                tab_inline_size
            };
            let zero_width = font::font_glyph_width(chunk.font, b'0' as u32);
            if tab_stop_distance.to_double() < f64::from(zero_width) * 0.5 {
                tab_stop_distance += tab_inline_size;
            }
            let tab_count = full_text[chunk.start..chunk.start + chunk.length]
                .iter()
                .take_while(|unit| **unit == b'\t' as u16)
                .count();
            tab_stop_distance = tab_stop_distance * tab_count;
            shaped_start += tab_count;
            shaped_length -= tab_count;
            inline_offset = tab_stop_distance.to_double() as f32;
        }
        let shaped_text = &full_text[shaped_start..shaped_start + shaped_length];
        let (glyphs, shaped_trailing_whitespace) = self.shape_text(
            shaped_text,
            chunk.font,
            text_type,
            inline_offset,
            style.letter_spacing().to_double() as f32,
            word_spacing.to_double() as f32,
        );
        let chunk_inline_size = CssPixels::nearest_value_for_f32(glyphs.width + inline_offset);
        let generated_empty = synthesize_zero_length_chunk
            || (self.context().facts(text_node).is_generated_for_pseudo_element() && chunk.length == 0);
        let mut item = Item::new(ItemType::Text, text_node);
        item.glyphs = Some(glyphs);
        item.offset_in_node = chunk.start;
        item.length_in_node = chunk.length;
        item.inline_size = chunk_inline_size;
        item.trailing_whitespace = if chunk.is_all_whitespace {
            line_box_fragment::TrailingWhitespace {
                length_in_code_units: chunk.length,
                inline_size: chunk_inline_size,
            }
        } else {
            shaped_trailing_whitespace
        };
        item.is_collapsible_whitespace =
            text_context.should_collapse_whitespace && chunk.is_all_whitespace && !generated_empty;
        item.can_break_before = self.previous_chunk_can_break_after;
        self.previous_chunk_can_break_after = chunk.can_break_after;
        self.add_extra_box_model_metrics_to_item(&mut item, is_first_chunk, is_last_chunk);
        Some(item)
    }

    fn generate_next_item(&mut self) -> Option<Item> {
        if self.current_node.is_invalid() {
            return None;
        }
        let node = self.current_node;
        let facts = self.context().facts(node);
        if facts.is_text_node() {
            return self.generate_text_item(node);
        }
        if facts.is_absolutely_positioned() {
            let preceded = self.extra_leading_metrics.is_some_and(|extra| {
                extra.margin != CssPixels::default()
                    || extra.border != CssPixels::default()
                    || extra.padding != CssPixels::default()
            });
            self.skip_to_next();
            let mut item = Item::new(ItemType::AbsolutelyPositionedElement, node);
            item.preceded_by_unattached_inline_start_edges = preceded;
            return Some(item);
        }
        if facts.is_floating() {
            self.skip_to_next();
            return Some(Item::new(ItemType::FloatingElement, node));
        }
        if facts.is_break_node() {
            self.skip_to_next();
            return Some(Item::new(ItemType::ForcedBreak, node));
        }
        if facts.is_fragmented_inline() {
            self.skip_to_next();
            return self.generate_next_item();
        }
        if facts.is_list_item_marker_box() && !facts.list_marker_is_inside() {
            let parent = self.context().parent_node(node);
            if parent.is_invalid()
                || !self.context().facts(parent).is_list_item_box()
                || !self.context().facts(parent).is_fragmented_inline()
            {
                self.skip_to_next();
                return self.generate_next_item();
            }
        }
        if !facts.is_box() {
            self.skip_to_next();
            return self.generate_next_item();
        }
        if facts.is_inline_flow_interrupting_block() {
            self.skip_to_next();
            return Some(Item::new(ItemType::BlockLevelBox, node));
        }

        let used = if self.box_model_node_stack.last().copied() == Some(node) {
            self.context().used(node)
        } else {
            self.context()
                .create_used_values(node, self.context().input.containing_block_constraints)
        };
        let content_baselines = self.context_mut().dimension_box_on_line(node);
        let min_content_inline_size = self.context().paired_min_content_inline_size_for_atomic_root(node);
        let mut item = Item::new(ItemType::Element, node);
        item.content_baselines = content_baselines;
        item.inline_size = used.content_inline_size.get();
        item.min_content_inline_size = min_content_inline_size;
        item.padding_start = used.padding_left.get();
        item.padding_end = used.padding_right.get();
        item.border_start = used.border_left.get();
        item.border_end = used.border_right.get();
        item.margin_start = used.margin_left.get();
        item.margin_end = used.margin_right.get();
        self.add_extra_box_model_metrics_to_item(&mut item, true, true);
        self.skip_to_next();
        Some(item)
    }
}

pub(crate) struct InlineLevelIterator {
    visited_fragmented_inlines: Vec<Node>,
    items: Vec<Item>,
    next_item_index: usize,
}

impl InlineLevelIterator {
    pub(crate) fn new(context: &mut inline_formatting_context::InlineFormattingContext<'_>) -> Self {
        InlineLevelIteratorGenerator::generate(context)
    }

    pub(crate) fn next(&mut self) -> Option<Item> {
        let index = self.next_item_index;
        if index >= self.items.len() {
            return None;
        }
        self.next_item_index += 1;
        Some(std::mem::replace(
            &mut self.items[index],
            Item::new(ItemType::ForcedBreak, NodeSlotId::INVALID),
        ))
    }

    pub(crate) fn items(&self) -> &[Item] {
        &self.items
    }

    pub(crate) fn skip_items(&mut self, count: usize) {
        assert!(self.next_item_index + count <= self.items.len());
        self.next_item_index += count;
    }

    pub(crate) fn next_non_whitespace_sequence_inline_size(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> CssPixels {
        let mut size = CssPixels::default();
        for item in &self.items[self.next_item_index..] {
            if matches!(item.type_, ItemType::ForcedBreak | ItemType::BlockLevelBox) {
                break;
            }
            let style = context.style(context.style_source(item.node));
            if style.text_wrap_mode() == text_wrap_mode::WRAP {
                if item.type_ != ItemType::Text || item.is_collapsible_whitespace {
                    break;
                }
                if item.is_ascii_whitespace(context) {
                    break;
                }
            }
            size += item.border_box_inline_size();
        }
        size
    }

    pub(crate) fn take_visited_fragmented_inlines(&mut self) -> Vec<Node> {
        std::mem::take(&mut self.visited_fragmented_inlines)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq)]
#[repr(C)]
pub struct FfiDrawGlyph {
    pub x: f32,
    pub y: f32,
    pub length_in_code_units: usize,
    pub glyph_width: f32,
    pub glyph_id: u32,
    pub should_paint: bool,
}
