/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ForcedBreak {
    No,
    Yes,
}

#[derive(Clone, Copy)]
struct VerticalAlignMetrics {
    baseline: CssPixels,
    block_size: CssPixels,
    effective_box_block_start_offset: CssPixels,
    effective_box_block_end_offset: CssPixels,
    line_height: CssPixels,
}

#[derive(Clone, Copy)]
struct InlineBoxAlignment {
    box_: Node,
    vertical_shift: CssPixels,
}

#[derive(Clone, Copy)]
struct FragmentAlignmentSnapshot {
    style_source: Node,
    layout_node: Node,
    baseline: CssPixels,
    block_size: CssPixels,
    border_box_block_start: CssPixels,
    is_atomic_inline: bool,
    inline_offset: CssPixels,
    block_offset: CssPixels,
}

// https://drafts.csswg.org/css2/#line-height
#[derive(Clone, Copy)]
struct LineRelativeAlignedSubtree {
    root: Node,
    alignment: u8,
    block_start: CssPixels,
    block_end: CssPixels,
    shift: CssPixels,
}

pub(crate) struct LineBuilder<'builder, 'context> {
    context: &'builder InlineFormattingContext<'context>,
    available_inline_size_for_current_line: AvailableSize,
    current_block_offset: CssPixels,
    max_block_size_on_current_line: CssPixels,
    unbreakable_run_inline_size_interrupted_by_float: CssPixels,
    text_indent: CssPixels,
    text_indent_hanging: bool,
    text_indent_each_line: bool,
    direction: u8,
    writing_mode: u8,
    last_line_needs_update: bool,
    should_advance_to_last_line_box_block_end: bool,
    current_line_committed_pending_margin: bool,
    pending_margin_follows_block_level_box: bool,
}

impl<'builder, 'context> LineBuilder<'builder, 'context> {
    pub(crate) fn new(context: &'builder InlineFormattingContext<'context>) -> Self {
        let mut builder = Self::initialized(context);
        builder.begin_new_line(false, true, ForcedBreak::No);
        builder
    }

    fn initialized(context: &'builder InlineFormattingContext<'context>) -> Self {
        let style = context.style(context.containing_block);
        let containing_inline_size = context.input.containing_block_constraints.inline_basis();
        Self {
            context,
            available_inline_size_for_current_line: AvailableSize::Indefinite,
            current_block_offset: CssPixels::default(),
            max_block_size_on_current_line: CssPixels::default(),
            unbreakable_run_inline_size_interrupted_by_float: CssPixels::default(),
            text_indent: style.text_indent().to_px(containing_inline_size),
            text_indent_hanging: style.text_indent_hanging(),
            text_indent_each_line: style.text_indent_each_line(),
            direction: style.direction(),
            writing_mode: style.writing_mode(),
            last_line_needs_update: false,
            should_advance_to_last_line_box_block_end: false,
            current_line_committed_pending_margin: false,
            pending_margin_follows_block_level_box: false,
        }
    }

    pub(crate) fn new_after_reused_lines(context: &'builder InlineFormattingContext<'context>) -> Self {
        assert!(!context.line_data().line_boxes.is_empty());
        let current_block_offset = context.line_data().line_boxes.last().unwrap().physical_vertical_end();
        let mut builder = Self::initialized(context);
        builder.current_block_offset = current_block_offset;
        context
            .line_data_mut()
            .line_boxes
            .push(LineBoxData::new(builder.direction, builder.writing_mode));
        builder.begin_new_line(false, true, ForcedBreak::No);
        builder.current_line_committed_pending_margin = true;
        builder
    }

    fn context(&self) -> &InlineFormattingContext<'context> {
        self.context
    }

    fn line_count(&self) -> usize {
        self.context().line_data().line_boxes.len()
    }

    fn ensure_last_line_index(&mut self) -> usize {
        if self.line_count() == 0 {
            let direction = self.direction;
            let writing_mode = self.writing_mode;
            self.context()
                .line_data_mut()
                .line_boxes
                .push(LineBoxData::new(direction, writing_mode));
        }
        self.line_count() - 1
    }

    fn line(&self, index: usize) -> Ref<'_, LineBoxData> {
        Ref::map(self.context().line_data(), |data| &data.line_boxes[index])
    }

    fn line_mut(&self, index: usize) -> RefMut<'_, LineBoxData> {
        RefMut::map(self.context().line_data_mut(), |data| &mut data.line_boxes[index])
    }

    fn containing_style(&self) -> StyleValues<'_> {
        self.context().style(self.context().containing_block)
    }

    fn fragment_facts(&self, node: Node) -> FragmentBuildFacts {
        let style_source = self.context().style_source(node);
        let style = self.context().style(style_source);
        let facts = self.context().facts(node);
        let (text_utf16, text_length) = if facts.is_text_node() {
            let text = &self.context().callbacks.text_content(node).text;
            (text.as_ptr(), text.len())
        } else {
            (std::ptr::null(), 0)
        };
        FragmentBuildFacts {
            style_source,
            is_atomic_inline: facts.is_atomic_inline(),
            white_space_collapse: style.white_space_collapse(),
            text_utf16,
            text_length_in_code_units: text_length,
        }
    }

    fn begin_new_line(&mut self, advance: bool, first_in_sequence: bool, forced: ForcedBreak) {
        let line_height = self.containing_style().line_height();
        if advance {
            if first_in_sequence {
                if self.should_advance_to_last_line_box_block_end && self.line_count() > 1 {
                    let previous_line_end = self.line(self.line_count() - 2).physical_vertical_end();
                    self.current_block_offset = previous_line_end;
                } else {
                    self.current_block_offset += self.max_block_size_on_current_line.max(line_height);
                }
            } else if let Some(next) = self
                .context()
                .next_float_band_block_start_after(self.current_block_offset)
            {
                self.current_block_offset = next;
            } else {
                self.current_block_offset += self.max_block_size_on_current_line.max(line_height);
            }
        }
        self.recalculate_available_space();
        let line_index = self.ensure_last_line_index();
        self.line_mut(line_index).original_available_inline_size = self.available_inline_size_for_current_line;
        self.max_block_size_on_current_line = CssPixels::default();
        self.last_line_needs_update = true;
        self.should_advance_to_last_line_box_block_end = false;
        let mut should_indent = self.line_count() <= 1 || (self.text_indent_each_line && forced == ForcedBreak::Yes);
        if self.text_indent_hanging {
            should_indent = !should_indent;
        }
        if should_indent {
            let indent = self.text_indent;
            self.line_mut(line_index).inline_length += indent;
        }
        self.current_line_committed_pending_margin = false;
    }

    pub(crate) fn break_line(&mut self, forced: ForcedBreak, next_item_inline_size: Option<CssPixels>) {
        // FIXME: Respect inline direction.

        let line_index = self.ensure_last_line_index();
        {
            let mut line = self.line_mut(line_index);
            line.has_break = true;
            line.has_forced_break = forced == ForcedBreak::Yes;
        }
        self.last_line_needs_update = true;
        self.update_last_line();

        let mut break_count = 0usize;
        loop {
            self.context()
                .line_data_mut()
                .line_boxes
                .push(LineBoxData::new(self.direction, self.writing_mode));
            self.begin_new_line(true, break_count == 0, forced);
            break_count += 1;
            let line_height = self.containing_style().line_height();
            let current_line_block_size = self.max_block_size_on_current_line.max(line_height);
            let floats_intrude = self.context().any_floats_intrude_in_block_range(
                self.current_block_offset,
                self.current_block_offset + current_line_block_size,
            );
            let next_too_wide = self
                .available_inline_size_for_current_line
                .pixels_greater_than(next_item_inline_size.unwrap_or_default());
            if !floats_intrude
                || (self
                    .context()
                    .can_fit_new_line_at_block_offset(self.current_block_offset, line_height)
                    && !next_too_wide)
            {
                break;
            }
        }
    }

    pub(crate) fn break_if_needed(&mut self, next_item_inline_size: CssPixels) -> bool {
        if !self.should_break(next_item_inline_size) {
            return false;
        }
        self.break_line(ForcedBreak::No, Some(next_item_inline_size));
        true
    }

    pub(crate) fn append_box(
        &mut self,
        node: Node,
        leading_size: CssPixels,
        trailing_size: CssPixels,
        leading_margin: CssPixels,
        trailing_margin: CssPixels,
        content_baselines: DerivedBaselines,
    ) {
        self.prepare_to_append_inline_content();
        let used = self.context().used(node);
        let content_inline = used.content_inline_size.get();
        let content_block = used.content_block_size.get();
        let border_start = used.border_box_top(false);
        let border_end = used.border_box_bottom(false);
        let margin_block_size = used.margin_box_block_size(false);
        let line_index = self.ensure_last_line_index();
        let fragment_index = self.line(line_index).fragments.len();
        let fragment_facts = self.fragment_facts(node);
        let text_align_is_justify = self.context().style(fragment_facts.style_source).text_align() == text_align::JUSTIFY;
        self.line_mut(line_index).add_fragment(
            node,
            0,
            0,
            leading_size,
            trailing_size,
            leading_margin,
            trailing_margin,
            content_inline,
            content_block,
            border_start,
            border_end,
            None,
            fragment_facts,
            text_align_is_justify,
            TrailingWhitespace::default(),
        );
        self.line_mut(line_index).fragments[fragment_index].content_baselines = Some(content_baselines);
        self.max_block_size_on_current_line = self.max_block_size_on_current_line.max(margin_block_size);
    }

    #[allow(clippy::too_many_arguments)]
    pub(crate) fn append_text_chunk(
        &mut self,
        node: Node,
        offset_in_node: usize,
        length_in_node: usize,
        leading_size: CssPixels,
        trailing_size: CssPixels,
        leading_margin: CssPixels,
        trailing_margin: CssPixels,
        content_inline_size: CssPixels,
        content_block_size: CssPixels,
        glyphs: GlyphData,
        trailing_whitespace: TrailingWhitespace,
    ) {
        self.prepare_to_append_inline_content();
        let line_index = self.ensure_last_line_index();
        let facts = self.fragment_facts(node);
        let text_align_is_justify = self.context().style(facts.style_source).text_align() == text_align::JUSTIFY;
        self.line_mut(line_index).add_fragment(
            node,
            offset_in_node,
            length_in_node,
            leading_size,
            trailing_size,
            leading_margin,
            trailing_margin,
            content_inline_size,
            content_block_size,
            CssPixels::default(),
            CssPixels::default(),
            Some(glyphs),
            facts,
            text_align_is_justify,
            trailing_whitespace,
        );
        let line_block_length = self.line(line_index).block_length;
        self.max_block_size_on_current_line = self.max_block_size_on_current_line.max(line_block_length);
    }

    pub(crate) fn append_static_position_marker(&mut self, node: Node, preceded_by_start_edges: bool) {
        let line_index = self.ensure_last_line_index();
        self.line_mut(line_index)
            .add_static_position_marker(node, preceded_by_start_edges);
    }

    pub(crate) fn prepare_to_append_inline_content(&mut self) {
        if self.current_line_committed_pending_margin {
            return;
        }
        let line_index = self.ensure_last_line_index();
        if self.line(line_index).has_block_level_box {
            return;
        }
        let margin = self.context().parent_commit_pending_margin_before_inline_content();
        if margin != CssPixels::default() {
            self.current_block_offset += margin;
            self.recalculate_available_space();
        }
        self.current_line_committed_pending_margin = true;
        self.pending_margin_follows_block_level_box = false;
    }

    pub(crate) fn commit_pending_margin_before_float(&mut self) {
        if self.pending_margin_follows_block_level_box {
            self.prepare_to_append_inline_content();
        }
    }

    pub(crate) fn finish_current_line_before_block_level_box(&mut self) {
        if self.line_count() == 0 || self.line(self.line_count() - 1).is_empty() {
            return;
        }
        let line_index = self.line_count() - 1;
        self.line_mut(line_index).has_break = true;
        self.line_mut(line_index).has_forced_break = true;
        self.last_line_needs_update = true;
        self.update_last_line();
        self.context()
            .line_data_mut()
            .line_boxes
            .push(LineBoxData::new(self.direction, self.writing_mode));
        self.begin_new_line(true, true, ForcedBreak::No);
    }

    pub(crate) fn line_index_for_block_level_box(&mut self) -> usize {
        let line_index = self.ensure_last_line_index();
        assert!(self.line(line_index).fragments.is_empty());
        line_index
    }

    pub(crate) fn append_block_level_box(
        &mut self,
        node: Node,
        line_index: usize,
        block_end: CssPixels,
        block_end_margin: CssPixels,
    ) {
        let used = self.context().used(node);
        assert!(used.has_content_offset.get());
        let (inline_offset, block_offset) = to_logical(
            self.writing_mode,
            used.content_offset.get().x,
            used.content_offset.get().y,
        );
        let (inline_length, block_length) = to_logical(
            self.writing_mode,
            used.content_inline_size.get(),
            used.content_block_size.get(),
        );
        let border_start = used.border_box_top(false);
        let line_inline_length = to_logical(
            self.writing_mode,
            used.margin_box_inline_size(false),
            used.margin_box_block_size(false),
        )
        .0;
        let ensured_line_index = self.ensure_last_line_index();
        assert_eq!(line_index, ensured_line_index);
        assert!(self.line(line_index).fragments.is_empty());
        let fragment = LineBoxFragmentData::new(
            node,
            0,
            0,
            inline_offset,
            block_offset,
            inline_length,
            block_length,
            border_start,
            self.direction,
            self.writing_mode,
            None,
            self.fragment_facts(node),
        );
        let current_block_offset = self.current_block_offset;
        {
            let mut line = self.line_mut(line_index);
            line.fragments.push(fragment);
            line.inline_length = line_inline_length;
            line.block_length = CssPixels::default();
            line.block_start = current_block_offset;
            line.block_end = block_end;
            line.baseline = CssPixels::default();
            line.has_block_level_box = true;
            line.block_level_box_block_end_margin = block_end_margin;
            line.has_break = true;
            line.has_forced_break = true;
            for marker in &mut line.static_position_markers {
                marker.block_offset += current_block_offset;
            }
        }
        self.pending_margin_follows_block_level_box = true;
        self.current_block_offset = block_end;
        self.max_block_size_on_current_line = CssPixels::default();
        self.last_line_needs_update = false;
        self.should_advance_to_last_line_box_block_end = false;
        self.context()
            .line_data_mut()
            .line_boxes
            .push(LineBoxData::new(self.direction, self.writing_mode));
        self.begin_new_line(false, true, ForcedBreak::No);
    }

    pub(crate) fn ceiling_for_float_to_be_inserted_here(&mut self, node: Node) -> CssPixels {
        let float_inline_size = self.context().used(node).margin_box_inline_size(false);
        let mut candidate = self.current_block_offset;
        let line_index = self.ensure_last_line_index();
        let mut line = self.line_mut(line_index);
        let current_line_inline_size = line.physical_horizontal_extent() - line.trailing_whitespace_inline_size();
        let line_is_empty_or_whitespace = line.is_empty_or_ends_in_whitespace();
        drop(line);
        let mut needed = current_line_inline_size;
        if !line_is_empty_or_whitespace {
            needed += self.unbreakable_run_inline_size_interrupted_by_float;
        }
        self.unbreakable_run_inline_size_interrupted_by_float = CssPixels::default();
        if current_line_inline_size > CssPixels::default()
            && self
                .available_inline_size_for_current_line
                .pixels_greater_than(needed + float_inline_size)
        {
            // This intentionally mixes physical horizontal and inline axes in
            // vertical writing modes, matching the old implementation.
            candidate += self.line(line_index).physical_vertical_extent();
        }
        candidate.max(self.context().block_axis_float_clearance())
    }

    fn should_break(&mut self, next_item_inline_size: CssPixels) -> bool {
        if self.available_inline_size_for_current_line == AvailableSize::MaxContent {
            return false;
        }
        if self.line_count() == 0 || self.line(self.line_count() - 1).is_empty() {
            let line_height = self.containing_style().line_height();
            if !self
                .context()
                .any_floats_intrude_in_block_range(self.current_block_offset, self.current_block_offset + line_height)
            {
                return false;
            }
        }
        let line_index = self.ensure_last_line_index();
        let current_inline_size = self.line(line_index).physical_horizontal_extent();
        self.available_inline_size_for_current_line
            .pixels_greater_than(current_inline_size + next_item_inline_size)
    }

    fn baseline_for_style(style: StyleValues, line_height: CssPixels) -> CssPixels {
        let typographic = CssPixels::nearest_value_for_f32(style.font_ascent() + style.font_descent());
        CssPixels::nearest_value_for_f32(style.font_ascent()) + (line_height - typographic) / 2
    }

    // The style of the box that an inline-level box is aligned against, which is its parent inline box, or the block
    // container for a box the containing block holds directly.
    fn parent_style(&self, node: Node) -> StyleValues<'_> {
        let parent = if node == self.context().containing_block {
            NodeSlotId::INVALID
        } else {
            self.context().parent_node(node)
        };
        if parent.is_invalid() {
            self.containing_style()
        } else {
            self.context().style(parent)
        }
    }

    fn block_offset_for_alignment(
        &self,
        style: StyleValues,
        parent_style: StyleValues,
        metrics: VerticalAlignMetrics,
        line_box_baseline: CssPixels,
    ) -> CssPixels {
        let alphabetic =
            self.current_block_offset + line_box_baseline - metrics.baseline + metrics.effective_box_block_start_offset;
        if !style.vertical_align_is_keyword() {
            return alphabetic - style.vertical_align_value().to_px(metrics.line_height);
        }
        // FIXME: middle, sub and super are defined against the parent inline box's font metrics too, but we use the
        //        block container's instead.
        match style.vertical_align_keyword() {
            vertical_align::BASELINE => alphabetic,
            vertical_align::MIDDLE => {
                let x_height = CssPixels::nearest_value_for_f32(self.containing_style().font_x_height());
                self.current_block_offset
                    + line_box_baseline
                    + (metrics.effective_box_block_start_offset
                        - metrics.effective_box_block_end_offset
                        - x_height
                        - metrics.block_size)
                        / 2
            }
            vertical_align::SUB => alphabetic + self.containing_style().font_size() / 5,
            vertical_align::SUPER => alphabetic - self.containing_style().font_size() / 3,
            // A top- or bottom-aligned box is the root of an aligned subtree, laid out on the baseline here and
            // shifted into place once the final line box size is known. These arms therefore only see boxes that
            // form no aligned subtree: the containing block itself, whose alignment does not apply to the line box
            // it establishes, and every box in a writing mode below.
            // FIXME: Line box alignment still uses physical vertical metrics in non-horizontal writing modes.
            vertical_align::TOP if self.writing_mode == writing_mode::HORIZONTAL_TB => alphabetic,
            vertical_align::TOP => self.current_block_offset + metrics.effective_box_block_start_offset,
            vertical_align::BOTTOM => alphabetic,
            vertical_align::TEXT_TOP => {
                self.current_block_offset + line_box_baseline
                    - CssPixels::nearest_value_for_f32(parent_style.font_ascent())
                    + metrics.effective_box_block_start_offset
            }
            vertical_align::TEXT_BOTTOM => {
                self.current_block_offset
                    + line_box_baseline
                    + CssPixels::nearest_value_for_f32(parent_style.font_descent())
                    - metrics.block_size
                    - metrics.effective_box_block_end_offset
            }
            _ => unreachable!("invalid vertical-align keyword"),
        }
    }

    // https://drafts.csswg.org/css2/#line-height
    fn line_relative_aligned_subtree_root(&self, style_source: Node) -> Option<(Node, u8)> {
        // FIXME: Line box alignment still uses physical vertical metrics in non-horizontal writing modes, where
        //        top and bottom are approximated without forming aligned subtrees.
        if self.writing_mode != writing_mode::HORIZONTAL_TB {
            return None;
        }

        // An aligned subtree contains its inline-level box and the aligned subtrees of all children except any
        // descendant subtree whose root is itself aligned to the line box. Walking outwards therefore stops at the
        // nearest ancestor aligned to the line box, and at the first ancestor that is not an inline box of this
        // formatting context.
        let containing_block = self.context().containing_block;
        let mut node = style_source;
        while !node.is_invalid() && node != containing_block {
            if let Some(alignment) = line_relative_alignment(self.context().style(node)) {
                return Some((node, alignment));
            }
            let parent = self.context().parent_node(node);
            if !parent.is_invalid()
                && parent != containing_block
                && !self.context().facts(parent).is_fragmented_inline()
            {
                break;
            }
            node = parent;
        }
        None
    }

    fn subtree_shift_for_box(
        &self,
        box_: Node,
        aligned_subtrees: &[LineRelativeAlignedSubtree],
        normal_subtree_shift: CssPixels,
    ) -> CssPixels {
        self.line_relative_aligned_subtree_root(box_)
            .and_then(|(root, _)| aligned_subtree_shift(aligned_subtrees, root))
            .unwrap_or(normal_subtree_shift)
    }

    fn inline_box_alignment_metrics(&self, node: Node) -> VerticalAlignMetrics {
        let style = self.context().style(node);
        let used = self.context().used(node);
        VerticalAlignMetrics {
            baseline: Self::baseline_for_style(style, style.line_height()),
            block_size: style.line_height(),
            effective_box_block_start_offset: used.border_box_top(false),
            effective_box_block_end_offset: used.border_box_bottom(false),
            line_height: style.line_height(),
        }
    }

    // https://drafts.csswg.org/css2/#line-height
    pub(crate) fn update_last_line(&mut self) {
        if !self.last_line_needs_update {
            return;
        }
        self.last_line_needs_update = false;
        if self.line_count() == 0 {
            return;
        }
        let line_index = self.line_count() - 1;
        if self.line(line_index).has_block_level_box {
            self.should_advance_to_last_line_box_block_end = false;
            return;
        }
        let has_fragments = !self.line(line_index).fragments.is_empty();
        if has_fragments {
            self.prepare_to_append_inline_content();
        }

        let containing_style = self.containing_style();
        let current_line_block_size = self.max_block_size_on_current_line.max(containing_style.line_height());
        let start_inline_offset = self
            .context()
            .leftmost_inline_offset_at(self.current_block_offset, current_line_block_size);
        let mut inline_offset = start_inline_offset;
        let mut block_offset = CssPixels::default();
        let excess = self.available_inline_size_for_current_line.to_px_or_zero() - self.line(line_index).inline_length;
        if self.writing_mode != writing_mode::HORIZONTAL_TB {
            block_offset =
                self.available_inline_size_for_current_line.to_px_or_zero() - self.line(line_index).block_length;
        }
        if excess > CssPixels::default() {
            match containing_style.text_align() {
                text_align::CENTER | text_align::_LIBWEB_CENTER | text_align::_LIBWEB_INHERIT_OR_CENTER => {
                    inline_offset += excess / 2;
                }
                text_align::START if containing_style.direction() == direction::RTL => inline_offset += excess,
                text_align::END if containing_style.direction() == direction::LTR => inline_offset += excess,
                text_align::RIGHT | text_align::_LIBWEB_RIGHT => inline_offset += excess,
                text_align::MATCH_PARENT => unreachable!("match-parent must be resolved"),
                _ => {}
            }
        } else if excess < CssPixels::default() && containing_style.direction() == direction::RTL {
            // An overflowing line ignores text-align and is aligned to the inline start edge.
            // In a right-to-left container the overflow therefore extends past the line-left edge.
            inline_offset += excess;
        }

        let strut_baseline = Self::baseline_for_style(containing_style, containing_style.line_height());
        let mut should_align_strut_to_line_box_baseline = false;
        let mut line_box_baseline = strut_baseline;
        let fragment_count = self.line(line_index).fragments.len();
        let has_line_relative_aligned_subtree = (0..fragment_count).any(|fragment_index| {
            let style_source = self.line(line_index).fragments[fragment_index].style_source;
            self.line_relative_aligned_subtree_root(style_source).is_some()
        });
        for fragment_index in 0..fragment_count {
            let (node, style_source, content_baselines) = {
                let fragment = &self.line(line_index).fragments[fragment_index];
                (fragment.layout_node, fragment.style_source, fragment.content_baselines)
            };
            let style = self.context().style(style_source);
            let fragment_baseline = if self.context().facts(node).is_text_node() {
                Self::baseline_for_style(style, style.line_height())
            } else if let Some(content_baselines) = content_baselines {
                crate::layout::box_baseline_with_content_baselines(
                    &self.context().callbacks,
                    node,
                    &self.context().used(node),
                    crate::layout::BaselineSet::Last,
                    content_baselines,
                )
            } else {
                crate::layout::box_baseline(
                    &self.context().callbacks,
                    node,
                    &self.context().used(node),
                    crate::layout::BaselineSet::Last,
                )
            };
            self.line_mut(line_index).fragments[fragment_index].baseline = fragment_baseline;
            if self.line_relative_aligned_subtree_root(style_source).is_some() {
                continue;
            }
            let adjusted_baseline = if style.vertical_align_is_keyword() {
                fragment_baseline
            } else {
                fragment_baseline + style.vertical_align_value().to_px(style.line_height())
            };
            if adjusted_baseline > line_box_baseline {
                // A line box holding an aligned subtree can extend past the strut on both sides, so the strut has to
                // be placed relative to the line box baseline instead of the line box block start.
                // FIXME: That is true of every line box, but the line box block start is currently derived both here
                //        and from max_block_size_on_current_line, and those two disagree once the strut moves.
                if !self.context().facts(node).is_text_node() && style.vertical_align_is_keyword() {
                    should_align_strut_to_line_box_baseline |= has_line_relative_aligned_subtree
                        || (style.display().is_inline_outside()
                            && style.display().is_flex_inside()
                            && style.vertical_align_keyword() == vertical_align::BASELINE);
                }
                line_box_baseline = adjusted_baseline;
            }
        }

        let strut_start = self.current_block_offset;
        let strut_end = if should_align_strut_to_line_box_baseline {
            self.current_block_offset + line_box_baseline + (containing_style.line_height() - strut_baseline)
        } else {
            self.current_block_offset + containing_style.line_height()
        };
        let mut earliest = strut_start;
        let mut latest = strut_end;
        let mut aligned_subtrees: Vec<LineRelativeAlignedSubtree> = Vec::new();
        let mut first_unshifted_text_baseline: Option<CssPixels> = None;
        let current_block_offset = self.current_block_offset;
        let mut inline_box_alignments: Vec<InlineBoxAlignment> = Vec::new();

        for fragment_index in 0..fragment_count {
            inline_box_alignments.clear();
            let mut snapshot = {
                let fragment = &self.line(line_index).fragments[fragment_index];
                FragmentAlignmentSnapshot {
                    style_source: fragment.style_source,
                    layout_node: fragment.layout_node,
                    baseline: fragment.baseline,
                    block_size: fragment.physical_vertical_extent(),
                    border_box_block_start: fragment.border_box_block_start,
                    is_atomic_inline: fragment.is_atomic_inline,
                    inline_offset: fragment.inline_offset,
                    block_offset: fragment.block_offset,
                }
            };
            let style = self.context().style(snapshot.style_source);
            let mut metrics = VerticalAlignMetrics {
                baseline: snapshot.baseline,
                block_size: snapshot.block_size,
                effective_box_block_start_offset: snapshot.border_box_block_start,
                // Intentional old quirk: start is reused as end.
                effective_box_block_end_offset: snapshot.border_box_block_start,
                line_height: style.line_height(),
            };
            if snapshot.is_atomic_inline {
                let used = self.context().used(snapshot.layout_node);
                metrics.effective_box_block_start_offset = used.margin_top.get() + used.border_box_top(false);
                metrics.effective_box_block_end_offset = used.margin_bottom.get() + used.border_box_bottom(false);
            }
            let containing_block = self.context().containing_block;
            let new_inline_offset = inline_offset + snapshot.inline_offset;
            let own_alignment_is_line_relative = line_relative_alignment(style).is_some();
            let aligned_subtree = self.line_relative_aligned_subtree_root(snapshot.style_source);
            let alignment_style = if aligned_subtree.is_some_and(|(root, _)| root == snapshot.style_source) {
                style.with_vertical_align_keyword(vertical_align::BASELINE)
            } else {
                style
            };
            let parent_style = self.parent_style(snapshot.style_source);
            let mut new_block_offset =
                self.block_offset_for_alignment(alignment_style, parent_style, metrics, line_box_baseline);
            // Fragments of the containing block count as unshifted, and a fragment whose effective
            // alignment already is baseline sits at its baseline-aligned offset.
            let baseline_aligned_block_offset = if snapshot.style_source == containing_block
                || (alignment_style.vertical_align_is_keyword()
                    && alignment_style.vertical_align_keyword() == vertical_align::BASELINE)
            {
                new_block_offset
            } else {
                self.block_offset_for_alignment(
                    style.with_vertical_align_keyword(vertical_align::BASELINE),
                    parent_style,
                    metrics,
                    line_box_baseline,
                )
            };
            if snapshot.style_source != containing_block
                && self.context().facts(snapshot.style_source).is_fragmented_inline()
            {
                inline_box_alignments.push(InlineBoxAlignment {
                    box_: snapshot.style_source,
                    vertical_shift: new_block_offset - baseline_aligned_block_offset,
                });
            }
            let mut ancestor = if snapshot.style_source == containing_block {
                NodeSlotId::INVALID
            } else {
                self.context().parent_node(snapshot.style_source)
            };
            while !own_alignment_is_line_relative
                && !ancestor.is_invalid()
                && self.context().facts(ancestor).is_fragmented_inline()
                && ancestor != containing_block
            {
                let ancestor_style = self.context().style(ancestor);
                if line_relative_alignment(ancestor_style).is_some() {
                    break;
                }
                if ancestor_style.vertical_align_is_keyword()
                    && ancestor_style.vertical_align_keyword() == vertical_align::BASELINE
                {
                    inline_box_alignments.push(InlineBoxAlignment {
                        box_: ancestor,
                        vertical_shift: CssPixels::default(),
                    });
                    ancestor = self.context().parent_node(ancestor);
                    continue;
                }
                let ancestor_metrics = self.inline_box_alignment_metrics(ancestor);
                let ancestor_parent_style = self.parent_style(ancestor);
                let baseline_style = ancestor_style.with_vertical_align_keyword(vertical_align::BASELINE);
                let ancestor_vertical_shift =
                    self.block_offset_for_alignment(ancestor_style, ancestor_parent_style, ancestor_metrics, line_box_baseline)
                        - self.block_offset_for_alignment(
                            baseline_style,
                            ancestor_parent_style,
                            ancestor_metrics,
                            line_box_baseline,
                        );
                new_block_offset += ancestor_vertical_shift;
                inline_box_alignments.push(InlineBoxAlignment {
                    box_: ancestor,
                    vertical_shift: ancestor_vertical_shift,
                });
                ancestor = self.context().parent_node(ancestor);
            }
            let accumulated_vertical_shift = new_block_offset - baseline_aligned_block_offset;
            {
                let fragment = &mut self.line_mut(line_index).fragments[fragment_index];
                fragment.inline_offset = new_inline_offset;
                fragment.block_offset = new_block_offset.floor() + block_offset;
                fragment.accumulated_vertical_shift = accumulated_vertical_shift;
                snapshot.block_offset = fragment.block_offset;
            }

            let (inline_box_start, mut inline_box_end) = if snapshot.is_atomic_inline {
                let used = self.context().used(snapshot.layout_node);
                (
                    snapshot.block_offset - (used.margin_top.get() + used.border_box_top(false)),
                    snapshot.block_offset
                        + used.content_block_size.get()
                        + used.margin_bottom.get()
                        + used.border_box_bottom(false),
                )
            } else {
                let typographic = CssPixels::nearest_value_for_f32(style.font_ascent() + style.font_descent());
                let half_leading = (style.line_height() - typographic) / 2;
                (
                    snapshot.block_offset + snapshot.baseline
                        - CssPixels::nearest_value_for_f32(style.font_ascent())
                        - half_leading,
                    snapshot.block_offset
                        + snapshot.baseline
                        + CssPixels::nearest_value_for_f32(style.font_descent())
                        + half_leading,
                )
            };
            if !style.vertical_align_is_keyword() {
                inline_box_end += style.vertical_align_value().to_px(style.line_height());
            }
            if let Some((root, alignment)) = aligned_subtree {
                if let Some(subtree) = aligned_subtrees.iter_mut().find(|subtree| subtree.root == root) {
                    subtree.block_start = subtree.block_start.min(inline_box_start);
                    subtree.block_end = subtree.block_end.max(inline_box_end);
                } else {
                    aligned_subtrees.push(LineRelativeAlignedSubtree {
                        root,
                        alignment,
                        block_start: inline_box_start,
                        block_end: inline_box_end,
                        shift: CssPixels::default(),
                    });
                }
            } else {
                earliest = earliest.min(inline_box_start);
                latest = latest.max(inline_box_end);
            }

            let is_text_node = self.context().facts(snapshot.layout_node).is_text_node();
            if is_text_node && self.writing_mode == writing_mode::HORIZONTAL_TB {
                let font_box_size = normal_line_height(style);
                let font_baseline = Self::baseline_for_style(style, font_box_size);
                let fragment_mut = &mut self.line_mut(line_index).fragments[fragment_index];
                fragment_mut.block_offset += fragment_mut.baseline - font_baseline;
                fragment_mut.baseline = font_baseline;
                fragment_mut.block_length = font_box_size;
            }

            let text_fragment_baseline = {
                let fragment = &self.line(line_index).fragments[fragment_index];
                (is_text_node && !fragment.is_fully_truncated).then(|| fragment.block_offset + fragment.baseline)
            };
            if let Some(fragment_baseline) = text_fragment_baseline {
                // Aligned-subtree fragments are placed at their baseline position here and moved by the
                // subtree shift below, so they never count as unshifted.
                let fragment_is_unshifted =
                    aligned_subtree.is_none() && accumulated_vertical_shift == CssPixels::default();
                if first_unshifted_text_baseline.is_none() && fragment_is_unshifted {
                    first_unshifted_text_baseline = Some(fragment_baseline - current_block_offset);
                }

                // https://drafts.csswg.org/css-text-decor-4/#text-line-constancy
                // UAs must adjust line positions to match the shifted metrics of decorating boxes shifted with
                // vertical-align values other than baseline [CSS2] or subscripted/superscripted via
                // font-variant-position [CSS-FONTS-3], but must not adjust the line position or thickness in
                // response to descendants of a decorating box that are so styled.
                let mut inline_box_baseline = fragment_baseline;
                let mut remaining_vertical_shift = accumulated_vertical_shift;
                for alignment in &inline_box_alignments {
                    let inserted = self.line_mut(line_index).set_inline_box_baseline(
                        alignment.box_,
                        inline_box_baseline,
                        remaining_vertical_shift,
                    );
                    // An already recorded box implies its ancestors are recorded as well, since the
                    // walk that recorded it continued through them.
                    if !inserted {
                        break;
                    }
                    inline_box_baseline -= alignment.vertical_shift;
                    remaining_vertical_shift -= alignment.vertical_shift;
                }
            }
        }

        // Top- and bottom-aligned subtrees do not participate in choosing the line box baseline. Once all other boxes
        // have been positioned, grow the line box to the minimum size that can contain every aligned subtree.
        let maximum_block_size = |alignment| {
            aligned_subtrees
                .iter()
                .filter(|subtree| subtree.alignment == alignment)
                .map(|subtree| subtree.block_end - subtree.block_start)
                .max()
                .unwrap_or_default()
        };
        latest = latest.max(earliest + maximum_block_size(vertical_align::TOP));
        earliest = earliest.min(latest - maximum_block_size(vertical_align::BOTTOM));

        let normal_subtree_shift = if has_line_relative_aligned_subtree {
            self.current_block_offset - earliest
        } else {
            CssPixels::default()
        };
        for subtree in &mut aligned_subtrees {
            subtree.shift = if subtree.alignment == vertical_align::TOP {
                self.current_block_offset - subtree.block_start
            } else {
                self.current_block_offset + latest - earliest - subtree.block_end
            };
        }

        if has_line_relative_aligned_subtree {
            for fragment_index in 0..fragment_count {
                let style_source = self.line(line_index).fragments[fragment_index].style_source;
                let shift = self.subtree_shift_for_box(style_source, &aligned_subtrees, normal_subtree_shift);
                let mut line = self.line_mut(line_index);
                let fragment = &mut line.fragments[fragment_index];
                fragment.block_offset += shift;
                // The line baseline moves by the normal subtree shift, so only displacement beyond
                // it keeps a fragment away from its baseline-aligned position.
                fragment.accumulated_vertical_shift += shift - normal_subtree_shift;
            }
            // Inline box baselines were recorded before the subtree shifts, so move them into the
            // shifted frame.
            let baseline_count = self.line(line_index).inline_box_baselines.len();
            for baseline_index in 0..baseline_count {
                let box_ = self.line(line_index).inline_box_baselines[baseline_index].box_;
                let shift = self.subtree_shift_for_box(box_, &aligned_subtrees, normal_subtree_shift);
                let mut line = self.line_mut(line_index);
                let entry = &mut line.inline_box_baselines[baseline_index];
                entry.baseline += shift;
                entry.accumulated_vertical_shift += shift - normal_subtree_shift;
            }
        }

        let marker_count = self.line(line_index).static_position_markers.len();
        for marker_index in 0..marker_count {
            // Static position markers are resolved against the inline box that contains them, so they move with that
            // box's aligned subtree.
            let box_ = self.line(line_index).static_position_markers[marker_index].box_;
            let shift =
                self.subtree_shift_for_box(self.context().parent_node(box_), &aligned_subtrees, normal_subtree_shift);
            let mut line = self.line_mut(line_index);
            let marker = &mut line.static_position_markers[marker_index];
            marker.inline_offset += inline_offset;
            marker.block_offset += block_offset + current_block_offset + shift;
        }

        {
            let mut line = self.line_mut(line_index);
            line.block_length = latest - earliest;
            line.block_start = current_block_offset;
            line.block_end = current_block_offset + line.block_length;
            // Fragment block offsets include the reversed-writing-mode shim, so the alignment
            // baseline fallback must include it as well to share their coordinate space.
            line.baseline =
                first_unshifted_text_baseline.unwrap_or(line_box_baseline + block_offset) + normal_subtree_shift;
        }
        self.should_advance_to_last_line_box_block_end = should_align_strut_to_line_box_baseline;
    }

    pub(crate) fn remove_last_line_if_empty(&mut self) {
        let last_line_is_empty = self.line_count() != 0 && self.line(self.line_count() - 1).is_empty();
        if last_line_is_empty {
            self.context().line_data_mut().line_boxes.pop();
            self.last_line_needs_update = false;
        }
    }

    pub(crate) fn recalculate_available_space(&mut self) {
        let current_line_block_size = self
            .max_block_size_on_current_line
            .max(self.containing_style().line_height());
        self.available_inline_size_for_current_line = self
            .context()
            .available_space_for_line(self.current_block_offset, current_line_block_size);
        if self.line_count() != 0 {
            let line_index = self.line_count() - 1;
            self.line_mut(line_index).original_available_inline_size = self.available_inline_size_for_current_line;
        }
    }

    pub(crate) fn did_introduce_clearance(&mut self, clearance: CssPixels) {
        if clearance <= self.current_block_offset {
            return;
        }
        if self.line_count() > 1 {
            let previous = self.line_count() - 2;
            self.line_mut(previous).block_end = clearance;
        }
        self.current_block_offset = clearance;
    }

    pub(crate) fn set_trailing_whitespace_on_previous_line(&mut self) {
        if self.line_count() < 2 {
            return;
        }
        let previous = self.line_count() - 2;
        if let Some(fragment) = self.line_mut(previous).fragments.last_mut() {
            fragment.has_trailing_whitespace = true;
        }
    }

    pub(crate) fn current_block_offset(&self) -> CssPixels {
        self.current_block_offset
    }

    pub(crate) fn set_unbreakable_run_inline_size_interrupted_by_float(&mut self, inline_size: CssPixels) {
        self.unbreakable_run_inline_size_interrupted_by_float = inline_size;
    }
}

// https://drafts.csswg.org/css2/#propdef-vertical-align
fn line_relative_alignment(style: StyleValues) -> Option<u8> {
    let keyword = style.vertical_align_is_keyword().then(|| style.vertical_align_keyword())?;
    matches!(keyword, vertical_align::TOP | vertical_align::BOTTOM).then_some(keyword)
}

// Returns None for a root with no fragment on this line, which happens when its inline box is fragmented across
// lines; such a line only holds content that moves with the rest of the line box.
fn aligned_subtree_shift(subtrees: &[LineRelativeAlignedSubtree], root: Node) -> Option<CssPixels> {
    subtrees
        .iter()
        .find(|subtree| subtree.root == root)
        .map(|subtree| subtree.shift)
}

pub(crate) fn normal_line_height(style: StyleValues) -> CssPixels {
    let ascent = style.font_ascent().round_ties_even() as i64;
    let descent = style.font_descent().round_ties_even() as i64;
    CssPixels::from_integer(ascent.saturating_add(descent))
}
