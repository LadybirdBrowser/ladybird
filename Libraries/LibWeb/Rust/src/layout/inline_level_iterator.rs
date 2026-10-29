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

impl ItemType {
    /// Whether an item of this type takes its place on the lines alone, unlike floats and block-level boxes, which lay
    /// out beside or between the lines.
    pub(crate) fn stays_on_lines(self) -> bool {
        matches!(
            self,
            Self::Text | Self::Element | Self::ForcedBreak | Self::AbsolutelyPositionedElement
        )
    }
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
    /// For an element item laid out on a line, the sizes and box sides of its box, as its layout left them.
    pub(crate) box_cells: Option<Box<used_values::RootInputCells>>,
}

fn shape_glyph_data(
    text: &[u16],
    font: libgfx_rust::font::FontHandle,
    text_type: u8,
    baseline_start_x: f32,
    letter_spacing: f32,
    word_spacing: f32,
) -> (line_box_fragment::GlyphData, line_box_fragment::TrailingWhitespace) {
    let shaped_text_type =
        libgfx_rust::text_layout::TextType::try_from(text_type).expect("invalid Gfx::GlyphRun::TextType");
    let shaped = libgfx_rust::text_layout::shape_text(
        &font,
        text,
        shaped_text_type,
        baseline_start_x,
        letter_spacing,
        word_spacing,
    );
    let trailing_whitespace = line_box_fragment::TrailingWhitespace {
        length_in_code_units: shaped.trailing_whitespace_length_in_code_units(),
        inline_size: CssPixels::nearest_value_for_f32(shaped.trailing_whitespace_advance()),
    };
    let glyph_data = line_box_fragment::GlyphData {
        width: shaped.width(),
        glyphs: shaped.into_glyphs(),
        font,
        text_type,
        is_upright: false,
    };
    (glyph_data, trailing_whitespace)
}

/// Whether text in the given writing mode and text orientation that starts with `code_point` is set with its glyphs
/// upright, stacked along the vertical inline axis, rather than turned sideways like a horizontal line of text.
/// https://drafts.csswg.org/css-writing-modes-4/#text-orientation
pub(crate) fn text_is_set_upright(writing_mode: u8, text_orientation: u8, code_point: u32) -> bool {
    // NB: The sideways writing modes set all text sideways, whatever its text orientation.
    if !matches!(writing_mode, writing_mode::VERTICAL_RL | writing_mode::VERTICAL_LR) {
        return false;
    }
    match text_orientation {
        // upright: Typographic character units from horizontal-only scripts are typeset upright, i.e. in their
        // standard horizontal orientation. Typographic character units from vertical scripts are typeset with their
        // intrinsic orientation and shaped normally.
        // FIXME: This value causes the used value of 'direction' to be 'ltr', and for the purposes of bidi
        //        reordering, causes all characters to be treated as strong LTR.
        text_orientation::UPRIGHT => true,
        // mixed: Typographic character units from horizontal-only scripts are typeset sideways, i.e. 90° clockwise
        // from their standard orientation in horizontal text. Typographic character units from vertical scripts are
        // typeset with their intrinsic orientation.
        text_orientation::MIXED => text_chunker::code_point_is_upright_in_mixed_orientation(code_point),
        _ => false,
    }
}

/// Sets shaped glyphs upright: each of them takes one em along the inline axis, which runs downwards in the frame of
/// a vertical writing mode, while keeping its own horizontal width, which painting centers it within. Invisible
/// characters such as soft hyphens take no space.
// FIXME: Use the vertical metrics of the font, where it has them.
fn set_glyphs_upright(glyph_data: &mut line_box_fragment::GlyphData, inline_offset: f32, advance: f32) {
    let glyphs = glyph_data.glyphs.to_mut();
    let mut pen = inline_offset;
    let mut count = 0usize;
    for glyph in glyphs.iter_mut() {
        if glyph.length_in_code_units != 0 && glyph.should_paint {
            if count > 0 {
                pen += advance;
            }
            count += 1;
        }
        glyph.x = pen;
    }
    glyph_data.width = advance * count as f32;
    glyph_data.is_upright = true;
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
            box_cells: None,
        }
    }

    /// The sizes and box sides of the box of an element item laid out on a line.
    #[track_caller]
    pub(crate) fn box_cells(&self) -> used_values::RootInputCells {
        *self
            .box_cells
            .as_deref()
            .expect("an element item laid out on a line carries the cells of its box")
    }

    pub(crate) fn border_box_inline_size(&self) -> CssPixels {
        self.border_start + self.padding_start + self.inline_size + self.padding_end + self.border_end
    }

    pub(crate) fn is_ascii_whitespace(&self, context: &inline_formatting_context::InlineFormattingContext<'_>) -> bool {
        assert_eq!(self.type_, ItemType::Text);
        let text = &context.callbacks.text_content(self.node).text;
        text[self.offset_in_node..self.offset_in_node + self.length_in_node]
            .iter()
            .all(|unit| *unit <= 0x7f && (*unit as u8).is_ascii_whitespace())
    }

    pub(crate) fn ends_in_ascii_space(&self, context: &inline_formatting_context::InlineFormattingContext<'_>) -> bool {
        assert_eq!(self.type_, ItemType::Text);
        let text = &context.callbacks.text_content(self.node).text;
        text[self.offset_in_node..self.offset_in_node + self.length_in_node]
            .last()
            .is_some_and(|unit| line_box_fragment::is_ascii_space(*unit))
    }

    pub(crate) fn allows_overflow_break(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        self.type_ == ItemType::Text
            && !self.is_collapsible_whitespace
            && !self.is_ascii_whitespace(context)
            && context.overflow_break_applies(self.node)
    }

    pub(crate) fn split_for_overflow_break(
        &mut self,
        text: &[u16],
        segmenter: &text_chunker::GraphemeSegmenter,
        letter_spacing: f32,
        word_spacing: f32,
        max_border_box_inline_size: CssPixels,
        take_first_grapheme_when_none_fits: bool,
    ) -> Option<Item> {
        assert_eq!(self.type_, ItemType::Text);
        self.split_at_shaped_glyph(
            segmenter,
            max_border_box_inline_size,
            take_first_grapheme_when_none_fits,
        )
        .or_else(|| {
            self.split_by_reshaping(
                text,
                segmenter,
                letter_spacing,
                word_spacing,
                max_border_box_inline_size,
                take_first_grapheme_when_none_fits,
            )
        })
    }

    pub(crate) fn drop_prefix_before(
        &mut self,
        split_offset_in_node: usize,
        text: &[u16],
        letter_spacing: f32,
        word_spacing: f32,
    ) {
        let end = self.offset_in_node + self.length_in_node;
        if split_offset_in_node <= self.offset_in_node || split_offset_in_node >= end {
            return;
        }
        let Some(glyph_data) = self.glyphs.as_ref() else {
            return;
        };
        let remainder_glyphs = self
            .shaped_glyphs_after(split_offset_in_node - self.offset_in_node)
            .unwrap_or_else(|| {
                shape_glyph_data(
                    &text[split_offset_in_node..end],
                    glyph_data.font.clone(),
                    glyph_data.text_type,
                    0.0,
                    letter_spacing,
                    word_spacing,
                )
                .0
            });
        self.drop_prefix(split_offset_in_node, remainder_glyphs);
    }

    fn shaped_glyphs_after(&self, code_units: usize) -> Option<line_box_fragment::GlyphData> {
        let glyph_data = self.glyphs.as_ref()?;
        if glyph_data.text_type == line_box_fragment::GLYPH_TEXT_TYPE_RTL
            || glyph_data
                .glyphs
                .iter()
                .map(|glyph| glyph.length_in_code_units)
                .sum::<usize>()
                != self.length_in_node
        {
            return None;
        }
        let mut consumed = 0;
        let split_glyph_index = glyph_data.glyphs.iter().position(|glyph| {
            let at_split = consumed == code_units && glyph.length_in_code_units != 0;
            consumed += glyph.length_in_code_units;
            at_split
        })?;
        let split_x = glyph_data.glyphs[split_glyph_index].x;
        let glyphs: Vec<_> = glyph_data.glyphs[split_glyph_index..]
            .iter()
            .map(|glyph| libgfx_rust::text_layout::DrawGlyph {
                x: glyph.x - split_x,
                ..*glyph
            })
            .collect();
        Some(line_box_fragment::GlyphData {
            glyphs: glyphs.into(),
            font: glyph_data.font.clone(),
            text_type: glyph_data.text_type,
            width: glyph_data.width - split_x,
            is_upright: glyph_data.is_upright,
        })
    }

    fn take_prefix_before(
        &mut self,
        split_offset_in_node: usize,
        prefix_glyphs: line_box_fragment::GlyphData,
        remainder_glyphs: line_box_fragment::GlyphData,
    ) -> Item {
        let mut prefix = Item::new(ItemType::Text, self.node);
        prefix.offset_in_node = self.offset_in_node;
        prefix.length_in_node = split_offset_in_node - self.offset_in_node;
        prefix.inline_size = CssPixels::nearest_value_for_f32(prefix_glyphs.width);
        prefix.margin_start = self.margin_start;
        prefix.border_start = self.border_start;
        prefix.padding_start = self.padding_start;
        prefix.can_break_before = self.can_break_before;
        prefix.glyphs = Some(prefix_glyphs);
        self.drop_prefix(split_offset_in_node, remainder_glyphs);
        prefix
    }

    fn drop_prefix(&mut self, split_offset_in_node: usize, remainder_glyphs: line_box_fragment::GlyphData) {
        self.length_in_node -= split_offset_in_node - self.offset_in_node;
        self.offset_in_node = split_offset_in_node;
        self.inline_size = CssPixels::nearest_value_for_f32(remainder_glyphs.width);
        self.glyphs = Some(remainder_glyphs);
        self.margin_start = CssPixels::default();
        self.border_start = CssPixels::default();
        self.padding_start = CssPixels::default();
        self.can_break_before = false;
    }

    fn split_at_shaped_glyph(
        &mut self,
        segmenter: &text_chunker::GraphemeSegmenter,
        max_border_box_inline_size: CssPixels,
        take_first_grapheme_when_none_fits: bool,
    ) -> Option<Item> {
        let glyph_data = self.glyphs.as_ref()?;
        if glyph_data.text_type == line_box_fragment::GLYPH_TEXT_TYPE_RTL || glyph_data.glyphs.len() < 2 {
            return None;
        }
        let shaped_length_in_code_units: usize = glyph_data.glyphs.iter().map(|glyph| glyph.length_in_code_units).sum();
        if shaped_length_in_code_units != self.length_in_node {
            return None;
        }

        let leading_edge_inline_size = self.margin_start + self.border_start + self.padding_start;
        let mut split: Option<(usize, usize)> = None;
        let mut code_units = glyph_data.glyphs[0].length_in_code_units;
        for (glyph_index, glyph) in glyph_data.glyphs.iter().enumerate().skip(1) {
            if glyph.length_in_code_units == 0 {
                continue;
            }
            let offset = self.offset_in_node + code_units;
            if segmenter.next_boundary(offset, true) == Some(offset) {
                let prefix_inline_size = CssPixels::nearest_value_for_f32(glyph.x);
                if leading_edge_inline_size + prefix_inline_size > max_border_box_inline_size {
                    if split.is_none() && take_first_grapheme_when_none_fits {
                        split = Some((glyph_index, code_units));
                    }
                    break;
                }
                split = Some((glyph_index, code_units));
            }
            code_units += glyph.length_in_code_units;
        }
        let (split_glyph_index, split_code_units) = split?;

        let mut prefix = self.glyphs.take().unwrap();
        let split_x = prefix.glyphs[split_glyph_index].x;
        let mut remainder_glyphs = prefix.glyphs.to_mut().split_off(split_glyph_index);
        for glyph in &mut remainder_glyphs {
            glyph.x -= split_x;
        }
        let remainder = line_box_fragment::GlyphData {
            glyphs: remainder_glyphs.into(),
            font: prefix.font.clone(),
            text_type: prefix.text_type,
            width: prefix.width - split_x,
            is_upright: prefix.is_upright,
        };
        prefix.width = split_x;

        Some(self.take_prefix_before(self.offset_in_node + split_code_units, prefix, remainder))
    }

    fn split_by_reshaping(
        &mut self,
        text: &[u16],
        segmenter: &text_chunker::GraphemeSegmenter,
        letter_spacing: f32,
        word_spacing: f32,
        max_border_box_inline_size: CssPixels,
        take_first_grapheme_when_none_fits: bool,
    ) -> Option<Item> {
        let glyph_data = self.glyphs.as_ref()?;
        if glyph_data.text_type == line_box_fragment::GLYPH_TEXT_TYPE_RTL {
            return None;
        }
        let font = glyph_data.font.clone();
        let text_type = glyph_data.text_type;
        let start = self.offset_in_node;
        let end = start + self.length_in_node;

        let mut boundaries = Vec::with_capacity(end - start);
        let mut boundary = segmenter.next_boundary(start, false)?;
        while boundary < end {
            boundaries.push(boundary);
            let Some(next) = segmenter.next_boundary(boundary, false) else {
                break;
            };
            boundary = next;
        }
        if boundaries.is_empty() {
            return None;
        }

        let shape = |text_range: &[u16]| {
            shape_glyph_data(text_range, font.clone(), text_type, 0.0, letter_spacing, word_spacing).0
        };

        let leading_edge_inline_size = self.margin_start + self.border_start + self.padding_start;
        let prefix_fits = |boundary: usize| {
            let shaped = shape(&text[start..boundary]);
            leading_edge_inline_size + CssPixels::nearest_value_for_f32(shaped.width) <= max_border_box_inline_size
        };
        let mut fitting_count = boundaries.partition_point(|boundary| prefix_fits(*boundary));
        if fitting_count == 0 {
            if !take_first_grapheme_when_none_fits {
                return None;
            }
            fitting_count = 1;
        }
        let split = boundaries[fitting_count - 1];

        Some(self.take_prefix_before(split, shape(&text[start..split]), shape(&text[split..end])))
    }

    pub(crate) fn grapheme_cluster_inline_sizes(
        &self,
        text: &[u16],
        segmenter: &text_chunker::GraphemeSegmenter,
        letter_spacing: f32,
        word_spacing: f32,
    ) -> Vec<CssPixels> {
        assert_eq!(self.type_, ItemType::Text);
        let mut remainder = self.clone();
        let mut sizes = Vec::new();
        while remainder.border_box_inline_size() > CssPixels::default()
            && let Some(prefix) = remainder.split_for_overflow_break(
                text,
                segmenter,
                letter_spacing,
                word_spacing,
                CssPixels::default(),
                true,
            )
        {
            sizes.push(prefix.inline_size);
        }
        sizes.push(remainder.inline_size);
        sizes
    }
}

#[derive(Clone, Copy, Default)]
struct ExtraBoxMetrics {
    margin: CssPixels,
    border: CssPixels,
    padding: CssPixels,
}

struct TextNodeContext<'pass> {
    chunks: std::sync::Arc<super::rendered_text::CachedTextChunks>,
    text: &'pass [u16],
    next_chunk_index: usize,
    should_collapse_whitespace: bool,
    should_respect_linebreaks: bool,
    last_known_direction: Option<u8>,
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum AtomicInlineSizing {
    Layout,
    Contributions,
}

struct InlineLevelIteratorGenerator<'iterator, 'context> {
    context: &'iterator mut inline_formatting_context::InlineFormattingContext<'context>,
    atomic_sizing: AtomicInlineSizing,
    current_node: Node,
    next_node: Node,
    text_node_context: Option<TextNodeContext<'context>>,
    is_unidirectional_left_to_right: bool,
    extra_leading_metrics: Option<ExtraBoxMetrics>,
    extra_trailing_metrics: Option<ExtraBoxMetrics>,
    box_model_node_stack: Vec<Node>,
    entered_box_model_nodes: Vec<Node>,
    items: Vec<Item>,
    accumulated_inline_size_for_tabs: CssPixels,
    previous_chunk_can_break_after: bool,
    /// The text of the text node before the current one, when nothing but inline box boundaries and out-of-flow
    /// boxes lies between them.
    previous_text: Option<&'context [u16]>,
    /// The number of regional indicators that end the previous text, including any in the text nodes before it.
    previous_text_regional_indicators: usize,
    /// The number of regional indicators that end the text before the current text node.
    regional_indicators_before_text: usize,
    /// The last code unit of the text before the previous text.
    code_unit_before_previous_text: Option<u16>,
    /// The last code unit of the text before the current text node.
    code_unit_before_text: Option<u16>,
}

impl<'iterator, 'context> InlineLevelIteratorGenerator<'iterator, 'context> {
    fn generate(
        context: &'iterator mut inline_formatting_context::InlineFormattingContext<'context>,
        atomic_sizing: AtomicInlineSizing,
    ) -> InlineLevelIterator {
        let containing_block = context.containing_block;
        let next_node = context.first_child(containing_block);
        let mut iterator = Self {
            context,
            atomic_sizing,
            next_node,
            current_node: NodeSlotId::INVALID,
            text_node_context: None,
            is_unidirectional_left_to_right: false,
            extra_leading_metrics: None,
            extra_trailing_metrics: None,
            box_model_node_stack: Vec::new(),
            entered_box_model_nodes: Vec::new(),
            items: Vec::new(),
            accumulated_inline_size_for_tabs: CssPixels::default(),
            previous_chunk_can_break_after: false,
            previous_text: None,
            previous_text_regional_indicators: 0,
            regional_indicators_before_text: 0,
            code_unit_before_previous_text: None,
            code_unit_before_text: None,
        };
        iterator.is_unidirectional_left_to_right = iterator.compute_is_unidirectional_left_to_right();
        iterator.skip_to_next();
        iterator.generate_all_items();
        InlineLevelIterator {
            entered_box_model_nodes: iterator.entered_box_model_nodes,
            items: ItemQueue::Owned(iterator.items.into_iter()),
            items_kept_for_looking_ahead: 0,
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
            } else if item.type_ != ItemType::FloatingElement {
                self.accumulated_inline_size_for_tabs += item.border_box_inline_size();
            }
            self.items.push(item);
        }
    }

    fn enter_node_with_box_model_metrics(&mut self, node: Node) {
        self.entered_box_model_nodes.push(node);
        let sides = lay_out_entered_inline_box(self.context, node);
        let leading = self.extra_leading_metrics.get_or_insert_default();
        leading.margin += sides.margin_left;
        leading.border += sides.border_left;
        leading.padding += sides.padding_left;
        self.box_model_node_stack.push(node);
    }

    fn exit_node_with_box_model_metrics(&mut self) {
        let node = self.box_model_node_stack.pop().unwrap();
        let sides = self.context().sides_of_entered_inline_box(node);
        let margin = sides.margin_right;
        let border = sides.border_right;
        let padding = sides.padding_right;
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

    fn resolve_text_direction_from_context(&self, context: &TextNodeContext<'_>) -> u8 {
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

    /// The end edges of inline boxes that end right after an out-of-flow box are pending when the iterator reaches
    /// that box. It takes no edges itself, so they precede the next item rather than follow its content.
    fn carry_trailing_metrics_past_out_of_flow_box(&mut self) {
        let Some(trailing) = self.extra_trailing_metrics.take() else {
            return;
        };
        let leading = self.extra_leading_metrics.get_or_insert_default();
        leading.margin += trailing.margin;
        leading.border += trailing.border;
        leading.padding += trailing.padding;
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

    /// The text node's next item, or none once it has no more and the iterator has moved past it.
    fn generate_text_item(&mut self, text_node: Node) -> Option<Item> {
        if self.text_node_context.is_none() {
            self.enter_text_node(text_node);
        }
        let mut text_context = self.text_node_context.take().unwrap();
        let chunks = &text_context.chunks;
        let is_first_chunk = text_context.next_chunk_index == 0;
        let chunk = chunks.get(text_context.next_chunk_index).cloned();
        if chunk.is_some() {
            text_context.next_chunk_index += 1;
        }
        let mut is_last_chunk = text_context.next_chunk_index >= chunks.len();
        let synthesize_zero_length_chunk = chunk.is_none()
            && is_first_chunk
            && is_last_chunk
            && text_context.text.is_empty()
            && self.context().facts(text_node).produces_line_box_fragment_when_empty();
        // INTEROP: A text control's text that ends in a preserved newline keeps the empty line after it, which gives
        //          that line its height and the caret on it a fragment.
        let synthesize_chunk_after_trailing_newline = chunk.is_none()
            && text_context.next_chunk_index == chunks.len()
            && text_context.should_respect_linebreaks
            && chunks.last().is_some_and(|last| last.has_breaking_newline)
            && self.context().facts(text_node).is_text_control_text();
        let chunk = if let Some(chunk) = chunk {
            chunk
        } else if synthesize_zero_length_chunk || synthesize_chunk_after_trailing_newline {
            text_context.next_chunk_index = chunks.len() + 1;
            let parent_style = self.context().style(self.context().parent_node(text_node));
            text_chunker::TextChunk {
                start: text_context.text.len(),
                length: 0,
                font: parent_style.first_available_font(),
                has_breaking_newline: false,
                has_breaking_tab: false,
                is_all_whitespace: true,
                can_break_after: false,
                text_type: line_box_fragment::GLYPH_TEXT_TYPE_COMMON,
            }
        } else {
            if !text_context.text.is_empty() {
                let regional_indicators = text_chunker::trailing_regional_indicator_count(text_context.text);
                // NB: Each regional indicator is a surrogate pair.
                self.previous_text_regional_indicators = if regional_indicators * 2 == text_context.text.len() {
                    self.regional_indicators_before_text + regional_indicators
                } else {
                    regional_indicators
                };
                self.code_unit_before_previous_text = self.code_unit_before_text;
                self.previous_text = Some(text_context.text);
            }
            self.text_node_context = None;
            self.previous_chunk_can_break_after = false;
            self.skip_to_next();
            return None;
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
        if text_type == line_box_fragment::GLYPH_TEXT_TYPE_CONTEXT_DEPENDENT {
            text_type = self.resolve_text_direction_from_context(&text_context);
        }
        if text_context.should_respect_linebreaks && chunk.has_breaking_newline {
            self.text_node_context = Some(text_context);
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
                let space = chunk.font.glyph_width(b' ' as u32);
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
            let zero_width = chunk.font.glyph_width(b'0' as u32);
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
        let (mut glyphs, shaped_trailing_whitespace) = shape_glyph_data(
            shaped_text,
            chunk.font.clone(),
            text_type,
            inline_offset,
            style.letter_spacing().to_double() as f32,
            word_spacing.to_double() as f32,
        );
        if !shaped_text.is_empty()
            && text_is_set_upright(
                style.writing_mode(),
                style.text_orientation(),
                text_chunker::code_point_at(shaped_text, 0),
            )
        {
            // NB: The font's own size takes any font-size-adjust into account.
            set_glyphs_upright(
                &mut glyphs,
                inline_offset,
                chunk.font.facts().pixel_size + style.letter_spacing().to_double() as f32,
            );
        }
        let chunk_inline_size = CssPixels::nearest_value_for_f32(glyphs.width + inline_offset);
        let generated_empty = synthesize_zero_length_chunk
            || synthesize_chunk_after_trailing_newline
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
        // NB: Look for a soft wrap opportunity between the texts of two text nodes, which the chunker of either text
        //     cannot see. This uses the line breaking properties of the text after it.
        if is_first_chunk {
            self.regional_indicators_before_text = if self.previous_text.is_some() {
                self.previous_text_regional_indicators
            } else {
                0
            };
            self.code_unit_before_text = self.previous_text.and_then(|text| text.last().copied());
        }
        if is_first_chunk
            && !item.can_break_before
            && let Some(previous_text) = self.previous_text.take()
            && style.text_wrap_mode() == text_wrap_mode::WRAP
        {
            let line_break_language = if style.line_break() == line_break::LOOSE {
                self.context().callbacks.arena().text_line_break_language(text_node)
            } else {
                text_chunker::LineBreakLanguage::Other
            };
            item.can_break_before = text_chunker::soft_wrap_opportunity_between(
                previous_text,
                &full_text[chunk.start..],
                self.code_unit_before_previous_text,
                self.regional_indicators_before_text,
                style.word_break(),
                style.line_break(),
                line_break_language,
            );
        }
        self.previous_text = None;
        self.previous_chunk_can_break_after = chunk.can_break_after;
        self.add_extra_box_model_metrics_to_item(&mut item, is_first_chunk, is_last_chunk);
        self.text_node_context = Some(text_context);
        Some(item)
    }

    fn generate_next_item(&mut self) -> Option<Item> {
        // A node that produces no item moves on to the next one in this loop rather than in a call: a line of ten
        // thousand empty spans would otherwise take ten thousand frames of the stack.
        loop {
            if self.current_node.is_invalid() {
                return None;
            }
            let node = self.current_node;
            let facts = self.context().facts(node);
            if facts.is_text_node() {
                match self.generate_text_item(node) {
                    Some(item) => return Some(item),
                    None => continue,
                }
            }
            if facts.is_absolutely_positioned() {
                let preceded = self.extra_leading_metrics.is_some_and(|extra| {
                    extra.margin != CssPixels::default()
                        || extra.border != CssPixels::default()
                        || extra.padding != CssPixels::default()
                });
                self.carry_trailing_metrics_past_out_of_flow_box();
                self.skip_to_next();
                let mut item = Item::new(ItemType::AbsolutelyPositionedElement, node);
                item.preceded_by_unattached_inline_start_edges = preceded;
                return Some(item);
            }
            if facts.is_floating() {
                self.carry_trailing_metrics_past_out_of_flow_box();
                self.skip_to_next();
                let mut item = Item::new(ItemType::FloatingElement, node);
                if self.atomic_sizing == AtomicInlineSizing::Contributions {
                    self.size_item_by_outer_inline_contributions(&mut item);
                }
                return Some(item);
            }
            if facts.is_break_node() {
                self.previous_text = None;
                self.skip_to_next();
                return Some(Item::new(ItemType::ForcedBreak, node));
            }
            if facts.is_fragmented_inline() {
                self.skip_to_next();
                continue;
            }
            if facts.is_list_item_marker_box() && !facts.list_marker_is_inside() {
                let parent = self.context().parent_node(node);
                if parent.is_invalid()
                    || !self.context().facts(parent).is_list_item_box()
                    || !self.context().facts(parent).is_fragmented_inline()
                {
                    self.skip_to_next();
                    continue;
                }
            }
            if !facts.is_box() {
                self.skip_to_next();
                continue;
            }
            if facts.is_inline_flow_interrupting_block() {
                self.previous_text = None;
                self.skip_to_next();
                let mut item = Item::new(ItemType::BlockLevelBox, node);
                if self.atomic_sizing == AtomicInlineSizing::Contributions {
                    self.size_item_by_outer_inline_contributions(&mut item);
                }
                return Some(item);
            }
            self.previous_text = None;
            return Some(self.generate_element_item(node));
        }
    }

    fn size_item_by_outer_inline_contributions(&self, item: &mut Item) {
        let context = self.context();
        let contributions = context.sizing().outer_inline_contributions(
            item.node,
            context.input.available_space.block_size,
            context.input.containing_block_constraints,
        );
        item.inline_size = contributions.max_content_inline_size;
        item.min_content_inline_size = Some(contributions.min_content_inline_size);
        item.padding_start = contributions.padding_start;
        item.padding_end = contributions.padding_end;
        item.border_start = contributions.border_start;
        item.border_end = contributions.border_end;
        item.margin_start = contributions.margin_start;
        item.margin_end = contributions.margin_end;
    }

    fn generate_element_item(&mut self, node: Node) -> Item {
        let mut item = Item::new(ItemType::Element, node);
        if self.atomic_sizing == AtomicInlineSizing::Contributions {
            self.size_item_by_outer_inline_contributions(&mut item);
        } else {
            let cells = self.context().hold_atomic_inline(node);
            let (content_baselines, box_cells) = self.context_mut().dimension_box_on_line(node, cells);
            item.content_baselines = content_baselines;
            item.inline_size = box_cells.content_inline_size;
            item.padding_start = box_cells.padding_left;
            item.padding_end = box_cells.padding_right;
            item.border_start = box_cells.border_left;
            item.border_end = box_cells.border_right;
            item.margin_start = box_cells.margin_left;
            item.margin_end = box_cells.margin_right;
            item.box_cells = Some(Box::new(box_cells));
        }
        self.add_extra_box_model_metrics_to_item(&mut item, true, true);
        self.skip_to_next();
        item
    }
}

/// Lays out `node`, a fragmented inline the context enters, resolves its box sides and returns them.
fn lay_out_entered_inline_box(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    node: Node,
) -> used_values::RootInputCells {
    let constraints = context.input.containing_block_constraints;
    let mut sides = used_values::cells_before_sizing(&context.callbacks, node, constraints);
    let style = context.style(node);
    let basis = constraints.inline_basis();
    sides.resolve_box_model(&style, basis);
    let insets = context.compute_inset(node);
    context.keep_entered_inline_box(node, sides, insets);
    if context.run.fragments.is_some() && context.run.fragmentainer.is_none() {
        formatting_context::place_child(
            context.run,
            node,
            formatting_context::PlacedChild::new(fragment_tree::FragmentGeometry {
                cells: sides.in_frame(context.callbacks.frame, context.run.callbacks.frame),
                line_data: None,
            })
            .with_in_flow_box_insets(insets),
            FfiCssPixelPoint::default(),
            None,
            None,
        );
    }
    sides
}

/// The inline items a block container generated, kept for the later runs of the container in the same pass. Their
/// glyphs are shared, so that each run that reads them copies no glyphs.
#[derive(Clone)]
pub(crate) struct StashedInlineItems {
    items: Vec<Item>,
    entered_box_model_nodes: Vec<Node>,
}

struct SharedInlineItems {
    items: std::rc::Rc<[Item]>,
    entered_box_model_nodes: std::rc::Rc<[Node]>,
}

#[derive(Default)]
pub(crate) struct InlineItemCache {
    items_by_block_container: RefCell<HashMap<Node, SharedInlineItems>>,
}

enum ItemQueue {
    Owned(std::vec::IntoIter<Item>),
    Shared { items: std::rc::Rc<[Item]>, next: usize },
}

impl ItemQueue {
    fn as_slice(&self) -> &[Item] {
        match self {
            Self::Owned(items) => items.as_slice(),
            Self::Shared { items, next } => &items[*next..],
        }
    }

    fn next(&mut self) -> Option<Item> {
        match self {
            Self::Owned(items) => items.next(),
            Self::Shared { items, next } => {
                let item = items.get(*next).cloned();
                *next += usize::from(item.is_some());
                item
            }
        }
    }

    fn skip(&mut self, count: usize) {
        assert!(count <= self.as_slice().len());
        match self {
            Self::Owned(items) => items.by_ref().take(count).for_each(drop),
            Self::Shared { next, .. } => *next += count,
        }
    }

    fn into_vec(self) -> Vec<Item> {
        match self {
            Self::Owned(items) => items.collect(),
            Self::Shared { items, next } => items[next..].to_vec(),
        }
    }
}

pub(crate) struct InlineLevelIterator {
    entered_box_model_nodes: Vec<Node>,
    items: ItemQueue,
    /// The number of items at the end that this iterator only shows to look ahead, and does not hand out.
    items_kept_for_looking_ahead: usize,
}

impl InlineLevelIterator {
    pub(crate) fn new(context: &mut inline_formatting_context::InlineFormattingContext<'_>) -> Self {
        Self::reuse_or_generate(context, AtomicInlineSizing::Layout)
    }

    pub(crate) fn new_sharing_items(
        context: &mut inline_formatting_context::InlineFormattingContext<'_>,
        cache: &InlineItemCache,
    ) -> Self {
        let containing_block = context.containing_block;
        let shared = cache
            .items_by_block_container
            .borrow()
            .get(&containing_block)
            .map(|shared| (shared.items.clone(), shared.entered_box_model_nodes.clone()));
        if let Some((items, entered_box_model_nodes)) = shared {
            for node in entered_box_model_nodes.iter() {
                lay_out_entered_inline_box(context, *node);
            }
            return Self {
                entered_box_model_nodes: entered_box_model_nodes.to_vec(),
                items: ItemQueue::Shared { items, next: 0 },
                items_kept_for_looking_ahead: 0,
            };
        }
        let iterator = Self::new(context);
        if !iterator.items.as_slice().iter().all(|item| item.type_.stays_on_lines()) {
            return iterator;
        }
        let items = iterator.items.into_vec();
        let shared_items: std::rc::Rc<[Item]> = items.as_slice().into();
        cache.items_by_block_container.borrow_mut().insert(
            containing_block,
            SharedInlineItems {
                items: shared_items.clone(),
                entered_box_model_nodes: iterator.entered_box_model_nodes.as_slice().into(),
            },
        );
        let laid_out_atomic_inlines = items.iter().any(|item| item.type_ == ItemType::Element);
        Self {
            entered_box_model_nodes: iterator.entered_box_model_nodes,
            items: if laid_out_atomic_inlines {
                ItemQueue::Owned(items.into_iter())
            } else {
                ItemQueue::Shared {
                    items: shared_items,
                    next: 0,
                }
            },
            items_kept_for_looking_ahead: 0,
        }
    }

    pub(crate) fn for_intrinsic_inline_size(
        context: &mut inline_formatting_context::InlineFormattingContext<'_>,
    ) -> Self {
        Self::reuse_or_generate(context, AtomicInlineSizing::Contributions)
    }

    fn reuse_or_generate(
        context: &mut inline_formatting_context::InlineFormattingContext<'_>,
        atomic_sizing: AtomicInlineSizing,
    ) -> Self {
        let callbacks = context.callbacks;
        match callbacks.inline_item_stash(context.containing_block) {
            Some(stash) => Self::from_stash(context, stash),
            None => InlineLevelIteratorGenerator::generate(context, atomic_sizing),
        }
    }

    fn from_stash(context: &inline_formatting_context::InlineFormattingContext<'_>, stash: StashedInlineItems) -> Self {
        for node in &stash.entered_box_model_nodes {
            lay_out_entered_inline_box(context, *node);
        }
        Self {
            entered_box_model_nodes: stash.entered_box_model_nodes,
            items: ItemQueue::Owned(stash.items.into_iter()),
            items_kept_for_looking_ahead: 0,
        }
    }

    /// Element items and percentage inline-box margins and paddings depend on the run's available space.
    fn is_independent_of_available_space(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        self.items
            .as_slice()
            .iter()
            .all(|item| matches!(item.type_, ItemType::Text | ItemType::ForcedBreak))
            && self.entered_box_model_nodes.iter().all(|node| {
                let style = context.style(*node);
                !style.margin_left().contains_percentage()
                    && !style.margin_right().contains_percentage()
                    && !style.padding_left().contains_percentage()
                    && !style.padding_right().contains_percentage()
            })
    }

    pub(crate) fn stash_for_reuse(self, context: &inline_formatting_context::InlineFormattingContext<'_>) {
        if !self.is_independent_of_available_space(context) {
            return;
        }
        let mut items = self.items.into_vec();
        for glyphs in items.iter_mut().filter_map(|item| item.glyphs.as_mut()) {
            if let libgfx_rust::text_layout::GlyphBuffer::Owned(owned) = &mut glyphs.glyphs {
                glyphs.glyphs = libgfx_rust::text_layout::GlyphBuffer::Shared(std::mem::take(owned).into());
            }
        }
        context.callbacks.store_inline_item_stash(
            context.containing_block,
            StashedInlineItems {
                items,
                entered_box_model_nodes: self.entered_box_model_nodes,
            },
        );
    }

    pub(crate) fn next(&mut self) -> Option<Item> {
        if self.items.as_slice().len() <= self.items_kept_for_looking_ahead {
            return None;
        }
        self.items.next()
    }

    pub(crate) fn entered_inline_boxes(&self) -> &[Node] {
        &self.entered_box_model_nodes
    }

    pub(crate) fn shares_items(&self) -> bool {
        matches!(self.items, ItemQueue::Shared { .. })
    }

    pub(crate) fn items(&self) -> &[Item] {
        self.items.as_slice()
    }

    pub(crate) fn skip_items(&mut self, count: usize) {
        self.items.skip(count);
    }

    /// An iterator over a copy of the items left in `range`, which leaves the items of this one in place. It looks
    /// ahead at the items after `range` up to `look_ahead_end`, without handing them out.
    pub(crate) fn duplicate(&self, range: std::ops::Range<usize>, look_ahead_end: usize) -> Self {
        let items: Vec<Item> = self.items()[range.start..look_ahead_end].to_vec();
        Self {
            entered_box_model_nodes: self.entered_box_model_nodes.clone(),
            items: ItemQueue::Owned(items.into_iter()),
            items_kept_for_looking_ahead: look_ahead_end - range.end,
        }
    }

    pub(crate) fn next_inline_run_size(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> Option<CssPixels> {
        sequence_inline_size(context, self.items(), false)
    }

    pub(crate) fn next_unbreakable_run_inline_size(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> CssPixels {
        sequence_inline_size(context, self.items(), true).unwrap_or_default()
    }

    pub(crate) fn next_non_whitespace_text_allows_overflow_break(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> bool {
        self.items
            .as_slice()
            .first()
            .is_some_and(|item| item.allows_overflow_break(context))
    }

    pub(crate) fn fragmented_inlines_in_pre_order(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
    ) -> Vec<Node> {
        self.entered_box_model_nodes
            .iter()
            .copied()
            .filter(|node| context.facts(*node).is_fragmented_inline())
            .collect()
    }
}

pub(crate) fn sequence_inline_size(
    context: &inline_formatting_context::InlineFormattingContext<'_>,
    items: &[Item],
    stop_at_overflow_breakable_text: bool,
) -> Option<CssPixels> {
    let mut size = CssPixels::default();
    for (index, item) in items.iter().enumerate() {
        match item.type_ {
            ItemType::ForcedBreak => return Some(CssPixels::default()),
            ItemType::BlockLevelBox => break,
            _ => {}
        }
        let style = context.style(context.style_source(item.node));
        if style.text_wrap_mode() == text_wrap_mode::WRAP {
            if item.type_ != ItemType::Text || item.is_collapsible_whitespace {
                break;
            }
            // NB: The sequence ends at its next soft wrap opportunity, such as the one after a hyphen.
            if index > 0 && item.can_break_before {
                break;
            }
            if item.is_ascii_whitespace(context) {
                break;
            }
            if stop_at_overflow_breakable_text && context.overflow_break_applies(item.node) {
                break;
            }
        }
        size += item.border_box_inline_size();
    }
    (size > CssPixels::default()).then_some(size)
}
