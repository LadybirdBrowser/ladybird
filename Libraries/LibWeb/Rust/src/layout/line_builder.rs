/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use line_box_fragment::{GLYPH_TEXT_TYPE_LTR, GLYPH_TEXT_TYPE_RTL};

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

/// Extends the aligned subtree whose root and alignment `subtree` names to hold `start..end`, or the line box
/// `earliest..latest` where the box is in no aligned subtree.
fn extend_aligned_subtree_or_line_box(
    aligned_subtrees: &mut Vec<LineRelativeAlignedSubtree>,
    (earliest, latest): (&mut CssPixels, &mut CssPixels),
    subtree: Option<(Node, u8)>,
    (start, end): (CssPixels, CssPixels),
) {
    let Some((root, alignment)) = subtree else {
        *earliest = (*earliest).min(start);
        *latest = (*latest).max(end);
        return;
    };
    match aligned_subtrees.iter_mut().find(|subtree| subtree.root == root) {
        Some(subtree) => {
            subtree.block_start = subtree.block_start.min(start);
            subtree.block_end = subtree.block_end.max(end);
        }
        None => aligned_subtrees.push(LineRelativeAlignedSubtree {
            root,
            alignment,
            block_start: start,
            block_end: end,
            shift: CssPixels::default(),
        }),
    }
}

struct LineAlignment {
    earliest: CssPixels,
    latest: CssPixels,
    line_box_baseline: CssPixels,
    has_line_relative_aligned_subtree: bool,
    aligned_subtrees: Vec<LineRelativeAlignedSubtree>,
    first_unshifted_text_baseline: Option<CssPixels>,
}

/// What the line-height calculation quirk needs to know about the text on a line, found once for the line.
struct LineTextFacts {
    in_line_height_quirks_mode: bool,
    /// The fragments from this one on are those of the struts of inline boxes.
    strut_fragment_start: usize,
    /// The fragments from the first to the last that isn't collapsible whitespace, all of which the line keeps.
    kept_fragments: std::ops::Range<usize>,
    /// The inline boxes that hold text the line keeps directly, found only in quirks mode.
    boxes_holding_text: std::collections::HashSet<Node>,
    /// Whether a block ellipsis displaced all of the line's content.
    holds_only_block_ellipsis: bool,
}

#[derive(Clone)]
pub(crate) struct LineBuilder<'builder, 'context> {
    context: &'builder inline_formatting_context::InlineFormattingContext<'context>,
    available_inline_size_for_current_line: AvailableSize,
    /// The inline size that text-wrap-style: balance takes off the available inline size of each line of each group
    /// between forced breaks, for the lines to break at.
    balancing_reductions: Vec<Option<CssPixels>>,
    forced_break_count: usize,
    /// Whether this builder finds line breaks without placing the boxes on the lines.
    only_finds_line_breaks: bool,
    /// Heights retained across retries, indexed by content lines rather than empty lines skipped to clear floats.
    float_fit_heights: Option<Vec<CssPixels>>,
    float_fit_line_index: usize,
    float_fit_trial: bool,
    float_refit: Option<(usize, CssPixels)>,
    current_block_offset: CssPixels,
    max_block_size_on_current_line: CssPixels,
    nodes_with_edges_of_collapsed_whitespace: Vec<(Node, CssPixels)>,
    /// The outside markers that the first line box this builder lays out contributes to.
    outside_marker_line: Option<block_formatting_context::OutsideMarkerLine>,
    unbreakable_run_inline_size_interrupted_by_float: CssPixels,
    /// The advance of the preserved white space that a line is breaking at, which hangs at the end of that line.
    hanging_whitespace_inline_size: CssPixels,
    /// The end edges of the inline boxes that end with the white space a line is breaking at, which stay on that line.
    end_edges_of_whitespace_at_break: CssPixels,
    /// How many lines from the first are known to hold no content, while no line is known to: Every line until the
    /// first formatted line is one past floats that held nothing.
    lines_known_to_hold_no_content: Option<usize>,
    text_indentation: inline_formatting_context::TextIndentation,
    direction: u8,
    /// The direction that unicode-bidi: plaintext gives a line from the first strong text of the next content, while
    /// the line holds none yet.
    plaintext_direction_of_next_content: Option<u8>,
    /// The last search for the first strong text ahead: the items it covers, from where it started to where it
    /// stopped, and the text type it found.
    first_strong_text_ahead: Option<(std::ops::RangeInclusive<u32>, Option<u8>)>,
    last_line_needs_update: bool,
    is_continuation: bool,
    should_advance_to_last_line_box_block_end: bool,
    current_line_committed_pending_margin: bool,
    pending_margin_follows_block_level_box: bool,
    containing_style: StyleValues<'context>,
    fragment_facts_cache: Option<(Node, line_box_fragment::FragmentBuildFacts)>,
    style_cache: Cell<Option<(Node, StyleValues<'context>)>>,
    /// The lookups behind the facts last asked for. Keeping the facts themselves copied the pass they
    /// hold into the cell and back, and the copy of the pass's padded tail stalled the loads after it.
    facts_cache: Cell<Option<(Node, &'builder NodeData, Option<ComputedValuesView<'builder>>)>>,
}

impl<'builder, 'context> LineBuilder<'builder, 'context> {
    pub(crate) fn new(context: &'builder inline_formatting_context::InlineFormattingContext<'context>) -> Self {
        let mut builder = Self::initialized(context);
        builder.begin_new_line(false, true, ForcedBreak::No);
        builder
    }

    /// A builder that only finds where the lines break, to balance them.
    pub(crate) fn new_for_finding_balanced_line_breaks(
        context: &'builder inline_formatting_context::InlineFormattingContext<'context>,
    ) -> Self {
        let mut builder = Self::initialized(context);
        builder.only_finds_line_breaks = true;
        builder.begin_new_line(false, true, ForcedBreak::No);
        builder
    }

    pub(crate) fn set_float_fit_heights(&mut self, heights: Vec<CssPixels>, trial: bool) {
        self.float_fit_heights = Some(heights);
        self.float_fit_line_index = 0;
        self.float_fit_trial = trial;
        self.float_refit = None;
        if trial {
            self.only_finds_line_breaks = true;
        }
        self.recalculate_available_space();
    }

    fn float_fit_height(&self) -> CssPixels {
        self.float_fit_heights
            .as_ref()
            .and_then(|heights| heights.get(self.float_fit_line_index))
            .copied()
            .unwrap_or_default()
    }

    pub(crate) fn float_refit(&self) -> Option<(usize, CssPixels)> {
        self.float_refit
    }

    pub(crate) fn set_balancing_reductions(&mut self, balancing_reductions: Vec<Option<CssPixels>>) {
        self.balancing_reductions = balancing_reductions;
    }

    pub(crate) fn new_for_continuation(
        context: &'builder inline_formatting_context::InlineFormattingContext<'context>,
        after_forced_break: bool,
    ) -> Self {
        let mut builder = Self::initialized(context);
        builder.is_continuation = true;
        builder.begin_new_line(
            false,
            true,
            if after_forced_break {
                ForcedBreak::Yes
            } else {
                ForcedBreak::No
            },
        );
        builder
    }

    fn initialized(context: &'builder inline_formatting_context::InlineFormattingContext<'context>) -> Self {
        let style = context.style(context.containing_block);
        let containing_inline_size = context.input.containing_block_constraints.inline_basis();
        Self {
            context,
            available_inline_size_for_current_line: AvailableSize::Indefinite,
            balancing_reductions: Vec::new(),
            forced_break_count: 0,
            only_finds_line_breaks: false,
            float_fit_heights: None,
            float_fit_line_index: 0,
            float_fit_trial: false,
            float_refit: None,
            current_block_offset: CssPixels::default(),
            max_block_size_on_current_line: CssPixels::default(),
            nodes_with_edges_of_collapsed_whitespace: Vec::new(),
            outside_marker_line: context.parent.pending_outside_marker_line(),
            unbreakable_run_inline_size_interrupted_by_float: CssPixels::default(),
            hanging_whitespace_inline_size: CssPixels::default(),
            end_edges_of_whitespace_at_break: CssPixels::default(),
            lines_known_to_hold_no_content: Some(0),
            text_indentation: inline_formatting_context::TextIndentation::of(style, containing_inline_size),
            direction: style.direction(),
            plaintext_direction_of_next_content: None,
            first_strong_text_ahead: None,
            last_line_needs_update: false,
            is_continuation: false,
            should_advance_to_last_line_box_block_end: false,
            current_line_committed_pending_margin: false,
            pending_margin_follows_block_level_box: false,
            containing_style: style,
            fragment_facts_cache: None,
            style_cache: Cell::new(None),
            facts_cache: Cell::new(None),
        }
    }

    pub(crate) fn new_after_reused_lines(
        context: &'builder inline_formatting_context::InlineFormattingContext<'context>,
    ) -> Self {
        assert!(!context.line_data().line_boxes.is_empty());
        let current_block_offset = context.line_data().line_boxes.last().unwrap().next_line_block_offset;
        let mut builder = Self::initialized(context);
        builder.current_block_offset = current_block_offset;
        builder.push_line_box();
        builder.begin_new_line(false, true, ForcedBreak::No);
        builder.current_line_committed_pending_margin = true;
        builder
    }

    fn context(&self) -> &inline_formatting_context::InlineFormattingContext<'context> {
        self.context
    }

    fn line_count(&self) -> usize {
        self.context().line_data().line_boxes.len()
    }

    fn push_line_box(&self) {
        self.context()
            .line_data_mut()
            .line_boxes
            .push(line_box::LineBoxData::new(
                self.direction,
                // NB: Lines are laid out in the frame of their formatting context, where they run horizontally.
                writing_mode::HORIZONTAL_TB,
                self.context().current_line_start.get(),
            ));
    }

    fn ensure_last_line_index(&mut self) -> usize {
        if self.line_count() == 0 {
            self.push_line_box();
        }
        self.line_count() - 1
    }

    fn line(&self, index: usize) -> Ref<'_, line_box::LineBoxData> {
        Ref::map(self.context().line_data(), |data| &data.line_boxes[index])
    }

    fn line_mut(&self, index: usize) -> RefMut<'_, line_box::LineBoxData> {
        RefMut::map(self.context().line_data_mut(), |data| &mut data.line_boxes[index])
    }

    fn containing_style(&self) -> StyleValues<'_> {
        self.containing_style
    }

    fn style(&self, node: Node) -> StyleValues<'context> {
        if let Some((cached_node, style)) = self.style_cache.get()
            && cached_node == node
        {
            return style;
        }
        let style = self.context().style(node);
        self.style_cache.set(Some((node, style)));
        style
    }

    fn facts(&self, node: Node) -> NodeFacts<'builder> {
        if let Some((cached_node, data, style_payloads)) = self.facts_cache.get()
            && cached_node == node
        {
            return NodeFacts::from_lookups(&self.context.pass, node, data, style_payloads);
        }
        let facts = NodeFacts::new(&self.context.pass, node);
        self.facts_cache.set(Some((node, facts.data(), facts.style_view())));
        facts
    }

    fn fragment_facts(&mut self, node: Node) -> line_box_fragment::FragmentBuildFacts {
        if let Some((cached_node, facts)) = self.fragment_facts_cache
            && cached_node == node
        {
            return facts;
        }
        let style_source = self.context().style_source(node);
        let style = self.style(style_source);
        let facts = self.facts(node);
        let (text_utf16, text_length) = if facts.is_text_node() {
            let text = &self.context().pass.text_content(node).text;
            (text.as_ptr(), text.len())
        } else {
            (std::ptr::null(), 0)
        };
        let facts = line_box_fragment::FragmentBuildFacts {
            style_source,
            is_atomic_inline: facts.is_atomic_inline(),
            white_space_collapse: style.white_space_collapse(),
            text_utf16,
            text_length_in_code_units: text_length,
        };
        self.fragment_facts_cache = Some((node, facts));
        facts
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
        if self.float_fit_heights.is_some() {
            self.max_block_size_on_current_line = CssPixels::default();
        }
        self.recalculate_available_space();
        let line_index = self.ensure_last_line_index();
        self.line_mut(line_index).original_available_inline_size = self.available_inline_size_for_current_line;
        self.max_block_size_on_current_line = CssPixels::default();
        self.last_line_needs_update = true;
        self.should_advance_to_last_line_box_block_end = false;
        // https://drafts.csswg.org/css-text-3/#text-indent-property
        // "only lines that are the first formatted line of an element are affected."
        // NB: Line boxes beside floats that no content fit beside come before the first formatted line, rather than
        //     being it.
        // NB: Each line before this one is checked once, from where the check for the line before it stopped.
        while let Some(checked) = self.lines_known_to_hold_no_content
            && checked < line_index
        {
            let holds_no_content = {
                let line = self.line(checked);
                !line.counts_toward_widows_and_orphans() && line.block_level_box.is_none()
            };
            self.lines_known_to_hold_no_content = holds_no_content.then_some(checked + 1);
        }
        let is_first_formatted_line = !self.is_continuation && self.lines_known_to_hold_no_content.is_some();
        let indent = self
            .text_indentation
            .indent_of_line(is_first_formatted_line, forced == ForcedBreak::Yes);
        {
            let mut line = self.line_mut(line_index);
            line.inline_length += indent;
            line.indent = indent;
        }
        self.current_line_committed_pending_margin = false;
    }

    pub(crate) fn break_line(
        &mut self,
        forced: ForcedBreak,
        forced_break: Option<inline_content::ForcedBreakPosition>,
        next_size: Option<CssPixels>,
    ) {
        self.break_line_sized_by(forced, forced_break, next_size, None);
    }

    /// Breaks the line, as break_line() does. `size_at`, where given, is the inline size of the next content where a
    /// line starts at the given position, for content whose size depends on that (tabs, e.g.): Each line the break
    /// moves past floats to is checked with the size the content would have there.
    fn break_line_sized_by(
        &mut self,
        forced: ForcedBreak,
        forced_break: Option<inline_content::ForcedBreakPosition>,
        next_size: Option<CssPixels>,
        mut size_at: Option<&mut dyn FnMut(CssPixels) -> CssPixels>,
    ) {
        // FIXME: Respect inline direction.

        self.keep_line_of_collapsed_whitespace_edges();
        let line_index = self.ensure_last_line_index();
        if forced == ForcedBreak::No && !self.line(line_index).fragments.is_empty() {
            let end_edges = std::mem::take(&mut self.end_edges_of_whitespace_at_break);
            self.line_mut(line_index).advance_inline_length(end_edges);
        }
        {
            let mut line = self.line_mut(line_index);
            line.has_break = true;
            line.has_forced_break = forced == ForcedBreak::Yes;
            line.forced_break = forced_break;
        }
        if forced == ForcedBreak::No {
            self.hyphenate_end_of_line(line_index);
        }
        self.last_line_needs_update = true;
        self.update_last_line(forced == ForcedBreak::No || next_size.is_some());
        if forced == ForcedBreak::Yes || !self.line(line_index).fragments.is_empty() {
            self.float_fit_line_index += 1;
        }
        if forced == ForcedBreak::Yes {
            self.forced_break_count += 1;
        }

        let mut break_count = 0usize;
        loop {
            self.push_line_box();
            self.begin_new_line(true, break_count == 0, forced);
            break_count += 1;
            let line_height = self.containing_style().line_height();
            let current_line_block_size = self
                .max_block_size_on_current_line
                .max(line_height)
                .max(self.float_fit_height());
            let floats_intrude = self.context().any_floats_intrude_in_block_range(
                self.current_block_offset,
                self.current_block_offset + current_line_block_size,
            );
            let available = self.available_inline_size_for_current_line;
            let indent = self.line(self.line_count() - 1).indent;
            let next_size_on_line = match size_at.as_mut() {
                Some(size_at) => {
                    let position = self.inline_position_of_next_content();
                    size_at(position)
                }
                None => next_size.unwrap_or_default(),
            };
            let next_too_wide = available.pixels_greater_than(next_size_on_line + indent);
            if !floats_intrude
                || (self
                    .context()
                    .can_fit_new_line_at_block_offset(self.current_block_offset, current_line_block_size)
                    && !next_too_wide)
            {
                break;
            }
        }
    }

    /// Where the next content on the current line starts, from the start edge of the block container's content box:
    /// past the floats on the start side of the line's direction, any indent, and the content already on the line.
    /// Notes `item` as the next content, for the direction that the first strong text from it on gives a line under
    /// unicode-bidi: plaintext, which tabs that start the item are placed for.
    pub(crate) fn note_next_content(
        &mut self,
        prepared: &prepared_inline_content::PreparedInlineContent,
        item: &inline_level_iterator::Item,
    ) {
        self.plaintext_direction_of_next_content = None;
        if self.containing_style().unicode_bidi() != unicode_bidi::PLAINTEXT || !item.starts_with_tabs(prepared) {
            return;
        }
        let index = item.prepared_item_index();
        let text_type = match &self.first_strong_text_ahead {
            Some((covered, text_type)) if covered.contains(&index) => *text_type,
            _ => {
                let (stop, text_type) = prepared.first_strong_text_from(index);
                self.first_strong_text_ahead = Some((index..=stop, text_type));
                text_type
            }
        };
        self.plaintext_direction_of_next_content = text_type.map(|text_type| {
            if text_type == GLYPH_TEXT_TYPE_RTL {
                direction::RTL
            } else {
                direction::LTR
            }
        });
    }

    fn line_holds_strong_text(&self, line_index: usize) -> bool {
        self.line(line_index).fragments.iter().any(|fragment| {
            fragment
                .glyphs
                .as_ref()
                .is_some_and(|glyphs| matches!(glyphs.text_type, GLYPH_TEXT_TYPE_LTR | GLYPH_TEXT_TYPE_RTL))
        })
    }

    pub(crate) fn inline_position_of_next_content(&mut self) -> CssPixels {
        let line_index = self.ensure_last_line_index();
        let line_block_size = self
            .max_block_size_on_current_line
            .max(self.containing_style().line_height())
            .max(self.float_fit_height());
        // NB: Under unicode-bidi: plaintext, a line takes its direction from its first strong text, which content
        //     before that text is placed for too.
        let line_direction = match self.plaintext_direction_of_next_content {
            Some(direction) if !self.line_holds_strong_text(line_index) => direction,
            _ => self.line(line_index).direction,
        };
        let start_side_intrusion = self.context().start_side_float_intrusion(
            self.current_block_offset,
            self.current_block_offset + line_block_size,
            line_direction,
        );
        start_side_intrusion + self.line(line_index).inline_length
    }

    pub(crate) fn current_line_is_empty(&mut self) -> bool {
        self.line_count() == 0 || self.line(self.line_count() - 1).is_empty()
    }

    pub(crate) fn can_break_before_next_item(&mut self) -> bool {
        !self.current_line_is_empty() || self.floats_intrude_at_current_line_start()
    }

    fn floats_intrude_at_current_line_start(&self) -> bool {
        let line_height = self.containing_style().line_height().max(self.float_fit_height());
        self.context()
            .any_floats_intrude_in_block_range(self.current_block_offset, self.current_block_offset + line_height)
    }

    /// The inline size the current line breaks at. Balancing breaks lines before they fill the available inline size,
    /// but they still align within it.
    fn inline_size_to_break_current_line_at(&self) -> AvailableSize {
        let balancing_reduction = self
            .balancing_reductions
            .get(self.forced_break_count)
            .copied()
            .flatten();
        match (self.available_inline_size_for_current_line, balancing_reduction) {
            (AvailableSize::Definite(available), Some(reduction)) => AvailableSize::Definite(available - reduction),
            (available, _) => available,
        }
    }

    fn remaining_inline_size_on_current_line_within(&mut self, inline_size: AvailableSize) -> Option<CssPixels> {
        let AvailableSize::Definite(available) = inline_size else {
            return None;
        };
        let line_index = self.ensure_last_line_index();
        Some(available - self.line(line_index).physical_horizontal_extent())
    }

    pub(crate) fn remaining_inline_size_for_soft_wrap(&mut self) -> Option<CssPixels> {
        self.remaining_inline_size_on_current_line_within(self.inline_size_to_break_current_line_at())
    }

    /// NB: Balancing does not break words that fit the available inline size, so they overflow the inline size the
    ///     line breaks at instead.
    pub(crate) fn remaining_inline_size_for_overflow_break(&mut self) -> Option<CssPixels> {
        self.remaining_inline_size_on_current_line_within(self.available_inline_size_for_current_line)
    }

    pub(crate) fn only_finds_line_breaks(&self) -> bool {
        self.only_finds_line_breaks
    }

    pub(crate) fn break_if_needed(&mut self, next_item_inline_size: CssPixels) -> bool {
        if !self.should_break(next_item_inline_size) {
            return false;
        }
        self.break_line(ForcedBreak::No, None, Some(next_item_inline_size));
        true
    }

    /// Breaks the line, as break_if_needed() does, before content whose inline size depends on where its line starts:
    /// `size_at` gives that size for a line starting at a position.
    pub(crate) fn break_if_needed_sized_by(
        &mut self,
        next_item_inline_size: CssPixels,
        size_at: &mut dyn FnMut(CssPixels) -> CssPixels,
    ) -> bool {
        if !self.should_break(next_item_inline_size) {
            return false;
        }
        self.break_line_sized_by(ForcedBreak::No, None, Some(next_item_inline_size), Some(size_at));
        true
    }

    pub(crate) fn break_if_needed_before_overflow_breakable_item(&mut self, next_item_inline_size: CssPixels) -> bool {
        if self.current_line_is_empty() {
            return false;
        }
        if !self.should_break(next_item_inline_size) {
            return false;
        }
        self.break_line(ForcedBreak::No, None, None);
        true
    }

    pub(crate) fn note_soft_wrap_opportunity(&mut self) {
        let line_index = self.ensure_last_line_index();
        let mut line = self.line_mut(line_index);
        if self.context().parent.has_line_clamp()
            && let Some(fragment) = line
                .fragments
                .iter_mut()
                .rfind(|fragment| !fragment.is_justifiable_whitespace())
        {
            fragment.has_soft_wrap_opportunity_after = true;
        }
    }
    pub(crate) fn current_line_has_no_space_left_by_floats(&mut self) -> bool {
        let Some(remaining) =
            self.remaining_inline_size_on_current_line_within(self.available_inline_size_for_current_line)
        else {
            return false;
        };
        if remaining > CssPixels::default() {
            return false;
        }
        let line_height = self.containing_style().line_height();
        let block_start = self.current_block_offset;
        let block_end = block_start
            + self
                .max_block_size_on_current_line
                .max(line_height)
                .max(self.float_fit_height());
        self.context().any_floats_intrude_in_block_range(block_start, block_end)
    }

    /// Appends the box of `item`, an element item, to the current line.
    pub(crate) fn append_box(&mut self, item: &inline_level_iterator::Item) {
        let node = item.node;
        let leading_size = item.border_start + item.padding_start;
        let trailing_size = item.padding_end + item.border_end;
        let box_cells = item.box_cells();
        self.prepare_to_append_inline_content();
        let content_inline = box_cells.content_inline_size;
        let content_block = box_cells.content_block_size;
        let border_start = box_cells.border_box_top(false);
        let border_end = box_cells.border_box_bottom(false);
        let margin_block_size = box_cells.margin_box_block_size(false);
        let line_index = self.ensure_last_line_index();
        let fragment_index = self.line(line_index).fragments.len();
        let fragment_facts = self.fragment_facts(node);
        let text_align_is_justify = may_justify_lines(self.containing_style());
        self.line_mut(line_index).add_fragment(
            node,
            0,
            0,
            leading_size,
            trailing_size,
            item.margin_start,
            item.margin_end,
            content_inline,
            content_block,
            border_start,
            border_end,
            None,
            fragment_facts,
            text_align_is_justify,
            line_box_fragment::TrailingWhitespace::default(),
        );
        {
            let mut line = self.line_mut(line_index);
            let fragment = &mut line.fragments[fragment_index];
            fragment.content_baselines = Some(item.content_baselines());
            fragment.box_cells = Some(box_cells);
        }
        self.max_block_size_on_current_line = self.max_block_size_on_current_line.max(margin_block_size);
    }

    pub(crate) fn append_inline_edge(&mut self, item: &inline_level_iterator::Item) {
        self.append_empty_fragment(
            item.node,
            item.border_start + item.padding_start,
            item.border_end + item.padding_end,
            item.margin_start,
            item.margin_end,
        );
    }

    fn append_empty_fragment(
        &mut self,
        node: Node,
        leading_size: CssPixels,
        trailing_size: CssPixels,
        leading_margin: CssPixels,
        trailing_margin: CssPixels,
    ) {
        self.prepare_to_append_inline_content();
        let line_index = self.ensure_last_line_index();
        let facts = self.fragment_facts(node);
        let block_size = TextLayoutBounds::new(self.style(facts.style_source), None).block_size();
        self.line_mut(line_index).add_fragment(
            node,
            0,
            0,
            leading_size,
            trailing_size,
            leading_margin,
            trailing_margin,
            CssPixels::default(),
            block_size,
            CssPixels::default(),
            CssPixels::default(),
            None,
            facts,
            false,
            line_box_fragment::TrailingWhitespace::default(),
        );
        self.max_block_size_on_current_line = self.max_block_size_on_current_line.max(block_size);
    }

    /// Collapsed whitespace adds no fragment, so that whitespace after it keeps collapsing too.
    fn append_edges_of_collapsed_whitespace(
        &mut self,
        margin: CssPixels,
        border: CssPixels,
        padding: CssPixels,
    ) -> bool {
        let none = CssPixels::default();
        if margin == none && border == none && padding == none {
            return false;
        }
        self.prepare_to_append_inline_content();
        let line_index = self.ensure_last_line_index();
        self.line_mut(line_index)
            .advance_inline_length(margin + border + padding);
        true
    }

    pub(crate) fn append_start_edges_of_collapsed_whitespace(
        &mut self,
        node: Node,
        margin: CssPixels,
        border: CssPixels,
        padding: CssPixels,
    ) {
        if self.append_edges_of_collapsed_whitespace(margin, border, padding) {
            let inline_offset = self.line(self.line_count() - 1).inline_length;
            self.note_edges_of_collapsed_whitespace(node, inline_offset);
        }
    }

    fn note_edges_of_collapsed_whitespace(&mut self, node: Node, inline_offset: CssPixels) {
        match self.nodes_with_edges_of_collapsed_whitespace.last_mut() {
            Some(last) if last.0 == node => last.1 = inline_offset,
            _ => self
                .nodes_with_edges_of_collapsed_whitespace
                .push((node, inline_offset)),
        }
    }

    pub(crate) fn append_end_edges_of_collapsed_whitespace(
        &mut self,
        node: Node,
        margin: CssPixels,
        border: CssPixels,
        padding: CssPixels,
    ) {
        if self.append_edges_of_collapsed_whitespace(margin, border, padding) {
            let inline_offset = self.line(self.line_count() - 1).inline_length - (margin + border + padding);
            self.note_edges_of_collapsed_whitespace(node, inline_offset);
        }
    }

    /// Nonzero inline-axis edges keep a line box even when it contains no text.
    /// https://drafts.csswg.org/css-inline-3/#invisible-line-boxes
    pub(crate) fn keep_line_of_collapsed_whitespace_edges(&mut self) {
        let nodes = std::mem::take(&mut self.nodes_with_edges_of_collapsed_whitespace);
        if nodes.is_empty() || !self.current_line_is_empty() {
            return;
        }
        let none = CssPixels::default();
        for (node, inline_offset) in nodes {
            // NB: The line has already advanced past these edges, so each fragment goes back to where its node was.
            let distance = self.line(self.line_count() - 1).inline_length - inline_offset;
            self.append_empty_fragment(node, none, none, none - distance, distance);
        }
    }

    pub(crate) fn append_text_item(
        &mut self,
        item: &mut inline_level_iterator::Item,
        prepared: &prepared_inline_content::PreparedInlineContent,
        style: StyleValues,
    ) {
        let glyphs = item.take_glyph_run(prepared);
        let text_type = glyphs.text_type;
        let content_block_size = TextLayoutBounds::new(style, Some(glyphs.font)).block_size();
        self.append_text_chunk(
            item.node,
            item.offset_in_node,
            item.length_in_node,
            item.border_start + item.padding_start,
            item.padding_end + item.border_end,
            item.margin_start,
            item.margin_end,
            item.inline_size,
            content_block_size,
            glyphs,
            item.trailing_whitespace,
        );
        // https://drafts.csswg.org/css-overflow-4/#block-ellipsis
        // AD-HOC: Block ellipses displace content at soft wraps. Legacy clamping preserves glyph-based truncation
        //         inside shaped runs, so only standard clamping keeps these internal opportunities.
        if self.context().parent.has_line_clamp() && !self.context().parent.clamped_content_is_scrollable_overflow() {
            let line_index = self.line_count() - 1;
            let mut line = self.line_mut(line_index);
            let fragment = line.fragments.last_mut().unwrap();
            let text = fragment.block_ellipsis_text.get_or_insert_default();
            text.runs.push((item.offset_in_node + item.length_in_node, text_type));
            text.soft_wrap_offsets.extend(item.soft_wrap_offsets(prepared));
        }
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
        glyphs: line_box_fragment::ItemGlyphRun<'_>,
        trailing_whitespace: line_box_fragment::TrailingWhitespace,
    ) {
        self.prepare_to_append_inline_content();
        let line_index = self.ensure_last_line_index();
        if self.containing_style().unicode_bidi() == unicode_bidi::PLAINTEXT
            && !self.line_holds_strong_text(line_index)
            && matches!(glyphs.text_type, GLYPH_TEXT_TYPE_LTR | GLYPH_TEXT_TYPE_RTL)
        {
            self.line_mut(line_index).direction = if glyphs.text_type == GLYPH_TEXT_TYPE_RTL {
                direction::RTL
            } else {
                direction::LTR
            };
        }
        let facts = self.fragment_facts(node);
        let text_align_is_justify = may_justify_lines(self.containing_style());
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

    /// Puts `box_`, an inline box that holds no in-flow content, at the end of the current line, for the line to take
    /// its strut from. Its content begins `leading_edges` past the content of the line: The item after the box takes
    /// the start edges between them. Such a box is on one line alone, so one that the line before this one holds
    /// stays there.
    pub(crate) fn add_strut_of_empty_inline_box(&mut self, box_: Node, leading_edges: CssPixels) {
        let line_index = self.ensure_last_line_index();
        if line_index > 0
            && self
                .line(line_index - 1)
                .inline_box_struts
                .iter()
                .any(|strut| strut.box_ == box_)
        {
            return;
        }
        let mut line = self.line_mut(line_index);
        let inline_offset = line.inline_length + leading_edges;
        match line.inline_box_struts.iter_mut().find(|strut| strut.box_ == box_) {
            Some(strut) => strut.inline_offset = inline_offset,
            None => line.add_inline_box_strut(box_, leading_edges),
        }
    }

    /// Puts the inline boxes that `node` is inside on the current line, for the line to take the strut of each one
    /// that no fragment on the line belongs to. `leading_edges` is the size of the start edges that `node` has yet to
    /// add to the line.
    pub(crate) fn add_struts_of_inline_boxes_around(&mut self, node: Node, leading_edges: CssPixels) {
        self.add_struts_of_inline_boxes_around_where(node, leading_edges, |_| true);
    }

    /// Puts the inline boxes that `node` is inside and that `includes` picks on the current line, as
    /// add_struts_of_inline_boxes_around() does.
    pub(crate) fn add_struts_of_inline_boxes_around_where(
        &mut self,
        node: Node,
        leading_edges: CssPixels,
        includes: impl Fn(Node) -> bool,
    ) {
        let line_index = self.ensure_last_line_index();
        let mut box_ = self.context().nearest_fragmented_inline_ancestor(node);
        while !box_.is_invalid() {
            if includes(box_) {
                self.line_mut(line_index).add_inline_box_strut(box_, leading_edges);
            }
            box_ = self.context().nearest_fragmented_inline_ancestor(box_);
        }
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
        self.keep_line_of_collapsed_whitespace_edges();
        if self.line_count() == 0 || self.line(self.line_count() - 1).is_empty() {
            return;
        }
        let line_index = self.line_count() - 1;
        self.line_mut(line_index).has_break = true;
        self.line_mut(line_index).has_forced_break = true;
        self.last_line_needs_update = true;
        self.update_last_line(false);
        self.push_line_box();
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
        block_level_box: line_box::BlockLevelBoxOnLine,
    ) {
        let block = block_level_box.cells;
        let inline_offset = block_level_box.content_offset.x;
        let block_offset = block_level_box.content_offset.y;
        let inline_length = block.content_inline_size;
        let block_length = block.content_block_size;
        let border_start = block.border_box_top(false);
        let line_inline_length = block.margin_box_inline_size(false);
        let ensured_line_index = self.ensure_last_line_index();
        assert_eq!(line_index, ensured_line_index);
        assert!(self.line(line_index).fragments.is_empty());
        let fragment = line_box_fragment::LineBoxFragmentData::new(
            node,
            0,
            0,
            inline_offset,
            block_offset,
            inline_length,
            block_length,
            border_start,
            self.direction,
            writing_mode::HORIZONTAL_TB,
            None,
            self.fragment_facts(node),
        );
        let current_block_offset = self.current_block_offset;
        {
            let mut line = self.line_mut(line_index);
            line.push_fragment(fragment);
            line.inline_length = line_inline_length;
            line.block_length = CssPixels::default();
            line.block_start = current_block_offset;
            line.block_end = block_end;
            line.baseline = CssPixels::default();
            line.has_block_level_box = true;
            line.block_level_box = Some(block_level_box);
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
        self.push_line_box();
        self.begin_new_line(false, true, ForcedBreak::No);
    }

    /// The block offset a float whose margin box is `float_inline_size` wide is placed at: beside the content of the
    /// current line where the float fits there, and below that line otherwise.
    pub(crate) fn ceiling_for_float_to_be_inserted_here(&mut self, float_inline_size: CssPixels) -> CssPixels {
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
            // NB: Align the content so far within the line box, as finishing the line does, to find how far that
            //     line box reaches. Aligning moves the fragments of the line, so put them back afterwards; the line is
            //     aligned again once it is finished.
            let line_before_alignment = self.line(line_index).clone();
            let alignment = self.align_fragments_of_line(line_index, CssPixels::default(), CssPixels::default());
            *self.line_mut(line_index) = line_before_alignment;
            let line_height = self.containing_style().line_height();
            candidate += self
                .max_block_size_on_current_line
                .max(line_height)
                .max(alignment.latest - alignment.earliest);
        }
        candidate.max(self.context().block_axis_float_clearance())
    }

    fn should_break(&mut self, next_item_inline_size: CssPixels) -> bool {
        let inline_size_to_break_at = if self.current_line_is_empty() {
            if !self.floats_intrude_at_current_line_start() {
                return false;
            }
            // NB: Balancing moves breaks between the content of lines, not lines past floats.
            self.available_inline_size_for_current_line
        } else {
            self.inline_size_to_break_current_line_at()
        };
        let line_index = self.ensure_last_line_index();
        let current_inline_size = self.line(line_index).physical_horizontal_extent();
        inline_size_to_break_at.pixels_greater_than(current_inline_size + next_item_inline_size)
    }

    pub(super) fn baseline_for_style(style: StyleValues, line_height: CssPixels) -> CssPixels {
        TextLayoutBounds::with_leading(style.font_ascent(), style.font_descent(), line_height).ascent
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
            self.style(parent)
        }
    }

    // https://drafts.csswg.org/css2/#propdef-vertical-align
    // The vertical-align of the block container establishing this formatting context positions the container itself
    // (a table-cell within its row, an inline-level box within its parent's line box) and does not apply to the line
    // boxes it holds: its direct text belongs to anonymous inline boxes, which take the initial value 'baseline' as
    // vertical-align is not inherited (https://drafts.csswg.org/css2/#anonymous-block-level).
    fn alignment_style<'a>(&self, style_source: Node, style: StyleValues<'a>) -> StyleValues<'a> {
        if style_source == self.context().containing_block {
            style.with_vertical_align_keyword(vertical_align::BASELINE)
        } else {
            style
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
        match style.vertical_align_keyword() {
            vertical_align::BASELINE => alphabetic,
            vertical_align::MIDDLE => {
                // https://drafts.csswg.org/css2/#valdef-vertical-align-middle
                // Align the vertical midpoint of the box with the baseline of the parent box plus half the x-height of
                // the parent.
                let x_height = CssPixels::nearest_value_for_f32(parent_style.font_x_height());
                self.current_block_offset
                    + line_box_baseline
                    + (metrics.effective_box_block_start_offset
                        - metrics.effective_box_block_end_offset
                        - x_height
                        - metrics.block_size)
                        / 2
            }
            vertical_align::_LIBWEB_BASELINE_MIDDLE => {
                self.current_block_offset
                    + line_box_baseline
                    + (metrics.effective_box_block_start_offset
                        - metrics.effective_box_block_end_offset
                        - metrics.block_size)
                        / 2
            }
            vertical_align::SUB => alphabetic + parent_style.font_size() / 5,
            vertical_align::SUPER => alphabetic - parent_style.font_size() / 3,
            // A top- or bottom-aligned box is the root of an aligned subtree, laid out on the baseline here and
            // shifted into place once the final line box size is known. These arms therefore only see boxes that
            // form no aligned subtree: the containing block itself, whose alignment does not apply to the line box
            // it establishes.
            vertical_align::TOP | vertical_align::BOTTOM => alphabetic,
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
        // An aligned subtree contains its inline-level box and the aligned subtrees of all children except any
        // descendant subtree whose root is itself aligned to the line box. Walking outwards therefore stops at the
        // nearest ancestor aligned to the line box, and at the first ancestor that is not an inline box of this
        // formatting context.
        let containing_block = self.context().containing_block;
        let mut node = style_source;
        while !node.is_invalid() && node != containing_block {
            if let Some(alignment) = line_relative_alignment(self.style(node)) {
                return Some((node, alignment));
            }
            let parent = self.context().parent_node(node);
            if !parent.is_invalid() && parent != containing_block && !self.facts(parent).is_fragmented_inline() {
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

    fn text_fragment_layout_bounds(
        &self,
        line_index: usize,
        fragment_index: usize,
        style: StyleValues,
    ) -> TextLayoutBounds {
        let fragment = &self.line(line_index).fragments[fragment_index];
        TextLayoutBounds::new(style, fragment.glyphs.as_ref().map(|glyphs| &glyphs.font))
    }

    /// How far the box that `metrics` describe reaches above the baseline of the line box, where `style` aligns it
    /// within the box of `parent_style`.
    fn extent_above_line_box_baseline(
        &self,
        style: StyleValues,
        parent_style: StyleValues,
        metrics: VerticalAlignMetrics,
    ) -> CssPixels {
        let block_start = self.block_offset_for_alignment(style, parent_style, metrics, CssPixels::default())
            - metrics.effective_box_block_start_offset;
        self.current_block_offset - block_start
    }

    /// How far its own vertical-align shifts the baseline of `box_`, an inline box, down from where 'baseline' puts it.
    fn vertical_shift_of_inline_box(&self, box_: Node) -> CssPixels {
        let style = self.style(box_);
        if style.vertical_align_is_keyword() && style.vertical_align_keyword() == vertical_align::BASELINE {
            return CssPixels::default();
        }
        let metrics = self.inline_box_alignment_metrics(box_);
        let parent_style = self.parent_style(box_);
        let baseline_style = style.with_vertical_align_keyword(vertical_align::BASELINE);
        self.block_offset_for_alignment(style, parent_style, metrics, CssPixels::default())
            - self.block_offset_for_alignment(baseline_style, parent_style, metrics, CssPixels::default())
    }

    /// The inline boxes of this formatting context around `style_source`, innermost first, up to the first one aligned
    /// to the line box, each with the shift of its baseline by its own vertical-align.
    fn vertical_shifts_of_enclosing_inline_boxes(&self, style_source: Node) -> Vec<(Node, CssPixels)> {
        let containing_block = self.context().containing_block;
        let mut shifts = Vec::new();
        if style_source == containing_block {
            return shifts;
        }
        let mut ancestor = self.context().parent_node(style_source);
        while !ancestor.is_invalid() && ancestor != containing_block && self.facts(ancestor).is_fragmented_inline() {
            if line_relative_alignment(self.style(ancestor)).is_some() {
                break;
            }
            shifts.push((ancestor, self.vertical_shift_of_inline_box(ancestor)));
            ancestor = self.context().parent_node(ancestor);
        }
        shifts
    }

    fn fragment_alignment_metrics(
        &self,
        line_index: usize,
        fragment_index: usize,
        style: StyleValues,
    ) -> VerticalAlignMetrics {
        let line = self.line(line_index);
        let fragment = &line.fragments[fragment_index];
        let mut metrics = VerticalAlignMetrics {
            baseline: fragment.baseline,
            block_size: fragment.physical_vertical_extent(),
            effective_box_block_start_offset: fragment.border_box_block_start,
            // Intentional old quirk: start is reused as end.
            effective_box_block_end_offset: fragment.border_box_block_start,
            line_height: style.line_height(),
        };
        if fragment.is_atomic_inline {
            let atomic_inline = fragment.box_cells();
            metrics.effective_box_block_start_offset = atomic_inline.margin_top + atomic_inline.border_box_top(false);
            metrics.effective_box_block_end_offset =
                atomic_inline.margin_bottom + atomic_inline.border_box_bottom(false);
        }
        metrics
    }

    fn inline_box_alignment_metrics(&self, node: Node) -> VerticalAlignMetrics {
        let style = self.style(node);
        let sides = self.context().sides_of_entered_inline_box(node);
        VerticalAlignMetrics {
            baseline: Self::baseline_for_style(style, style.line_height()),
            block_size: style.line_height(),
            effective_box_block_start_offset: sides.border_box_top(false),
            effective_box_block_end_offset: sides.border_box_bottom(false),
            line_height: style.line_height(),
        }
    }

    /// The baseline of the line box at `line_index` from its block start.
    fn line_box_baseline(
        &self,
        line_index: usize,
        text: &LineTextFacts,
        has_line_relative_aligned_subtree: bool,
    ) -> CssPixels {
        let containing_style = self.containing_style();
        let mut line_box_baseline = if self.line_has_strut_of_block_container(line_index, text) {
            Self::baseline_for_style(containing_style, containing_style.line_height())
        } else {
            CssPixels::from_raw(i32::MIN)
        };
        let fragment_count = self.line(line_index).fragments.len();
        for fragment_index in 0..fragment_count {
            if !self.line(line_index).fragments[fragment_index].takes_part_in_line_alignment() {
                continue;
            }
            // NB: An ellipsis is text in the block's style, whichever node it stands at.
            let (node, style_source, content_baselines, box_cells, is_ellipsis) = {
                let fragment = &self.line(line_index).fragments[fragment_index];
                (
                    fragment.layout_node,
                    fragment.style_source,
                    fragment.content_baselines,
                    fragment.box_cells,
                    fragment.is_block_ellipsis || fragment.is_text_overflow_ellipsis,
                )
            };
            let style = self.style(style_source);
            let fragment_baseline =
                if is_ellipsis || self.facts(node).is_text_node() || self.facts(node).is_fragmented_inline() {
                    self.text_fragment_layout_bounds(line_index, fragment_index, style)
                        .ascent
                } else {
                    let box_cells = box_cells.expect("the fragment of an element item carries the cells of its box");
                    formatting_context::inline_box_baseline(
                        &self.context().pass,
                        node,
                        &box_cells,
                        content_baselines
                            .expect("the fragment of an element item carries the content baselines of its box"),
                    )
                };
            self.line_mut(line_index).fragments[fragment_index].baseline = fragment_baseline;
            if has_line_relative_aligned_subtree
                && let Some((root, _)) = self.line_relative_aligned_subtree_root(style_source)
            {
                // NB: The inline boxes around the root of an aligned subtree are aligned like any other, and hold
                //     their own layout bounds around their baselines. Those inside an aligned subtree of their own
                //     don't take part in choosing the baseline of the line box.
                let enclosing_shifts = self.vertical_shifts_of_enclosing_inline_boxes(root);
                let mut ancestor_shift = enclosing_shifts
                    .iter()
                    .fold(CssPixels::default(), |total, (_, shift)| total + *shift);
                for (ancestor, shift) in enclosing_shifts {
                    if self.line_relative_aligned_subtree_root(ancestor).is_none()
                        && self.inline_box_has_strut_on_line(text, ancestor)
                    {
                        let ascent = TextLayoutBounds::new(self.style(ancestor), None).ascent - ancestor_shift;
                        line_box_baseline = line_box_baseline.max(ascent);
                    }
                    ancestor_shift -= shift;
                }
                continue;
            }
            let alignment_style = self.alignment_style(style_source, style);
            // https://drafts.csswg.org/css2/#line-height
            // The line box height is the distance between the uppermost box top and the lowermost box bottom.
            // NB: So, the baseline of the line box is as far below its top as the box that reaches highest above it
            //     once aligned, which is how far its vertical-align and those of the inline boxes around it shift it.
            let enclosing_shifts = self.vertical_shifts_of_enclosing_inline_boxes(style_source);
            let enclosing_shift = enclosing_shifts
                .iter()
                .fold(CssPixels::default(), |total, (_, shift)| total + *shift);
            let metrics = self.fragment_alignment_metrics(line_index, fragment_index, style);
            let adjusted_baseline =
                self.extent_above_line_box_baseline(alignment_style, self.parent_style(style_source), metrics)
                    - enclosing_shift;
            if self.fragment_extends_line_box(line_index, text, fragment_index) {
                line_box_baseline = line_box_baseline.max(adjusted_baseline);
            }
            // https://drafts.csswg.org/css2/#line-height
            // The inline-level boxes are aligned vertically according to their 'vertical-align' property.
            // NB: An inline box around the fragment holds its own layout bounds around its baseline, wherever its
            //     vertical-align and those of the boxes around it put that, which a box whose content is only other
            //     inline boxes has no text fragment of its own to bring in.
            let mut ancestor_shift = enclosing_shift;
            for (ancestor, shift) in enclosing_shifts {
                if self.inline_box_has_strut_on_line(text, ancestor) {
                    let ascent = TextLayoutBounds::new(self.style(ancestor), None).ascent - ancestor_shift;
                    line_box_baseline = line_box_baseline.max(ascent);
                }
                ancestor_shift -= shift;
            }
        }
        if let Some(marker_line) = self.outside_marker_line(line_index) {
            line_box_baseline = line_box_baseline.max(marker_line.ascent);
        }
        // NB: A line with nothing on it that reaches above its baseline has its baseline at its top.
        if line_box_baseline == CssPixels::from_raw(i32::MIN) {
            return CssPixels::default();
        }
        line_box_baseline
    }

    fn line_text_facts(&self, line_index: usize, strut_fragment_start: usize) -> LineTextFacts {
        let in_line_height_quirks_mode = self
            .facts(self.context().containing_block)
            .document_in_line_height_quirks_mode();
        let line = self.line(line_index);
        let fragments = &line.fragments[..strut_fragment_start];
        let is_content = |fragment: &line_box_fragment::LineBoxFragmentData| {
            fragment.takes_part_in_line_alignment() && !Self::is_collapsible_whitespace(fragment)
        };
        let kept_fragments = match (
            fragments.iter().position(is_content),
            fragments.iter().rposition(is_content),
        ) {
            (Some(first), Some(last)) => first..last + 1,
            _ => 0..0,
        };
        let mut boxes_holding_text = std::collections::HashSet::new();
        if in_line_height_quirks_mode {
            for (fragment_index, fragment) in fragments.iter().enumerate() {
                // NB: A block ellipsis borrows the text node of the content it displaces, but isn't text in the
                //     block container.
                if fragment.takes_part_in_line_alignment()
                    && !fragment.is_block_ellipsis
                    && (kept_fragments.contains(&fragment_index) || !Self::is_collapsible_whitespace(fragment))
                    && self.facts(fragment.layout_node).is_text_node()
                {
                    boxes_holding_text.insert(fragment.style_source);
                }
            }
        }
        let holds_only_block_ellipsis = fragments.iter().any(|fragment| fragment.is_block_ellipsis)
            && fragments
                .iter()
                .all(|fragment| fragment.is_block_ellipsis || !is_content(fragment));
        LineTextFacts {
            in_line_height_quirks_mode,
            strut_fragment_start,
            kept_fragments,
            boxes_holding_text,
            holds_only_block_ellipsis,
        }
    }

    /// Whether the line at `line_index` starts with the strut of the block container.
    fn line_has_strut_of_block_container(&self, line_index: usize, text: &LineTextFacts) -> bool {
        // https://drafts.csswg.org/css2/#line-height
        // The minimum height consists of a minimum height above the baseline and a minimum depth below it, exactly as
        // if each line box starts with a zero-width inline box with the element's font and line height properties.
        if !text.in_line_height_quirks_mode {
            return true;
        }
        // https://quirks.spec.whatwg.org/#the-line-height-calculation-quirk
        // In quirks mode and limited-quirks mode, an inline box that matches the following conditions, must, for the
        // purpose of line height calculation, act as if the box had a line-height of zero.
        // NB: The zero-width inline box that starts each line is the block container's own, so it matches them on a
        //     line with no text directly in the block container. Only a forced break directly in the block container
        //     that's alone on its line gives it its strut back, and a list item keeps its strut on every line.
        // NB: So does a line whose content a block ellipsis displaced, which holds the ellipsis alone.
        let containing_block = self.context().containing_block;
        if self.containing_style().display().is_list_item()
            || text.boxes_holding_text.contains(&containing_block)
            || text.holds_only_block_ellipsis
        {
            return true;
        }
        let line = self.line(line_index);
        let has_content = line.fragments.iter().enumerate().any(|(fragment_index, fragment)| {
            fragment.takes_part_in_line_alignment()
                && if fragment_index < text.strut_fragment_start {
                    !Self::is_collapsible_whitespace(fragment)
                } else {
                    self.strut_brings_line_height(line_index, text, fragment.style_source)
                }
        });
        !has_content
            && line.forced_break.is_some_and(|forced_break| {
                !forced_break.node.is_invalid() && self.context().parent_node(forced_break.node) == containing_block
            })
    }

    /// Whether `box_`, an inline box around content on the line that `text` describes, brings its own line height to
    /// it.
    fn inline_box_has_strut_on_line(&self, text: &LineTextFacts, box_: Node) -> bool {
        // https://quirks.spec.whatwg.org/#the-line-height-calculation-quirk
        // In quirks mode and limited-quirks mode, an inline box that matches the following conditions, must, for the
        // purpose of line height calculation, act as if the box had a line-height of zero.
        // - The border-top-width, border-bottom-width, padding-top and padding-bottom properties have a used value of
        //   zero and the box has a vertical writing mode, or the border-right-width, border-left-width,
        //   padding-right and padding-left properties have a used value of zero and the box has a horizontal writing
        //   mode.
        // - It either contains no text or it contains only collapsed whitespace.
        // NB: An inline box contains text on a line when it holds text there directly.
        if !text.in_line_height_quirks_mode {
            return true;
        }
        let sides = self.context().sides_of_entered_inline_box(box_);
        let none = CssPixels::default();
        sides.border_left != none
            || sides.border_right != none
            || sides.padding_left != none
            || sides.padding_right != none
            || text.boxes_holding_text.contains(&box_)
    }

    /// Whether the fragment at `fragment_index` on the line at `line_index`, which `text` describes, extends its line
    /// box.
    fn fragment_extends_line_box(&self, line_index: usize, text: &LineTextFacts, fragment_index: usize) -> bool {
        // https://quirks.spec.whatwg.org/#the-line-height-calculation-quirk
        // NB: The collapsible whitespace that a line starts or ends with isn't text the line keeps, so in quirks mode,
        //     it has no line height to bring to it either.
        if !text.in_line_height_quirks_mode {
            return true;
        }
        let fragment = &self.line(line_index).fragments[fragment_index];
        if fragment_index >= text.strut_fragment_start {
            return self.strut_brings_line_height(line_index, text, fragment.style_source);
        }
        // NB: A block ellipsis has the line height of the block container, which only the line's strut brings.
        if fragment.is_block_ellipsis {
            return false;
        }
        text.kept_fragments.contains(&fragment_index) || !Self::is_collapsible_whitespace(fragment)
    }

    /// Whether the strut of `box_`, an inline box on the line at `line_index` with none of its content there, brings
    /// its line height to the line.
    fn strut_brings_line_height(&self, line_index: usize, text: &LineTextFacts, box_: Node) -> bool {
        // https://quirks.spec.whatwg.org/#the-line-height-calculation-quirk
        // NB: In quirks mode, such a box has a line height only if it has a border or padding at its start or end, or
        //     if the line's forced break is inside it.
        if !text.in_line_height_quirks_mode || self.inline_box_has_strut_on_line(text, box_) {
            return true;
        }
        let Some(forced_break) = self.line(line_index).forced_break else {
            return false;
        };
        let containing_block = self.context().containing_block;
        let mut node = forced_break.node;
        while !node.is_invalid() && node != containing_block {
            if node == box_ {
                return true;
            }
            node = self.context().parent_node(node);
        }
        false
    }

    fn is_collapsible_whitespace(fragment: &line_box_fragment::LineBoxFragmentData) -> bool {
        matches!(
            fragment.white_space_collapse,
            white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
        ) && (fragment.is_justifiable_whitespace()
            || (fragment.length_in_code_units > 0
                && fragment.trailing_whitespace.length_in_code_units == fragment.length_in_code_units))
    }

    /// The outside markers whose list items have the line at `line_index` as their first line box.
    fn outside_marker_line(&self, line_index: usize) -> Option<block_formatting_context::OutsideMarkerLine> {
        let line = self.line(line_index);
        if line.is_empty() || line.has_block_level_box || self.has_line_box_before(line_index) {
            return None;
        }
        self.outside_marker_line
    }

    /// Whether a line before the line at `line_index` holds content.
    pub(crate) fn has_line_box_before(&self, line_index: usize) -> bool {
        (0..line_index).any(|index| !self.line(index).is_empty())
    }

    // https://drafts.csswg.org/css-text-3/#hyphenation
    // When a word is hyphenated, the UA must insert a hyphen [...] at the end of the line.
    /// Shows a hyphen at the end of a line that breaks after a soft hyphen. The hyphen joins the soft hyphen's own
    /// fragment, in its place, so that it takes the soft hyphen's style.
    fn hyphenate_end_of_line(&mut self, line_index: usize) {
        let (fragment_index, hyphen, font, text_type, is_upright, soft_hyphen_offset) = {
            let line = self.line(line_index);
            // NB: Only the empty edges of inline boxes that end after the soft hyphen can follow its fragment. An atomic
            //     inline after it means the line broke after that instead.
            let Some(fragment_index) = line.fragments.iter().rposition(|fragment| {
                fragment.glyphs.is_some() || fragment.length_in_code_units != 0 || fragment.is_atomic_inline
            }) else {
                return;
            };
            let fragment = &line.fragments[fragment_index];
            let Some(glyphs) = fragment.glyphs.as_ref() else {
                return;
            };
            let text_content = self.context().pass.text_content(fragment.layout_node);
            if !text_chunker::breaks_at_soft_hyphen(&text_content.text, fragment.start + fragment.length_in_code_units)
            {
                return;
            }
            let style = self.style(fragment.style_source);
            if !inline_level_iterator::soft_hyphen_hyphenates(style) {
                return;
            }
            // NB: The hyphen goes in the direction of the run that ends the fragment, which runs of either direction can
            //     share.
            let text_type = if fragment.current_insert_direction == direction::RTL {
                GLYPH_TEXT_TYPE_RTL
            } else {
                GLYPH_TEXT_TYPE_LTR
            };
            let hyphen = inline_level_iterator::hyphen_glyph_run(
                &glyphs.font,
                text_type,
                style.letter_spacing().to_double() as f32,
                style.word_spacing().to_double() as f32,
            );
            (
                fragment_index,
                hyphen,
                glyphs.font.clone(),
                text_type,
                glyphs.is_upright,
                fragment.length_in_code_units - 1,
            )
        };
        let inline_size = CssPixels::nearest_value_for_f32(hyphen.width);
        let mut line = self.line_mut(line_index);
        for fragment in &mut line.fragments[fragment_index + 1..] {
            fragment.inline_offset += inline_size;
        }
        for marker in &mut line.static_position_markers {
            if marker.fragments_before > fragment_index {
                marker.inline_offset += inline_size;
            }
        }
        let fragment = &mut line.fragments[fragment_index];
        fragment.append_glyph_run(
            &line_box_fragment::ItemGlyphRun {
                glyphs: std::borrow::Cow::Borrowed(&hyphen.glyphs),
                font: &font,
                text_type,
                width: hyphen.width,
                is_upright,
            },
            inline_size,
            soft_hyphen_offset,
        );
        if let Some(glyphs) = fragment.glyphs.as_mut() {
            glyphs.width += hyphen.width;
        }
        line.inline_length += inline_size;
    }

    pub(crate) fn set_outside_marker_line(&mut self, marker_line: Option<block_formatting_context::OutsideMarkerLine>) {
        self.outside_marker_line = marker_line;
    }

    // https://drafts.csswg.org/css2/#line-height
    pub(crate) fn update_last_line(&mut self, has_immediate_continuation: bool) {
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
        // NB: A line that holds nothing but a forced break is a line box all the same, so the margins before it end
        //     there as they do before any other.
        if has_fragments || self.line(line_index).has_forced_break {
            self.prepare_to_append_inline_content();
        }
        let text = self.line_text_facts(line_index, self.line(line_index).fragments.len());
        if self.line(line_index).takes_inline_box_struts() {
            let strut_count = self.line(line_index).inline_box_struts.len();
            for strut_index in 0..strut_count {
                let box_ = self.line(line_index).inline_box_struts[strut_index].box_;
                if !self.strut_brings_line_height(line_index, &text, box_) {
                    continue;
                }
                let block_size = TextLayoutBounds::new(self.style(box_), None).block_size();
                self.max_block_size_on_current_line = self.max_block_size_on_current_line.max(block_size);
            }
        }
        let containing_style = self.containing_style;
        // NB: Under the line-height quirk, a line without the strut of the block container is as tall as its content.
        let strut_block_size = if self.line_has_strut_of_block_container(line_index, &text) {
            containing_style.line_height()
        } else {
            CssPixels::default()
        };
        let unclamped_line_block_size = self.max_block_size_on_current_line.max(strut_block_size);
        if !self.only_finds_line_breaks && (has_fragments || self.line(line_index).has_forced_break) {
            self.context().prepare_line_for_line_clamp(
                line_index,
                has_immediate_continuation,
                self.current_block_offset + unclamped_line_block_size,
            );
        }

        // https://drafts.csswg.org/css-text-3/#white-space-phase-2
        // "A sequence of collapsible spaces at the end of a line is removed"
        // NB: That happens before the line is aligned, so that its alignment doesn't count the spaces, and mirroring
        //     uses the retained line width, so that discarded trailing spaces don't become leading gaps.
        if self.line(line_index).inline_length_before_block_ellipsis.is_none() {
            self.line_mut(line_index).trim_trailing_whitespace();
        }

        let current_line_block_size = if self.line(line_index).inline_length_before_block_ellipsis.is_some() {
            self.line(line_index)
                .visible_fragments()
                .map(|fragment| {
                    if fragment.is_atomic_inline {
                        fragment.box_cells().margin_box_block_size(false)
                    } else {
                        fragment.block_length
                    }
                })
                .max()
                .unwrap_or_default()
                .max(strut_block_size)
        } else {
            unclamped_line_block_size
        };
        let fitted_height = current_line_block_size.max(self.float_fit_height());
        if self.float_fit_trial && has_fragments {
            let line_before_alignment = self.line(line_index).clone();
            let alignment = self.align_fragments_of_line(line_index, CssPixels::default(), CssPixels::default());
            *self.line_mut(line_index) = line_before_alignment;
            let height = alignment.latest - alignment.earliest;
            let available = self
                .context()
                .available_space_for_line(self.current_block_offset, height);
            if height > self.float_fit_height()
                && (available != self.available_inline_size_for_current_line
                    || self
                        .context()
                        .leftmost_inline_offset_at(self.current_block_offset, height)
                        != self
                            .context()
                            .leftmost_inline_offset_at(self.current_block_offset, fitted_height))
            {
                self.float_refit.get_or_insert((self.float_fit_line_index, height));
            }
        }
        let start_inline_offset = self
            .context()
            .leftmost_inline_offset_at(self.current_block_offset, fitted_height);
        let uses_last_line_alignment = !has_immediate_continuation || self.line(line_index).has_forced_break;
        self.line_mut(line_index).uses_last_line_alignment = Some(uses_last_line_alignment);
        let text_align = line_text_align(containing_style, uses_last_line_alignment);
        // NB: A line's direction is its block's, unless unicode-bidi: plaintext gave the line the direction of its own
        //     first strong character, and start and end are that direction's edges.
        let line_direction = self.line(line_index).direction;
        let aligned_inline_offset = |inline_length| {
            let excess = self.available_inline_size_for_current_line.to_px_or_zero() - inline_length;
            if excess < CssPixels::default() {
                return if line_direction == direction::RTL {
                    start_inline_offset + excess
                } else {
                    start_inline_offset
                };
            }
            start_inline_offset
                + match text_align {
                    text_align::CENTER | text_align::_WEBKIT_CENTER | text_align::_LIBWEB_INHERIT_OR_CENTER => {
                        excess / 2
                    }
                    text_align::START if line_direction == direction::RTL => excess,
                    text_align::END if line_direction == direction::LTR => excess,
                    text_align::RIGHT | text_align::_WEBKIT_RIGHT => excess,
                    text_align::MATCH_PARENT => unreachable!("match-parent must be resolved"),
                    _ => CssPixels::default(),
                }
        };
        let inline_offset = aligned_inline_offset(self.line(line_index).inline_length);
        self.line_mut(line_index).placed_inline_start = start_inline_offset;
        let block_offset = CssPixels::default();

        let LineAlignment {
            earliest,
            latest,
            line_box_baseline,
            has_line_relative_aligned_subtree,
            mut aligned_subtrees,
            first_unshifted_text_baseline,
        } = self.align_fragments_of_line(line_index, inline_offset, block_offset);
        let fragment_count = self.line(line_index).fragments.len();
        let current_block_offset = self.current_block_offset;

        let refitted = self.float_fit_height() > CssPixels::default();
        let normal_subtree_shift = if has_line_relative_aligned_subtree || refitted {
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

        if has_line_relative_aligned_subtree || refitted {
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
        let marker_inline_offset = self
            .line(line_index)
            .inline_length_before_block_ellipsis
            .map_or(inline_offset, aligned_inline_offset);
        for marker_index in 0..marker_count {
            // Static position markers are resolved against the inline box that contains them, so they move with that
            // box's aligned subtree.
            let box_ = self.line(line_index).static_position_markers[marker_index].box_;
            let shift = self.subtree_shift_for_box(
                self.context().parent_node(box_),
                &aligned_subtrees,
                normal_subtree_shift,
            );
            let mut line = self.line_mut(line_index);
            // NB: A marker records where its box would have been in the logical order of the line's content, which a
            //     right-to-left line lays out from its right end.
            // FIXME: Resolve the marker's position within the visual order of the line's bidi runs.
            if line.direction == direction::RTL {
                let content_inline_length = line.inline_length;
                let marker = &mut line.static_position_markers[marker_index];
                marker.inline_offset = content_inline_length - marker.inline_offset;
            }
            let marker = &mut line.static_position_markers[marker_index];
            marker.inline_offset += marker_inline_offset;
            marker.block_offset += block_offset + current_block_offset + shift;
        }
        {
            // NB: An inline box after preserved white space that hangs at the end of the line comes after that white
            //     space, even though the line's inline length doesn't count it.
            let hanging_whitespace_inline_size = self.hanging_whitespace_inline_size;
            let fragments_are_mirrored = self.context().atomic_items_are_right_to_left;
            let mut line = self.line_mut(line_index);
            let content_inline_length = line.inline_length;
            let indent = line.indent;
            let is_right_to_left = line.direction == direction::RTL;
            for strut in &mut line.inline_box_struts {
                let line_end = content_inline_length + hanging_whitespace_inline_size;
                let offset = strut.inline_offset.min(line_end);
                // NB: A box between two fragments sits between them as they're laid out, which is in logical order
                //     unless the fragments are mirrored, and moved by the line's indent as they are. A box at either
                //     end of a right-to-left line is at that end in the line's own direction.
                let is_at_line_edge = offset <= indent || offset >= line_end;
                strut.inline_offset = marker_inline_offset
                    + if is_right_to_left && (fragments_are_mirrored || is_at_line_edge) {
                        content_inline_length - offset
                    } else if is_right_to_left {
                        offset - indent
                    } else {
                        offset
                    };
            }
        }
        {
            let mut line = self.line_mut(line_index);
            line.block_length = latest - earliest;
            line.block_start = current_block_offset;
            line.block_end = current_block_offset + line.block_length;
            // Retain the advance used by begin_new_line, so a reused prefix resumes at the same position.
            line.next_line_block_offset = line.block_end;
            // Fragment block offsets include the reversed-writing-mode shim, so the alignment
            // baseline fallback must include it as well to share their coordinate space.
            line.baseline =
                first_unshifted_text_baseline.unwrap_or(line_box_baseline + block_offset) + normal_subtree_shift;
        }
        self.should_advance_to_last_line_box_block_end = true;
    }

    /// Aligns the fragments of the line at `line_index` within its line box, and finds the extent of that line box.
    fn align_fragments_of_line(
        &self,
        line_index: usize,
        inline_offset: CssPixels,
        block_offset: CssPixels,
    ) -> LineAlignment {
        // https://drafts.csswg.org/css-inline-3/#invisible-line-boxes
        // An inline box on the line with none of its content there is aligned like any other box, as a fragment as
        // wide as nothing. The line holds that fragment only while its fragments are aligned — so, nothing else
        // gets to see it.
        let fragment_count = self.line(line_index).fragments.len();
        for strut in self.inline_box_struts_without_fragments(line_index) {
            let fragment = self.fragment_of_inline_box_strut(line_index, strut);
            self.line_mut(line_index).push_fragment(fragment);
        }
        let alignment =
            self.align_fragments_and_struts_of_line(line_index, fragment_count, inline_offset, block_offset);
        self.line_mut(line_index).fragments.truncate(fragment_count);
        alignment
    }

    /// The struts of the line at `line_index` that count: those of inline boxes that no fragment on the line belongs
    /// to, on a line that isn't empty apart from them.
    fn inline_box_struts_without_fragments(&self, line_index: usize) -> Vec<line_box::InlineBoxStrut> {
        let line = self.line(line_index);
        if line.inline_box_struts.is_empty() || !line.takes_inline_box_struts() {
            return Vec::new();
        }
        let containing_block = self.context().containing_block;
        let holds_fragment = |box_: Node| {
            line.visible_fragments().any(|fragment| {
                let mut node = fragment.style_source;
                while !node.is_invalid() && node != containing_block {
                    if node == box_ {
                        return true;
                    }
                    node = self.context().parent_node(node);
                }
                false
            })
        };
        line.inline_box_struts
            .iter()
            .filter(|strut| !holds_fragment(strut.box_))
            .copied()
            .collect()
    }

    fn fragment_of_inline_box_strut(
        &self,
        line_index: usize,
        strut: line_box::InlineBoxStrut,
    ) -> line_box_fragment::LineBoxFragmentData {
        let style = self.style(strut.box_);
        let line = self.line(line_index);
        line_box_fragment::LineBoxFragmentData::new(
            strut.box_,
            0,
            0,
            strut.inline_offset,
            CssPixels::default(),
            CssPixels::default(),
            TextLayoutBounds::new(style, None).block_size(),
            CssPixels::default(),
            line.direction,
            line.writing_mode,
            None,
            line_box_fragment::FragmentBuildFacts {
                style_source: strut.box_,
                is_atomic_inline: false,
                white_space_collapse: style.white_space_collapse(),
                text_utf16: std::ptr::null(),
                text_length_in_code_units: 0,
            },
        )
    }

    /// Aligns the fragments of the line at `line_index`, the ones from `strut_fragment_start` on being those of the
    /// struts of inline boxes.
    fn align_fragments_and_struts_of_line(
        &self,
        line_index: usize,
        strut_fragment_start: usize,
        inline_offset: CssPixels,
        block_offset: CssPixels,
    ) -> LineAlignment {
        let containing_style = self.containing_style();
        let strut_baseline = Self::baseline_for_style(containing_style, containing_style.line_height());
        let fragment_count = self.line(line_index).fragments.len();
        let has_line_relative_aligned_subtree = self.line(line_index).fragments.iter().any(|fragment| {
            fragment.takes_part_in_line_alignment()
                && self.line_relative_aligned_subtree_root(fragment.style_source).is_some()
        });
        let text = self.line_text_facts(line_index, strut_fragment_start);
        let line_box_baseline = self.line_box_baseline(line_index, &text, has_line_relative_aligned_subtree);

        // https://drafts.csswg.org/css2/#line-height
        // The minimum height consists of a minimum height above the baseline and a minimum depth below it, exactly as
        // if each line box starts with a zero-width inline box with the element's font and line height properties.
        let has_strut = self.line_has_strut_of_block_container(line_index, &text);
        // FIXME: The strut belongs on the baseline in a vertical line too, but that's the central baseline there, which
        //        the layout bounds of text don't center on yet. So, it stays at the block start of the line box.
        let strut_start = if containing_style.writing_mode() == writing_mode::HORIZONTAL_TB {
            self.current_block_offset + line_box_baseline - strut_baseline
        } else {
            self.current_block_offset
        };
        // NB: Without the strut, the line box starts out holding nothing, and each box it holds extends it.
        let (mut earliest, mut latest) = if has_strut {
            (strut_start, strut_start + containing_style.line_height())
        } else {
            (CssPixels::from_raw(i32::MAX), CssPixels::from_raw(i32::MIN))
        };
        if let Some(marker_line) = self.outside_marker_line(line_index) {
            earliest = earliest.min(self.current_block_offset + line_box_baseline - marker_line.ascent);
            latest = latest.max(self.current_block_offset + line_box_baseline + marker_line.descent);
        }
        let mut aligned_subtrees: Vec<LineRelativeAlignedSubtree> = Vec::new();
        let mut first_unshifted_text_baseline: Option<CssPixels> = None;
        let current_block_offset = self.current_block_offset;
        let mut inline_box_alignments: Vec<InlineBoxAlignment> = Vec::new();

        for fragment_index in 0..fragment_count {
            if !self.line(line_index).fragments[fragment_index].takes_part_in_line_alignment() {
                continue;
            }
            inline_box_alignments.clear();
            let mut snapshot = {
                let fragment = &self.line(line_index).fragments[fragment_index];
                FragmentAlignmentSnapshot {
                    style_source: fragment.style_source,
                    layout_node: fragment.layout_node,
                    baseline: fragment.baseline,
                    is_atomic_inline: fragment.is_atomic_inline,
                    inline_offset: fragment.inline_offset,
                    block_offset: fragment.block_offset,
                }
            };
            let style = self.style(snapshot.style_source);
            let metrics = self.fragment_alignment_metrics(line_index, fragment_index, style);
            let containing_block = self.context().containing_block;
            let fragment_inline_offset = if self.context().atomic_items_are_right_to_left {
                self.line(line_index).inline_length
                    - snapshot.inline_offset
                    - self.line(line_index).fragments[fragment_index].inline_length
            } else if self.line(line_index).direction == direction::RTL {
                // https://drafts.csswg.org/css-text-3/#text-indent-property
                // "The indent is treated as a margin applied to the start edge of the line box."
                // NB: The start edge of a right-to-left line is its right end, so the content comes before the indent
                //     from the left, even though the line's inline length counts the indent first.
                snapshot.inline_offset - self.line(line_index).indent
            } else {
                snapshot.inline_offset
            };
            let new_inline_offset = inline_offset + fragment_inline_offset;
            let own_alignment_is_line_relative = line_relative_alignment(style).is_some();
            let aligned_subtree = has_line_relative_aligned_subtree
                .then(|| self.line_relative_aligned_subtree_root(snapshot.style_source))
                .flatten();
            let alignment_style = if aligned_subtree.is_some_and(|(root, _)| root == snapshot.style_source) {
                style.with_vertical_align_keyword(vertical_align::BASELINE)
            } else {
                self.alignment_style(snapshot.style_source, style)
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
            if snapshot.style_source != containing_block && self.facts(snapshot.style_source).is_fragmented_inline() {
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
            // A line-relative child moves independently of its ancestors, but their own baselines still need
            // their vertical-align offsets for inline geometry and line bounds.
            let mut enclosing_vertical_shift = CssPixels::default();
            while (!own_alignment_is_line_relative || snapshot.is_atomic_inline)
                && !ancestor.is_invalid()
                && self.facts(ancestor).is_fragmented_inline()
                && ancestor != containing_block
            {
                if line_relative_alignment(self.style(ancestor)).is_some() {
                    inline_box_alignments.push(InlineBoxAlignment {
                        box_: ancestor,
                        vertical_shift: CssPixels::default(),
                    });
                    break;
                }
                let ancestor_vertical_shift = self.vertical_shift_of_inline_box(ancestor);
                enclosing_vertical_shift += ancestor_vertical_shift;
                if !own_alignment_is_line_relative {
                    new_block_offset += ancestor_vertical_shift;
                }
                inline_box_alignments.push(InlineBoxAlignment {
                    box_: ancestor,
                    vertical_shift: ancestor_vertical_shift,
                });
                ancestor = self.context().parent_node(ancestor);
            }
            // NB: The inline boxes around the root of an aligned subtree are aligned like any other, and hold their own
            //     layout bounds around their baselines. The walk above stops at that root, or skips them altogether
            //     for a root that isn't atomic, and a box it already recorded isn't recorded again.
            if let Some((root, _)) = aligned_subtree {
                let enclosing_shifts = self.vertical_shifts_of_enclosing_inline_boxes(root);
                let mut ancestor_shift = enclosing_shifts
                    .iter()
                    .fold(CssPixels::default(), |total, (_, shift)| total + *shift);
                for (ancestor, shift) in enclosing_shifts {
                    let ancestor_baseline = current_block_offset + line_box_baseline + ancestor_shift;
                    let inserted =
                        self.line_mut(line_index)
                            .set_inline_box_baseline(ancestor, ancestor_baseline, ancestor_shift);
                    if inserted && self.inline_box_has_strut_on_line(&text, ancestor) {
                        let bounds = TextLayoutBounds::new(self.style(ancestor), None);
                        let (box_start, box_end) =
                            (ancestor_baseline - bounds.ascent, ancestor_baseline + bounds.descent);
                        // NB: An inline box inside an aligned subtree of its own extends that subtree.
                        extend_aligned_subtree_or_line_box(
                            &mut aligned_subtrees,
                            (&mut earliest, &mut latest),
                            self.line_relative_aligned_subtree_root(ancestor),
                            (box_start, box_end),
                        );
                    }
                    ancestor_shift -= shift;
                }
            }
            let accumulated_vertical_shift = new_block_offset - baseline_aligned_block_offset;
            {
                let fragment = &mut self.line_mut(line_index).fragments[fragment_index];
                fragment.inline_offset = new_inline_offset;
                fragment.block_offset = new_block_offset + block_offset;
                fragment.accumulated_vertical_shift = accumulated_vertical_shift;
                snapshot.block_offset = fragment.block_offset;
            }
            let (inline_box_start, inline_box_end) = if snapshot.is_atomic_inline {
                let atomic_inline = self.line(line_index).fragments[fragment_index].box_cells();
                (
                    snapshot.block_offset - (atomic_inline.margin_top + atomic_inline.border_box_top(false)),
                    snapshot.block_offset
                        + atomic_inline.content_block_size
                        + atomic_inline.margin_bottom
                        + atomic_inline.border_box_bottom(false),
                )
            } else {
                let bounds = self.text_fragment_layout_bounds(line_index, fragment_index, style);
                (
                    snapshot.block_offset + snapshot.baseline - bounds.ascent,
                    snapshot.block_offset + snapshot.baseline + bounds.descent,
                )
            };
            if self.fragment_extends_line_box(line_index, &text, fragment_index) {
                extend_aligned_subtree_or_line_box(
                    &mut aligned_subtrees,
                    (&mut earliest, &mut latest),
                    aligned_subtree,
                    (inline_box_start, inline_box_end),
                );
            }

            let is_text_node = self.line(line_index).fragments[fragment_index].is_block_ellipsis
                || self.line(line_index).fragments[fragment_index].is_text_overflow_ellipsis
                || self.facts(snapshot.layout_node).is_text_node()
                || self.facts(snapshot.layout_node).is_fragmented_inline();
            if is_text_node {
                let font_box_size = content_area_block_size(style);
                let font_baseline = Self::baseline_for_style(style, font_box_size);
                let fragment_mut = &mut self.line_mut(line_index).fragments[fragment_index];
                fragment_mut.record.block_offset += fragment_mut.baseline - font_baseline;
                fragment_mut.baseline = font_baseline;
                fragment_mut.block_length = font_box_size;
            }

            let text_fragment_baseline = {
                let fragment = &self.line(line_index).fragments[fragment_index];
                (is_text_node && fragment.takes_part_in_line_alignment())
                    .then(|| fragment.block_offset + fragment.baseline)
            };
            if let Some(fragment_baseline) = text_fragment_baseline {
                // Aligned-subtree fragments are placed at their baseline position here and moved by the
                // subtree shift below, so they never count as unshifted.
                let fragment_is_unshifted =
                    aligned_subtree.is_none() && accumulated_vertical_shift == CssPixels::default();
                if first_unshifted_text_baseline.is_none()
                    && fragment_is_unshifted
                    && fragment_index < strut_fragment_start
                {
                    first_unshifted_text_baseline = Some(fragment_baseline - current_block_offset);
                }
            }

            let inline_box_layout_baseline = text_fragment_baseline.or_else(|| {
                snapshot
                    .is_atomic_inline
                    .then_some(current_block_offset + line_box_baseline + enclosing_vertical_shift)
            });
            if let Some(mut inline_box_baseline) = inline_box_layout_baseline {
                // https://drafts.csswg.org/css-text-decor-4/#text-line-constancy
                // UAs must adjust line positions to match the shifted metrics of decorating boxes shifted with
                // vertical-align values other than baseline [CSS2] or subscripted/superscripted via
                // font-variant-position [CSS-FONTS-3], but must not adjust the line position or thickness in
                // response to descendants of a decorating box that are so styled.
                let mut remaining_vertical_shift = accumulated_vertical_shift;
                if snapshot.is_atomic_inline {
                    remaining_vertical_shift = enclosing_vertical_shift;
                }
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
                    // https://drafts.csswg.org/css2/#line-height
                    // The height of each inline-level box in the line box is calculated. For replaced elements,
                    // inline-block elements, and inline-table elements, this is the height of their margin box; for
                    // inline boxes, this is their 'line-height'.
                    // NB: An inline box without direct text contributes its layout bounds around its own baseline,
                    //     independently of the size and vertical alignment of its descendants.
                    if self.inline_box_has_strut_on_line(&text, alignment.box_) {
                        let bounds = TextLayoutBounds::new(self.style(alignment.box_), None);
                        let (box_start, box_end) = (
                            inline_box_baseline - bounds.ascent,
                            inline_box_baseline + bounds.descent,
                        );
                        extend_aligned_subtree_or_line_box(
                            &mut aligned_subtrees,
                            (&mut earliest, &mut latest),
                            self.line_relative_aligned_subtree_root(alignment.box_),
                            (box_start, box_end),
                        );
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
        if earliest > latest {
            earliest = self.current_block_offset;
            latest = self.current_block_offset;
        }
        latest = latest.max(earliest + maximum_block_size(vertical_align::TOP));
        earliest = earliest.min(latest - maximum_block_size(vertical_align::BOTTOM));
        LineAlignment {
            earliest,
            latest,
            line_box_baseline,
            has_line_relative_aligned_subtree,
            aligned_subtrees,
            first_unshifted_text_baseline,
        }
    }

    pub(crate) fn remove_last_line_if_empty(&mut self) {
        while self.line_count() != 0 && self.line(self.line_count() - 1).is_empty() {
            self.context().line_data_mut().line_boxes.pop();
            self.last_line_needs_update = false;
        }
    }

    pub(crate) fn recalculate_available_space(&mut self) {
        let current_line_block_size = self
            .max_block_size_on_current_line
            .max(self.containing_style().line_height())
            .max(self.float_fit_height());
        self.available_inline_size_for_current_line = self
            .context()
            .available_space_for_line(self.current_block_offset, current_line_block_size);
        if self.line_count() != 0 {
            let line_index = self.line_count() - 1;
            self.line_mut(line_index).original_available_inline_size = self.available_inline_size_for_current_line;
        }
    }

    pub(crate) fn did_introduce_clearance(&mut self, clearance: CssPixels) {
        // NB: The line the clearing break ends reaches down to the clearance, so that its block container does even
        //     when no line follows. That's the last line before the current one that holds anything: Moving past a
        //     float that left no room beside the break can leave empty lines between them. The line grows rather
        //     than moves, so it keeps its start and its baseline.
        let current = self.line_count() - 1;
        if let Some(ended) = (0..current).rev().find(|&index| !self.line(index).is_empty()) {
            let mut line = self.line_mut(ended);
            let block_end = line.block_end;
            if clearance > block_end {
                line.block_length += clearance - block_end;
                line.clearance_block_length += clearance - block_end;
                line.block_end = clearance;
            }
        }
        self.current_block_offset = self.current_block_offset.max(clearance);
    }

    /// Gives the whitespace that a line break before it dropped from the start of the current line to the end of the
    /// previous line, where it follows on from the same text.
    pub(crate) fn set_trailing_whitespace_on_previous_line(&mut self, whitespace: &inline_level_iterator::Item) {
        if self.line_count() < 2 {
            return;
        }
        let previous = self.line_count() - 2;
        if let Some(fragment) = self.line_mut(previous).fragments.last_mut()
            && fragment.layout_node == whitespace.node
            && fragment.start
                + fragment.length_in_code_units
                + fragment.trimmed_trailing_whitespace.length_in_code_units
                == whitespace.offset_in_node
        {
            fragment.record.trimmed_trailing_whitespace += line_box_fragment::TrailingWhitespace {
                length_in_code_units: whitespace.length_in_node,
                inline_size: whitespace.inline_size,
                ascii_inline_size: whitespace.inline_size,
            };
        }
    }

    pub(crate) fn current_block_offset(&self) -> CssPixels {
        self.current_block_offset
    }

    pub(crate) fn set_hanging_whitespace_inline_size(&mut self, inline_size: CssPixels) {
        self.hanging_whitespace_inline_size = inline_size;
    }

    pub(crate) fn set_end_edges_of_whitespace_at_break(&mut self, inline_size: CssPixels) {
        self.end_edges_of_whitespace_at_break = inline_size;
    }

    pub(crate) fn set_unbreakable_run_inline_size_interrupted_by_float(&mut self, inline_size: CssPixels) {
        self.unbreakable_run_inline_size_interrupted_by_float = inline_size;
    }
}

// https://drafts.csswg.org/css2/#propdef-vertical-align
// https://drafts.csswg.org/css-text-3/#text-align-last-property
// This property describes how the last line of a block or a line right before a forced line break is aligned.
/// The text-align a line takes: the block's text-align-last for its last line or a line before a forced break, and its
/// text-align for any other.
pub(crate) fn line_text_align(style: StyleValues, uses_last_line_alignment: bool) -> u8 {
    if !uses_last_line_alignment {
        return style.text_align();
    }
    match style.text_align_last() {
        // If auto is specified, content on the affected line is aligned per text-align-all unless text-align-all is set
        // to justify, in which case it is start-aligned.
        text_align_last::AUTO => match style.text_align() {
            text_align::JUSTIFY => text_align::START,
            text_align => text_align,
        },
        // All other values are interpreted as described for text-align.
        text_align_last::START => text_align::START,
        text_align_last::END => text_align::END,
        text_align_last::LEFT => text_align::LEFT,
        text_align_last::RIGHT => text_align::RIGHT,
        text_align_last::CENTER => text_align::CENTER,
        text_align_last::JUSTIFY => text_align::JUSTIFY,
        _ => unreachable!("text-align-last computes to one of its keywords"),
    }
}

/// Whether any line of a block with this style may be justified, which keeps its whitespace in fragments of its own.
pub(crate) fn may_justify_lines(style: StyleValues) -> bool {
    style.text_align() == text_align::JUSTIFY || style.text_align_last() == text_align_last::JUSTIFY
}

fn line_relative_alignment(style: StyleValues) -> Option<u8> {
    let keyword = style
        .vertical_align_is_keyword()
        .then(|| style.vertical_align_keyword())?;
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

// https://drafts.csswg.org/css-inline-3/#inline-height
/// The extent of the layout bounds of a text fragment above and below its baseline.
#[derive(Clone, Copy)]
pub(crate) struct TextLayoutBounds {
    pub(crate) ascent: CssPixels,
    pub(crate) descent: CssPixels,
}

impl TextLayoutBounds {
    /// The bounds of text in a font with `ascent` and `descent`, with half the line height's leading on each side.
    /// A font's ascent and descent count in whole pixels, as they do for 'line-height: normal'.
    fn with_leading(ascent: f32, descent: f32, line_height: CssPixels) -> Self {
        let ascent = font_metric_in_whole_pixels(ascent);
        let descent = font_metric_in_whole_pixels(descent);
        let leading = line_height - (ascent + descent);
        let leading_above = leading / 2;
        Self {
            ascent: ascent + leading_above,
            descent: descent + (leading - leading_above),
        }
    }

    pub(crate) fn new(style: StyleValues, font: Option<&libgfx_rust::font::FontHandle>) -> Self {
        let mut bounds = Self::with_leading(style.font_ascent(), style.font_descent(), style.line_height());
        // When its computed line-height is normal, the layout bounds of an inline box encloses all its glyphs, going
        // from the highest A to the deepest D. (Note that glyphs in a single box can come from different fonts and
        // thus might not all have the same A and D.)
        // NB: The bounds above already enclose the glyphs of the first available font. Each other font brings in the
        //     leading of its own normal line height, which includes its line gap.
        if let Some(font) = font
            && style.line_height_is_normal()
            && style
                .font()
                .first_available_font()
                .is_none_or(|first_available| font.as_raw() != first_available.as_raw())
        {
            let facts = font.facts();
            let normal_line_height = font_metric_in_whole_pixels(facts.ascent)
                + font_metric_in_whole_pixels(facts.descent)
                + font_metric_in_whole_pixels(facts.line_gap);
            let font_bounds = Self::with_leading(facts.ascent, facts.descent, normal_line_height);
            bounds.ascent = bounds.ascent.max(font_bounds.ascent);
            bounds.descent = bounds.descent.max(font_bounds.descent);
        }
        bounds
    }

    pub(crate) fn block_size(self) -> CssPixels {
        self.ascent + self.descent
    }
}

pub(crate) fn font_metric_in_whole_pixels(metric: f32) -> CssPixels {
    CssPixels::from_integer(metric.round_ties_even() as i64)
}

/// The block size of the content area of an inline box: its font's ascent plus descent. Unlike a normal line-height,
/// it leaves out the font's line gap.
pub(crate) fn content_area_block_size(style: StyleValues) -> CssPixels {
    font_metric_in_whole_pixels(style.font_ascent()) + font_metric_in_whole_pixels(style.font_descent())
}
