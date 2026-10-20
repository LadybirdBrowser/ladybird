/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::cell::{RefCell, RefMut};
use std::rc::Rc;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ColumnGeometry {
    pub(crate) count: usize,
    pub(crate) inline_size: CssPixels,
    pub(crate) gap: CssPixels,
}

impl ColumnGeometry {
    pub(crate) fn pitch(&self) -> CssPixels {
        self.inline_size + self.gap
    }

    pub(crate) fn column_inline_offset(
        &self,
        index: usize,
        content_inline_size: CssPixels,
        is_right_to_left: bool,
    ) -> CssPixels {
        let offset = self.pitch() * index;
        if is_right_to_left {
            content_inline_size - self.inline_size - offset
        } else {
            offset
        }
    }

    pub(crate) fn gap_center(
        &self,
        gap_index: usize,
        content_inline_size: CssPixels,
        is_right_to_left: bool,
    ) -> CssPixels {
        self.column_inline_offset(gap_index, content_inline_size, is_right_to_left)
            .min(self.column_inline_offset(gap_index + 1, content_inline_size, is_right_to_left))
            + self.inline_size
            + self.gap / 2usize
    }
}

pub(crate) fn used_column_gap(style: StyleValues<'_>, percentage_basis: CssPixels) -> CssPixels {
    let gap = style.column_gap();
    if gap.is_normal() {
        return style.font_size();
    }
    gap.to_px(percentage_basis).max(CssPixels::default())
}

pub(crate) const MAXIMUM_COLUMN_COUNT: usize = 1000;

pub(crate) fn effects_need_unbroken_box(style: ComputedValuesView<'_>) -> bool {
    crate::painting::style_queries::clip_path_has_value(style.mask())
        || crate::painting::style_queries::mask_layers_have_image(style.mask())
        || style.effects().backdrop_filter.operations.length != 0
}

pub(crate) fn resolved_column_count(style: StyleValues<'_>) -> usize {
    (style.column_count().max(1) as usize).min(MAXIMUM_COLUMN_COUNT)
}

pub(crate) fn resolved_column_width(style: StyleValues<'_>, percentage_basis: CssPixels) -> Option<CssPixels> {
    let column_width = style.column_width();
    (!column_width.is_auto()).then(|| column_width.to_px(percentage_basis).max(CssPixels::from_integer(1)))
}

// https://drafts.csswg.org/css-multicol-1/#pseudo-algorithm
pub(crate) fn resolve_column_geometry(style: StyleValues<'_>, available_inline_size: CssPixels) -> ColumnGeometry {
    let available_inline_size = available_inline_size.max(CssPixels::default());
    let gap = used_column_gap(style, available_inline_size);
    let count = match resolved_column_width(style, available_inline_size) {
        // (04) N := column-count
        None => resolved_column_count(style),
        Some(column_width) => {
            // (06) N := max(1, floor((U + column-gap)/(column-width + column-gap)))
            let fitting_count = ((i64::from(available_inline_size.raw_value()) + i64::from(gap.raw_value()))
                / (i64::from(column_width.raw_value()) + i64::from(gap.raw_value())))
            .clamp(1, MAXIMUM_COLUMN_COUNT as i64) as usize;
            if style.has_column_count() {
                // (08) N := min(column-count, max(1, floor((U + column-gap)/(column-width + column-gap))))
                fitting_count.min(resolved_column_count(style))
            } else {
                fitting_count
            }
        }
    };
    // (09) W := max(0, ((U + column-gap)/N - column-gap))
    let inline_size = CssPixels::from_raw(
        ((i64::from(available_inline_size.raw_value()) + i64::from(gap.raw_value())) / count as i64
            - i64::from(gap.raw_value()))
        .max(0) as i32,
    );
    ColumnGeometry {
        count,
        inline_size,
        gap,
    }
}

// https://drafts.csswg.org/css-multicol-1/#intrinsic-sizes
pub(crate) fn multicol_intrinsic_inline_size(
    style: StyleValues<'_>,
    content_inline_size: CssPixels,
    is_min_content: bool,
) -> CssPixels {
    let column_count = if style.has_column_count() {
        resolved_column_count(style)
    } else {
        1
    };
    let gaps = used_column_gap(style, CssPixels::default()) * (column_count - 1);
    let column_width = resolved_column_width(style, CssPixels::default());
    if is_min_content {
        // The min-content inline size of a multi-column container with a computed column-width not auto is the
        // smaller of its column-width and the largest min-content contribution of its contents.
        if let Some(column_width) = column_width {
            return column_width.min(content_inline_size);
        }
        // The min-content inline size of a multi-column container with a computed column-width of auto is the
        // largest min-content contribution of its contents multiplied by its column-count (treating auto as 1),
        // plus its column-gap multiplied by column-count minus 1.
        return content_inline_size * column_count + gaps;
    }
    // The max-content inline size of a multi-column container is the product of its column-count (treating auto
    // as 1) and the larger of its column-width (treating auto as zero) and the largest max-content contribution of
    // its contents, plus its column-gap multiplied by one less than its column-count.
    content_inline_size.max(column_width.unwrap_or_default()) * column_count + gaps
}

pub(crate) fn ceil_divide(value: CssPixels, divisor: usize) -> CssPixels {
    let divisor = divisor.max(1) as i64;
    CssPixels::from_raw(((value.raw_value() as i64 + divisor - 1).div_euclid(divisor)) as i32)
}

pub(crate) fn is_forced_column_break(value: u8) -> bool {
    value == break_between::COLUMN
}

pub(crate) fn avoids_column_break_between(value: u8) -> bool {
    matches!(value, break_between::AVOID | break_between::AVOID_COLUMN)
}

pub(crate) fn avoids_column_break_inside(value: u8) -> bool {
    matches!(value, break_inside::AVOID | break_inside::AVOID_COLUMN)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ColumnRow {
    pub(crate) block_offset: CssPixels,
    pub(crate) block_size: CssPixels,
    pub(crate) column_count: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct MulticolLayoutData {
    pub(crate) geometry: ColumnGeometry,
    pub(crate) rows: Vec<ColumnRow>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum BlockResume {
    BeforeChild { child: Node, after_forced_break: bool },
    InsideChild(Rc<BlockBreakToken>),
    AfterChildren,
    AtLine(LinePosition),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LinePosition {
    pub(crate) item_index: usize,
    pub(crate) offset_in_node: usize,
}

impl LinePosition {
    pub(crate) const AFTER_LAST_LINE: Self = Self {
        item_index: usize::MAX,
        offset_in_node: 0,
    };
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct BlockBreakToken {
    pub(crate) node: Node,
    pub(crate) consumed_block_size: CssPixels,
    pub(crate) resume: BlockResume,
}

impl BlockResume {
    pub(crate) fn resume_child(&self) -> Option<Node> {
        match self {
            BlockResume::BeforeChild { child, .. } => Some(*child),
            BlockResume::InsideChild(token) => Some(token.node),
            BlockResume::AfterChildren | BlockResume::AtLine(_) => None,
        }
    }
}

impl BlockBreakToken {
    pub(crate) fn follows_forced_break(&self) -> bool {
        match &self.resume {
            BlockResume::BeforeChild { after_forced_break, .. } => *after_forced_break,
            BlockResume::InsideChild(token) => token.follows_forced_break(),
            BlockResume::AfterChildren | BlockResume::AtLine(_) => false,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct LineRootFragmentation {
    pub(crate) block_size: Option<CssPixels>,
    pub(crate) resume: Option<LinePosition>,
    pub(crate) fragmentainer_had_content: bool,
    pub(crate) break_before_is_avoided: bool,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct FragmentainerOutcome {
    pub(crate) outgoing: Option<Rc<BlockBreakToken>>,
    pub(crate) needs_early_break_before: Option<Node>,
    pub(crate) minimal_space_shortage: Option<CssPixels>,
    pub(crate) tallest_unbreakable_block_size: CssPixels,
    pub(crate) violates_break_avoidance: bool,
    pub(crate) overflows: bool,
}

pub(crate) struct FragmentainerContext {
    pub(crate) block_size: Option<CssPixels>,
    pub(crate) honors_forced_breaks: bool,
    pub(crate) incoming: Option<Rc<BlockBreakToken>>,
    pub(crate) inline_items: Rc<inline_level_iterator::InlineItemCache>,
    pub(crate) early_break_before: Option<Node>,
    pub(crate) allows_early_break_requests: bool,
    outcome: RefCell<FragmentainerOutcome>,
}

impl FragmentainerContext {
    pub(crate) fn new(
        block_size: Option<CssPixels>,
        honors_forced_breaks: bool,
        incoming: Option<Rc<BlockBreakToken>>,
        inline_items: Rc<inline_level_iterator::InlineItemCache>,
        early_break_before: Option<Node>,
        allows_early_break_requests: bool,
    ) -> Self {
        Self {
            block_size,
            honors_forced_breaks,
            incoming,
            inline_items,
            early_break_before,
            allows_early_break_requests,
            outcome: RefCell::new(FragmentainerOutcome::default()),
        }
    }

    pub(crate) fn outcome_mut(&self) -> RefMut<'_, FragmentainerOutcome> {
        self.outcome.borrow_mut()
    }

    pub(crate) fn request_early_break_before(&self, node: Node) -> bool {
        if !self.allows_early_break_requests {
            return false;
        }
        let mut outcome = self.outcome.borrow_mut();
        if outcome.needs_early_break_before.is_none() {
            outcome.needs_early_break_before = Some(node);
        }
        true
    }

    pub(crate) fn take_outcome(&self) -> FragmentainerOutcome {
        self.outcome.take()
    }

    pub(crate) fn exceeds(&self, block_end: CssPixels) -> bool {
        self.block_size.is_some_and(|block_size| block_end > block_size)
    }

    pub(crate) fn note_space_shortage(&self, block_end: CssPixels) {
        let Some(block_size) = self.block_size else {
            return;
        };
        let shortage = block_end - block_size;
        if shortage <= CssPixels::default() {
            return;
        }
        let mut outcome = self.outcome.borrow_mut();
        outcome.minimal_space_shortage = Some(
            outcome
                .minimal_space_shortage
                .map_or(shortage, |minimal| minimal.min(shortage)),
        );
    }

    pub(crate) fn note_unbreakable_block_size(&self, block_size: CssPixels) {
        let mut outcome = self.outcome.borrow_mut();
        outcome.tallest_unbreakable_block_size = outcome.tallest_unbreakable_block_size.max(block_size);
    }
}
