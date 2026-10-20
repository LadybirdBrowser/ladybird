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
}

pub(crate) fn used_column_gap(style: StyleValues<'_>, percentage_basis: CssPixels) -> CssPixels {
    let gap = style.column_gap();
    if gap.is_normal() {
        return style.font_size();
    }
    gap.to_px(percentage_basis).max(CssPixels::default())
}

pub(crate) const MAXIMUM_COLUMN_COUNT: usize = 1000;

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
    pub(crate) column_inline_size: CssPixels,
    pub(crate) column_gap: CssPixels,
    pub(crate) rows: Vec<ColumnRow>,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) enum BlockResume {
    BeforeChild { child: Node, after_forced_break: bool },
    InsideChild { child: Node, token: Rc<BlockBreakToken> },
    AfterChildren,
    AtLine(LinePosition),
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct LinePosition {
    pub(crate) item_index: usize,
    pub(crate) offset_in_node: usize,
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct BlockBreakToken {
    pub(crate) node: Node,
    pub(crate) consumed_block_size: CssPixels,
    pub(crate) resume: BlockResume,
}

impl BlockBreakToken {
    pub(crate) fn ends_at_forced_break(&self) -> bool {
        match &self.resume {
            BlockResume::BeforeChild { after_forced_break, .. } => *after_forced_break,
            BlockResume::InsideChild { token, .. } => token.ends_at_forced_break(),
            BlockResume::AfterChildren | BlockResume::AtLine(_) => false,
        }
    }
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct LineRootFragmentation {
    pub(crate) block_size: Option<CssPixels>,
    pub(crate) resume: Option<LinePosition>,
    pub(crate) fragmentainer_had_content: bool,
}

#[derive(Clone, Debug, Default)]
pub(crate) struct FragmentainerOutcome {
    pub(crate) outgoing: Option<Rc<BlockBreakToken>>,
    pub(crate) needs_early_break_before: Option<Node>,
    pub(crate) minimal_space_shortage: Option<CssPixels>,
    pub(crate) tallest_unbreakable_block_size: CssPixels,
    pub(crate) violates_break_avoidance: bool,
}

impl FragmentainerOutcome {
    pub(crate) fn ended_at_forced_break(&self) -> bool {
        self.outgoing.as_ref().is_some_and(|token| token.ends_at_forced_break())
    }
}

pub(crate) struct FragmentainerContext {
    pub(crate) block_size: Option<CssPixels>,
    pub(crate) incoming: Option<Rc<BlockBreakToken>>,
    pub(crate) inline_items: Rc<inline_level_iterator::InlineItemCache>,
    pub(crate) early_break_before: Option<Node>,
    pub(crate) allows_early_break_requests: bool,
    outcome: RefCell<FragmentainerOutcome>,
}

impl FragmentainerContext {
    pub(crate) fn new(
        block_size: Option<CssPixels>,
        incoming: Option<Rc<BlockBreakToken>>,
        inline_items: Rc<inline_level_iterator::InlineItemCache>,
        early_break_before: Option<Node>,
        allows_early_break_requests: bool,
    ) -> Self {
        Self {
            block_size,
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
        self.outcome.borrow_mut().needs_early_break_before = Some(node);
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
