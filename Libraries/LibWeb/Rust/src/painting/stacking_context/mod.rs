/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

pub mod dump;
pub mod entries;
pub mod verify;

use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::painting::style_queries::{self, effective_z_index};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StackingContextFacts {
    pub establishes_stacking_context: bool,
    pub effective_z_index: Option<i32>,
    pub is_positioned: bool,
    pub is_non_positioned_float: bool,
    pub is_inline_or_replaced_without_own_context: bool,
    pub enclosing_stacking_context: NodeSlotId,
    pub contribution_is_registered: bool,
}

impl StackingContextFacts {
    pub(crate) fn gather(arena: &LayoutNodeArena, slot: NodeSlotId, enclosing: NodeSlotId) -> Self {
        let establishes_stacking_context = style_queries::establishes_stacking_context(arena, slot);
        let is_positioned = style_queries::is_positioned(arena, slot);
        let is_inline_or_replaced =
            style_queries::is_inline(arena, slot) || style_queries::is_replaced_box(arena, slot);
        Self {
            establishes_stacking_context,
            effective_z_index: effective_z_index(arena, slot),
            is_positioned,
            is_non_positioned_float: !is_positioned && style_queries::is_floating(arena, slot),
            is_inline_or_replaced_without_own_context: !establishes_stacking_context && is_inline_or_replaced,
            enclosing_stacking_context: enclosing,
            contribution_is_registered: false,
        }
    }

    pub(crate) fn for_viewport() -> Self {
        Self {
            establishes_stacking_context: true,
            effective_z_index: None,
            is_positioned: false,
            is_non_positioned_float: false,
            is_inline_or_replaced_without_own_context: false,
            enclosing_stacking_context: NodeSlotId::INVALID,
            contribution_is_registered: false,
        }
    }

    pub(crate) fn nonzero_z_index_child_contribution(&self) -> Option<i32> {
        if self.establishes_stacking_context && self.effective_z_index.unwrap_or(0) != 0 {
            self.effective_z_index
        } else {
            None
        }
    }

    pub(crate) fn contributes_to_stack_level_zero(&self) -> bool {
        (self.is_positioned || self.establishes_stacking_context) && self.effective_z_index.unwrap_or(0) == 0
    }

    pub(crate) fn contributes_non_positioned_float(&self) -> bool {
        self.is_non_positioned_float
    }

    pub(crate) fn contributes_inline_or_replaced(&self) -> bool {
        self.is_inline_or_replaced_without_own_context
    }

    pub(crate) fn same_contributions_as(&self, other: &Self) -> bool {
        self.establishes_stacking_context == other.establishes_stacking_context
            && self.effective_z_index == other.effective_z_index
            && self.is_positioned == other.is_positioned
            && self.is_non_positioned_float == other.is_non_positioned_float
            && self.is_inline_or_replaced_without_own_context == other.is_inline_or_replaced_without_own_context
            && self.enclosing_stacking_context == other.enclosing_stacking_context
    }

    pub(crate) fn paints_inline_content_itself(&self) -> bool {
        self.establishes_stacking_context || self.is_positioned
    }
}
