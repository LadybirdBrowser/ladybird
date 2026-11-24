/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

// NB: Keep the addition order shared with intrinsic sizing: CssPixels arithmetic saturates.
pub(crate) fn inline_advance(
    leading_margin: CssPixels,
    leading_size: CssPixels,
    content_inline_size: CssPixels,
    trailing_size: CssPixels,
    trailing_margin: CssPixels,
) -> CssPixels {
    leading_margin + leading_size + content_inline_size + trailing_size + trailing_margin
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct StaticPositionMarker {
    pub(crate) box_: Node,
    pub(crate) inline_offset: CssPixels,
    pub(crate) block_offset: CssPixels,
    pub(crate) writing_mode: u8,
    pub(crate) preceded_by_in_flow_content: bool,
    /// How many fragments the line held when the marker was added, which places it in the line's logical order.
    pub(crate) fragments_before: usize,
    /// The visual inline coordinate relative to the preceding fragment, when both neighbors share a bidi run.
    pub(super) inline_offset_from_bidi_fragment: Option<CssPixels>,
}

impl StaticPositionMarker {
    pub(crate) fn offset(self) -> (CssPixels, CssPixels) {
        geometry::to_physical(self.writing_mode, self.inline_offset, self.block_offset)
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct InlineBoxBaseline {
    pub(crate) box_: Node,
    pub(crate) baseline: CssPixels,
    pub(crate) accumulated_vertical_shift: CssPixels,
}

/// An inline box on a line, and where on the line its content begins. No fragment on the line belongs to a box that's
/// empty, or to one whose content there is only whitespace that collapsed away, the forced break that ends the line,
/// or a block-level box the line stops at.
#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct InlineBoxStrut {
    pub(crate) box_: Node,
    pub(crate) inline_offset: CssPixels,
}

/// What text-overflow elided from the end of a line.
#[derive(Clone, Copy, PartialEq)]
pub(crate) struct TextOverflowElision {
    /// The inline length of the line before the ellipsis.
    pub(crate) inline_length: CssPixels,
    pub(crate) origin: TextOverflowOrigin,
    /// The inline length of the fragment that ends in the ellipsis before it was shortened.
    pub(crate) ellipsis_fragment_inline_length: CssPixels,
    /// How much of its inline length that fragment lost at its left end, where the ellipsis is on the left. And there,
    /// the fragments are still where the content put them, rather than where the line box starts.
    pub(crate) ellipsis_fragment_inline_length_lost_on_left: Option<CssPixels>,
}

#[derive(Clone, Copy, PartialEq)]
pub(crate) enum TextOverflowOrigin {
    /// The line box's inline start when ellipsing took place after placement.
    LineBoxStart(CssPixels),
    /// The cut fragment's line-relative offset before placement.
    CutFragmentStart(CssPixels),
}

/// What a line its block container interrupted for a block-level box keeps of the box, as the layout of the box left it.
#[derive(Clone, Copy, PartialEq)]
pub(crate) struct BlockLevelBoxOnLine {
    /// The offset of the content box of the box in its containing block.
    pub(crate) content_offset: FfiCssPixelPoint,
    /// The sizes and box sides of the box.
    pub(crate) cells: used_values::RootInputCells,
    /// The baselines the box gives the line.
    pub(crate) baselines: formatting_context::LineBaselines,
}

struct BidiFragmentPlacement {
    key: i64,
    tie: usize,
    index: usize,
    left: CssPixels,
    advance: CssPixels,
}

#[derive(Clone, PartialEq)]
pub(crate) struct LineBoxData {
    pub(crate) start: super::fragmentation::LinePosition,
    pub(crate) fragments: Vec<line_box_fragment::LineBoxFragmentData>,
    pub(crate) static_position_markers: Vec<StaticPositionMarker>,
    pub(crate) inline_box_baselines: Vec<InlineBoxBaseline>,
    /// The inline boxes that are on the line whether or not a fragment on it belongs to them. One that no fragment
    /// belongs to takes part in the height and alignment of the line box by its strut (a box as wide as nothing, with
    /// its font and line height) — but only where the line holds content or ends in a forced break.
    /// https://drafts.csswg.org/css-inline-3/#invisible-line-boxes
    pub(crate) inline_box_struts: Vec<InlineBoxStrut>,
    pub(crate) text_overflow_elision: Option<TextOverflowElision>,
    /// The block-level box the line holds, where the line is interrupted for one.
    pub(crate) block_level_box: Option<BlockLevelBoxOnLine>,
    /// Whether placing the line aligned it as the last line of its block, or a line before a forced break, by
    /// text-align-last. Justification follows the same decision.
    pub(crate) uses_last_line_alignment: Option<bool>,
    /// Where the line box starts, past any floats, once the line is placed.
    pub(crate) placed_inline_start: CssPixels,
    /// Preserved space separators at the visual line end that do not count for alignment or justification.
    pub(super) hanging_space_inline_size: CssPixels,
    pub(super) bidi_reordered: bool,
    pub(crate) record: inline_content::LineRecord,
}

impl std::ops::Deref for LineBoxData {
    type Target = inline_content::LineRecord;
    fn deref(&self) -> &Self::Target {
        &self.record
    }
}

impl std::ops::DerefMut for LineBoxData {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.record
    }
}

impl LineBoxData {
    pub(crate) fn note_text_overflow_elision(
        &mut self,
        cut_fragment_start: CssPixels,
        original_fragment_inline_length: CssPixels,
        fragment_inline_length_lost_on_left: Option<CssPixels>,
    ) {
        let origin = if self.uses_last_line_alignment.is_some() {
            TextOverflowOrigin::LineBoxStart(self.placed_inline_start)
        } else {
            TextOverflowOrigin::CutFragmentStart(cut_fragment_start)
        };
        self.text_overflow_elision = Some(TextOverflowElision {
            inline_length: self.inline_length,
            origin,
            ellipsis_fragment_inline_length: original_fragment_inline_length,
            ellipsis_fragment_inline_length_lost_on_left: fragment_inline_length_lost_on_left,
        });
        self.hanging_space_inline_size = CssPixels::default();
    }

    pub(crate) fn new(direction: u8, writing_mode: u8, start: super::fragmentation::LinePosition) -> Self {
        Self {
            start,
            fragments: Vec::new(),
            static_position_markers: Vec::new(),
            inline_box_baselines: Vec::new(),
            inline_box_struts: Vec::new(),
            text_overflow_elision: None,
            block_level_box: None,
            uses_last_line_alignment: None,
            placed_inline_start: CssPixels::default(),
            hanging_space_inline_size: CssPixels::default(),
            bidi_reordered: false,
            record: inline_content::LineRecord {
                direction,
                writing_mode,
                is_empty: true,
                ..Default::default()
            },
        }
    }

    pub(crate) fn retained_metrics(&self) -> inline_content::LineRecord {
        inline_content::LineRecord {
            is_empty: self.is_empty(),
            horizontal_extent: inline_formatting_context::line_physical_horizontal_extent(self),
            ..self.record
        }
    }

    pub(crate) fn push_fragment(&mut self, fragment: line_box_fragment::LineBoxFragmentData) {
        // Many lines contain a single fragment, so avoid Vec's initial capacity of four large fragments.
        if self.fragments.capacity() == 0 {
            self.fragments.reserve_exact(1);
        }
        self.fragments.push(fragment);
    }

    #[allow(clippy::too_many_arguments)]
    pub(super) fn add_fragment(
        &mut self,
        layout_node: Node,
        start: usize,
        length: usize,
        leading_size: CssPixels,
        trailing_size: CssPixels,
        leading_margin: CssPixels,
        trailing_margin: CssPixels,
        content_inline_size: CssPixels,
        content_block_size: CssPixels,
        border_box_block_start: CssPixels,
        border_box_block_end: CssPixels,
        glyphs: Option<line_box_fragment::ItemGlyphRun<'_>>,
        facts: line_box_fragment::FragmentBuildFacts,
        text_align_is_justify: bool,
        trailing_whitespace: line_box_fragment::TrailingWhitespace,
        bidi: Option<(bidi::Position, &bidi::Paragraph)>,
        bidi_edges_on_left: CssPixels,
        bidi_inline_box_edges: Option<Box<bidi::InlineBoxEdges>>,
    ) {
        let bidi_position = bidi.map(|(position, _)| position);
        if let Some((_, paragraph)) = bidi {
            self.record.direction = paragraph.alignment_direction;
        }
        let can_merge = glyphs.as_ref().is_some_and(|glyphs| {
            !text_align_is_justify
                && self.fragments.last().is_some_and(|last| {
                    !last.has_soft_wrap_opportunity_after
                        && last.layout_node == layout_node
                        && last.glyphs.as_ref().is_some_and(|last_glyphs| {
                            last_glyphs.font == *glyphs.font && last_glyphs.is_upright == glyphs.is_upright
                        })
                        && last.start + last.length_in_code_units == start
                        && match (last.bidi_position, bidi) {
                            (None, None) => true,
                            (Some(previous), Some((position, paragraph))) => {
                                previous.paragraph == position.paragraph
                                    && previous.offset + last.length_in_code_units == position.offset
                                    && paragraph.shaping_level_at(previous.offset)
                                        == paragraph.shaping_level_at(position.offset)
                            }
                            _ => false,
                        }
                })
        });
        if can_merge {
            let last = self.fragments.last_mut().unwrap();
            last.length_in_code_units += length;
            last.append_glyph_run(&glyphs.unwrap(), content_inline_size, start - last.start);
            last.bidi_edges.1 += trailing_size + trailing_margin;
            last.bidi_edges_on_left += bidi_edges_on_left;
            if let Some(mut edges) = bidi_inline_box_edges {
                if let Some(last_edges) = &mut last.bidi_inline_box_edges {
                    last_edges.leading.append(&mut edges.leading);
                    last_edges.trailing.append(&mut edges.trailing);
                } else {
                    last.bidi_inline_box_edges = Some(edges);
                }
            }
            if trailing_whitespace.length_in_code_units == length {
                last.trailing_whitespace += trailing_whitespace;
            } else {
                last.trailing_whitespace = trailing_whitespace;
            }
        } else {
            let inline_offset = leading_margin + leading_size + self.record.inline_length;
            let mut fragment = line_box_fragment::LineBoxFragmentData::new(
                layout_node,
                start,
                length,
                inline_offset,
                CssPixels::default(),
                content_inline_size,
                content_block_size,
                border_box_block_start,
                self.direction,
                self.writing_mode,
                glyphs.map(line_box_fragment::ItemGlyphRun::into_glyph_data),
                facts,
            );
            fragment.trailing_whitespace = trailing_whitespace;
            fragment.bidi_position = bidi_position;
            fragment.bidi_edges = (leading_margin + leading_size, trailing_size + trailing_margin);
            fragment.bidi_edges_on_left = bidi_edges_on_left;
            fragment.bidi_inline_box_edges = bidi_inline_box_edges;
            self.push_fragment(fragment);
        }
        self.record.inline_length += inline_advance(
            leading_margin,
            leading_size,
            content_inline_size,
            trailing_size,
            trailing_margin,
        );
        self.block_length = self
            .block_length
            .max(content_block_size + border_box_block_start + border_box_block_end);
    }

    pub(super) fn reorder_bidi_fragments(&mut self, context: &inline_formatting_context::InlineFormattingContext<'_>) {
        if self.bidi_reordered {
            return;
        }
        let Some(first) = self.visible_fragments().find_map(|fragment| fragment.bidi_position) else {
            return;
        };
        let paragraph = &context.bidi_paragraphs[first.paragraph];
        let is_unidirectional_ltr = paragraph.is_unidirectional_ltr;
        if is_unidirectional_ltr
            && !self
                .visible_fragments()
                .any(|frag| frag.bidi_inline_box_edges.is_some())
        {
            return;
        }
        let end = self
            .visible_fragments()
            .filter_map(|fragment| {
                let position = fragment.bidi_position?;
                debug_assert_eq!(position.paragraph, first.paragraph);
                Some(position.offset + fragment.length_in_code_units.max(1))
            })
            .max()
            .unwrap();
        let (indices, trailing_whitespace_start) = paragraph.visual_indices(first.offset..end);
        for fragment in &mut self.fragments {
            if fragment.length_in_code_units != 0
                && !fragment.is_fully_truncated
                && fragment
                    .bidi_position
                    .is_some_and(|position| position.offset >= trailing_whitespace_start)
            {
                fragment.set_bidi_whitespace_direction(bidi::direction_of_level(paragraph.base_level));
            }
        }
        let visual_index = |position: bidi::Position| i64::from(indices[position.offset - first.offset]) * 2;
        let level_at = |position: bidi::Position| {
            if position.offset >= trailing_whitespace_start {
                paragraph.base_level
            } else {
                bidi::resolved_level(paragraph.level_at(position.offset))
            }
        };
        let is_rtl = |position| bidi::direction_of_level(level_at(position)) == direction::RTL;

        // Keep the fragments in logical order for DOM ranges and inline box ownership. Only their positions go into
        // visual order. Offsets in the paragraph include the zero-width controls between inline boxes.
        let mut logical_end = self.indent;
        let mut anchor = None;
        let mut order: Vec<_> = self
            .fragments
            .iter()
            .enumerate()
            .filter(|(_, fragment)| !fragment.is_fully_truncated)
            .map(|(index, fragment)| {
                let position = fragment.bidi_position;
                let (key, reversed) = if fragment.is_block_ellipsis {
                    if self.direction == direction::RTL {
                        (i64::MIN, true)
                    } else {
                        (i64::MAX, false)
                    }
                } else if let Some(position) = position {
                    anchor = Some(position);
                    (visual_index(position), is_rtl(position))
                } else if let Some(previous) = anchor {
                    let reversed = is_rtl(previous);
                    (visual_index(previous) + if reversed { -1 } else { 1 }, reversed)
                } else {
                    let reversed = is_rtl(first);
                    (visual_index(first) + if reversed { 1 } else { -1 }, reversed)
                };
                let before = fragment.inline_offset - logical_end;
                let after = fragment.bidi_edges.1;
                logical_end = fragment.inline_offset + fragment.inline_length + after;
                let advance = before + fragment.inline_length + after;
                let gap = before - fragment.bidi_edges.0;
                let left = if is_unidirectional_ltr {
                    before
                } else {
                    fragment.bidi_edges_on_left + if reversed { CssPixels::default() } else { gap }
                };
                BidiFragmentPlacement {
                    key,
                    tie: if reversed { usize::MAX - index } else { index },
                    index,
                    left,
                    advance,
                }
            })
            .collect();
        order.sort_unstable_by_key(|placement| (placement.key, placement.tie));
        let relocated_edges = self.place_bidi_inline_box_edges(context, &mut order, is_unidirectional_ltr);
        if is_unidirectional_ltr && relocated_edges.is_none() {
            return;
        }
        for marker in &mut self.static_position_markers {
            let Some([previous, next]) = marker
                .fragments_before
                .checked_sub(1)
                .and_then(|previous| self.fragments.get(previous..=marker.fragments_before))
            else {
                continue;
            };
            let (Some(previous_position), Some(next_position)) = (previous.bidi_position, next.bidi_position) else {
                continue;
            };
            if previous.is_fully_truncated
                || next.is_fully_truncated
                || previous_position.offset + previous.length_in_code_units.max(1) != next_position.offset
                || level_at(previous_position) != level_at(next_position)
            {
                continue;
            }
            // A marker inside a run follows its logical neighbor. Mirror the distance from that neighbor's
            // end when bidi placement reverses the run, including any inline box edges between them.
            marker.inline_offset_from_bidi_fragment = Some(if is_rtl(previous_position) {
                previous.inline_offset + previous.inline_length - marker.inline_offset
            } else {
                marker.inline_offset - previous.inline_offset
            });
        }
        let mut offset = self.indent;
        if self.direction == direction::RTL {
            // Edges of collapsed whitespace can advance the line without emitting a fragment.
            offset += (self.inline_length - logical_end).max(CssPixels::default());
        }
        for (visual_index, placement) in order.into_iter().enumerate() {
            self.fragments[placement.index].inline_offset = offset + placement.left;
            self.fragments[placement.index].bidi_visual_index =
                u32::try_from(visual_index).expect("fragment indices fit in 32 bits");
            offset += placement.advance;
        }
        for (empty_index, target_index, is_left) in relocated_edges.unwrap_or_default() {
            let target = &self.fragments[target_index];
            let offset = if is_left {
                target.inline_offset
            } else {
                target.inline_offset + target.inline_length
            };
            let visual_index = target.bidi_visual_index;
            self.fragments[empty_index].inline_offset = offset;
            self.fragments[empty_index].bidi_visual_index = visual_index;
        }
        self.bidi_reordered = true;
    }

    fn place_bidi_inline_box_edges(
        &self,
        context: &inline_formatting_context::InlineFormattingContext<'_>,
        order: &mut [BidiFragmentPlacement],
        is_unidirectional_ltr: bool,
    ) -> Option<Vec<(usize, usize, bool)>> {
        let edges_of = |index: usize| {
            self.fragments[index]
                .bidi_inline_box_edges
                .iter()
                .flat_map(|edges| edges.iter())
        };
        let mut bounds = HashMap::<Node, Option<(usize, usize)>>::default();
        for placement in order.iter() {
            for (edge, _) in edges_of(placement.index) {
                bounds.entry(edge.owner).or_insert(None);
            }
        }
        if bounds.is_empty() {
            return None;
        }
        // https://drafts.csswg.org/css-writing-modes-4/#bidi-box-model
        // Resolve the box's line-left and line-right sides from its contents in visual order. The items that
        // carried its advances for line fitting can be on different sides, or even outside the box.
        for (position, placement) in order.iter().enumerate() {
            let fragment = &self.fragments[placement.index];
            if fragment.bidi_position.is_none() {
                continue;
            }
            let mut owner = context.inline_box_for_fragment(fragment.layout_node);
            while !owner.is_invalid() {
                if let Some(bounds) = bounds.get_mut(&owner) {
                    bounds.get_or_insert((position, position)).1 = position;
                }
                owner = context.nearest_fragmented_inline_ancestor(owner);
            }
        }

        let mut moved_empty_edges = None::<Vec<_>>;
        for source in 0..order.len() {
            let source_index = order[source].index;
            let fragment = &self.fragments[source_index];
            for (edge, was_leading) in edges_of(source_index) {
                let Some((left, right)) = bounds[&edge.owner] else {
                    continue;
                };
                let target = if edge.is_left { left } else { right };
                let was_left = if is_unidirectional_ltr {
                    was_leading
                } else {
                    edge.is_left
                };
                if source == target && was_left == edge.is_left {
                    continue;
                }
                let empty_edges = moved_empty_edges.get_or_insert_with(Vec::new);
                if source != target {
                    order[source].advance -= edge.advance;
                    order[target].advance += edge.advance;
                }
                if was_left {
                    order[source].left -= edge.advance;
                }
                if edge.is_left {
                    order[target].left += edge.advance;
                }
                if fragment.bidi_position.is_none() && fragment.layout_node == edge.owner {
                    // An edge without a text or atomic fragment of its own still contributes to its box's rect.
                    empty_edges.push((source_index, order[target].index, edge.is_left));
                }
            }
        }
        moved_empty_edges
    }

    pub(super) fn fragment_indices_in_visual_order(&self) -> Vec<usize> {
        let mut indices: Vec<_> = self
            .fragments
            .iter()
            .enumerate()
            .filter_map(|(index, fragment)| (!fragment.is_fully_truncated).then_some(index))
            .collect();
        if self.bidi_reordered {
            // Negative margins can put a later fragment at a smaller coordinate. Empty edges that were moved to
            // a fragment's boundary share its visual index and are ordered by position within that group.
            indices.sort_by_key(|&index| {
                let fragment = &self.fragments[index];
                (fragment.bidi_visual_index, fragment.inline_offset)
            });
        }
        indices
    }

    pub(crate) fn advance_inline_length(&mut self, inline_size: CssPixels) {
        self.record.inline_length += inline_size;
    }

    pub(crate) fn add_static_position_marker(&mut self, box_: Node, preceded_by_inline_box_start_edges: bool) {
        self.static_position_markers.push(StaticPositionMarker {
            box_,
            inline_offset: self.record.inline_length,
            block_offset: CssPixels::default(),
            writing_mode: self.writing_mode,
            preceded_by_in_flow_content: !self.fragments.is_empty() || preceded_by_inline_box_start_edges,
            fragments_before: self.fragments.len(),
            inline_offset_from_bidi_fragment: None,
        });
    }

    pub(crate) fn set_inline_box_baseline(
        &mut self,
        box_: Node,
        baseline: CssPixels,
        accumulated_vertical_shift: CssPixels,
    ) -> bool {
        if self.inline_box_baselines.iter().any(|entry| entry.box_ == box_) {
            return false;
        }
        self.inline_box_baselines.push(InlineBoxBaseline {
            box_,
            baseline,
            accumulated_vertical_shift,
        });
        true
    }

    pub(crate) fn inline_box_baseline(&self, box_: Node) -> Option<InlineBoxBaseline> {
        self.inline_box_baselines
            .iter()
            .find(|entry| entry.box_ == box_)
            .copied()
    }

    /// Puts `box_` on the line, with its content beginning `leading_edges` past the content the line holds so far.
    pub(crate) fn add_inline_box_strut(&mut self, box_: Node, leading_edges: CssPixels) {
        if self.inline_box_struts.iter().any(|strut| strut.box_ == box_) {
            return;
        }
        self.inline_box_struts.push(InlineBoxStrut {
            box_,
            inline_offset: self.record.inline_length + leading_edges,
        });
    }

    /// Whether the struts of the inline boxes on the line count, which they don't on a line with nothing else on it.
    pub(crate) fn takes_inline_box_struts(&self) -> bool {
        !self.has_block_level_box && (!self.fragments.is_empty() || self.has_forced_break)
    }

    fn inline_end_offset(&self) -> CssPixels {
        // Before placement, both the line and its markers are line-relative and this start offset is zero.
        self.placed_inline_start + self.record.inline_length
    }

    pub(crate) fn clamp_static_position_markers_to_inline_end(&mut self) {
        let inline_end = self.inline_end_offset();
        for marker in &mut self.static_position_markers {
            marker.inline_offset = marker.inline_offset.min(inline_end);
        }
    }

    fn remove_whitespace_advance(&mut self, fragment_index: usize, advance: CssPixels, removed_fragment: bool) {
        let changes_inline_size = advance != CssPixels::default();
        if !changes_inline_size && !removed_fragment {
            return;
        }
        self.record.inline_length -= advance;
        let inline_end = self.inline_end_offset();
        for marker in &mut self.static_position_markers {
            if marker.fragments_before > fragment_index {
                marker.inline_offset -= advance;
                marker.fragments_before -= usize::from(removed_fragment);
            }
            // Removing a zero-width fragment changes the fragment count without moving placed markers.
            if changes_inline_size {
                marker.inline_offset = marker.inline_offset.min(inline_end);
            }
        }
    }

    fn calculate_or_trim_trailing_whitespace(&mut self, should_remove: bool) -> CssPixels {
        let mut whitespace_inline_size = CssPixels::default();
        // The whitespace fragments removed so far that follow on from the same node.
        let mut removed_whitespace: Option<(Node, line_box_fragment::TrailingWhitespace)> = None;
        let mut fragment_index = self.fragments.len();
        // Record the whitespace trimmed from each retained fragment and those following it. Only whitespace trimmed
        // from before the fragment changes its position.
        let mut whitespace_at_or_after_retained_fragments = Vec::new();

        let last_fragment_index = loop {
            if fragment_index == 0 {
                break None;
            }
            fragment_index -= 1;
            let fragment = &self.fragments[fragment_index];
            if fragment.is_bidi_inline_edge() {
                if should_remove {
                    whitespace_at_or_after_retained_fragments.push(whitespace_inline_size);
                }
                continue;
            }
            if !matches!(
                fragment.white_space_collapse,
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            ) {
                break None;
            }
            // Trailing ideographic spaces keep their advance. Only the collapsible suffix is removed from the
            // fragment. Line fitting before a float can also exclude whitespace that would hang at the line end.
            let trailing_whitespace = if should_remove {
                fragment.collapsible_trailing_whitespace()
            } else {
                fragment.trailing_whitespace
            };
            let is_all_trailing_whitespace = fragment.length_in_code_units > 0
                && trailing_whitespace.length_in_code_units == fragment.length_in_code_units;
            let carries_bidi_edges = fragment.bidi_position.is_some() && fragment.bidi_edges != Default::default();
            if is_all_trailing_whitespace && (!fragment.is_justifiable_whitespace() || carries_bidi_edges) {
                whitespace_inline_size += trailing_whitespace.inline_size;
                if should_remove {
                    whitespace_at_or_after_retained_fragments.push(whitespace_inline_size);
                    let fragment = &mut self.fragments[fragment_index];
                    fragment.remove_trailing_whitespace(trailing_whitespace);
                    self.remove_whitespace_advance(fragment_index, trailing_whitespace.inline_size, false);
                }
                continue;
            }
            if !fragment.is_justifiable_whitespace() {
                break Some(fragment_index);
            }

            whitespace_inline_size += fragment.inline_length;
            if should_remove {
                let removed = line_box_fragment::TrailingWhitespace {
                    length_in_code_units: fragment.length_in_code_units,
                    inline_size: fragment.inline_length,
                    ascii_inline_size: fragment.inline_length,
                };
                removed_whitespace = match removed_whitespace {
                    Some((node, mut sum)) if node == fragment.layout_node => {
                        sum += removed;
                        Some((node, sum))
                    }
                    _ => Some((fragment.layout_node, removed)),
                };
                self.remove_whitespace_advance(fragment_index, removed.inline_size, true);
                self.fragments.remove(fragment_index);
            }
        };

        if let Some(last_fragment_index) = last_fragment_index
            && !self.fragments[last_fragment_index].text().is_empty()
        {
            // Trim the last text fragment's trailing whitespace by the advance recorded when it was shaped.
            let fragment = &self.fragments[last_fragment_index];
            let fragment_trailing_whitespace = if should_remove {
                fragment.collapsible_trailing_whitespace()
            } else {
                fragment.trailing_whitespace
            };
            whitespace_inline_size += fragment_trailing_whitespace.inline_size;
            if should_remove {
                let fragment = &mut self.fragments[last_fragment_index];
                fragment.remove_trailing_whitespace(fragment_trailing_whitespace);
                if let Some((node, removed)) = removed_whitespace
                    && node == fragment.layout_node
                {
                    fragment.record.trimmed_trailing_whitespace += removed;
                }
                self.remove_whitespace_advance(last_fragment_index, fragment_trailing_whitespace.inline_size, false);
            }
        }
        if should_remove {
            for (fragment, whitespace_at_or_after) in self
                .fragments
                .iter_mut()
                .rev()
                .zip(whitespace_at_or_after_retained_fragments)
            {
                fragment.inline_offset -= whitespace_inline_size - whitespace_at_or_after;
            }
        }
        whitespace_inline_size
    }

    pub(crate) fn trailing_whitespace_inline_size(&mut self) -> CssPixels {
        self.calculate_or_trim_trailing_whitespace(false)
    }

    pub(crate) fn trim_trailing_whitespace(&mut self) {
        let trimmed_inline_size = self.calculate_or_trim_trailing_whitespace(true);
        // An inline box after the trimmed whitespace is at the end of what the line keeps.
        if trimmed_inline_size != CssPixels::default()
            && let Some(last) = self.fragments.last()
        {
            let end = last.inline_offset + last.inline_length;
            for strut in &mut self.inline_box_struts {
                strut.inline_offset = strut.inline_offset.min(end);
            }
        }
    }

    pub(super) fn measure_hanging_spaces(&mut self) {
        self.hanging_space_inline_size = CssPixels::default();
        if !self.visible_fragments().any(|fragment| {
            matches!(
                fragment.white_space_collapse,
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            ) && fragment.trailing_whitespace.inline_size > fragment.trailing_whitespace.ascii_inline_size
        }) {
            return;
        }

        // https://drafts.csswg.org/css-text-3/#hanging
        // Measure the whitespace at the visual end of the line. A gap from inline box edges stops it from hanging.
        let ends_on_left = self.bidi_reordered && self.direction == direction::RTL;
        let mut edge = if ends_on_left { self.indent } else { self.inline_length };
        let mut indices = self.fragment_indices_in_visual_order();
        if !ends_on_left {
            indices.reverse();
        }
        for index in indices {
            let fragment = &self.fragments[index];
            let left = fragment.inline_offset;
            let right = left + fragment.inline_length;
            if edge != if ends_on_left { left } else { right } {
                break;
            }
            if fragment.is_atomic_inline {
                break;
            }
            if fragment.length_in_code_units == 0 {
                continue;
            }
            if !matches!(
                fragment.white_space_collapse,
                white_space_collapse::COLLAPSE | white_space_collapse::PRESERVE_BREAKS
            ) {
                break;
            }
            let whitespace = fragment.trailing_whitespace;
            let is_all_whitespace = whitespace.length_in_code_units == fragment.length_in_code_units;
            if !is_all_whitespace && fragment.current_insert_direction != self.direction {
                break;
            }
            self.hanging_space_inline_size += whitespace.inline_size;
            if !is_all_whitespace {
                break;
            }
            edge = if ends_on_left { right } else { left };
        }
    }

    pub(crate) fn trim_trailing_whitespace_before_block_ellipsis(&mut self) {
        while let Some(fragment) = self.fragments.last_mut() {
            let whitespace = std::mem::take(&mut fragment.trailing_whitespace);
            if whitespace.length_in_code_units == 0 {
                break;
            }
            self.record.inline_length -= whitespace.inline_size;
            if whitespace.length_in_code_units == fragment.length_in_code_units {
                self.fragments.pop();
                continue;
            }
            fragment.length_in_code_units -= whitespace.length_in_code_units;
            fragment.inline_length -= whitespace.inline_size;
            fragment.record.trimmed_trailing_whitespace += whitespace;
            break;
        }
        self.clamp_static_position_markers_to_inline_end();
    }
    pub(crate) fn is_empty_or_ends_in_whitespace(&self) -> bool {
        self.fragments
            .iter()
            .rfind(|fragment| !fragment.is_bidi_inline_edge())
            .is_none_or(line_box_fragment::LineBoxFragmentData::ends_in_whitespace)
    }

    pub(crate) fn visible_fragments(&self) -> impl DoubleEndedIterator<Item = &line_box_fragment::LineBoxFragmentData> {
        self.fragments.iter().filter(|fragment| !fragment.is_fully_truncated)
    }

    pub(crate) fn is_empty(&self) -> bool {
        self.fragments.is_empty() && !self.has_break
    }

    pub(crate) fn counts_toward_widows_and_orphans(&self) -> bool {
        // Empty records can have a soft break when advancing below a float, without representing a line of content.
        !self.fragments.is_empty() || self.has_forced_break
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ellipsing_a_placed_line_keeps_its_unelided_extent_and_drops_hanging_space() {
        let px = CssPixels::from_integer;
        let mut line = LineBoxData::new(direction::LTR, writing_mode::HORIZONTAL_TB, Default::default());
        line.uses_last_line_alignment = Some(true);
        line.placed_inline_start = px(30);
        line.inline_length = px(150);
        line.hanging_space_inline_size = px(20);

        let mut fragment = line_box_fragment::LineBoxFragmentData::new(
            Node::INVALID,
            0,
            0,
            px(30),
            px(0),
            px(50),
            px(20),
            px(0),
            line.direction,
            line.writing_mode,
            None,
            line_box_fragment::FragmentBuildFacts {
                style_source: Node::INVALID,
                is_atomic_inline: false,
                white_space_collapse: white_space_collapse::COLLAPSE,
                text_utf16: std::ptr::null(),
                text_length_in_code_units: 0,
            },
        );
        fragment.has_text_overflow_ellipsis = true;
        line.push_fragment(fragment);
        line.note_text_overflow_elision(px(30), px(130), None);
        line.inline_length = px(70);

        assert_eq!(line.hanging_space_inline_size, px(0));
        let overflow = inline_formatting_context::elided_content_rect(&line).unwrap();
        assert_eq!(overflow.x, px(30));
        assert_eq!(overflow.width, px(150));
    }
}
