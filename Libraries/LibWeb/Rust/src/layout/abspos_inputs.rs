/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum StaticPositionAlignment {
    Start,
    Center,
    End,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct StaticPositionRect {
    pub(crate) rect: geometry::LogicalRect,
    pub(crate) inline_alignment: StaticPositionAlignment,
    pub(crate) block_alignment: StaticPositionAlignment,
    pub(crate) alignment_derives_from_own_computed_values: bool,
    /// False when the box was laid out without computing where it would have been in flow, because
    /// insets placed it on both axes. Such a rect stands in for nothing and cannot be replayed.
    pub(crate) is_known: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AbsposAxisMode {
    StaticPosition,
    InsetFromRect,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum AbsposAlignment {
    AnchorCenter,
    Baseline,
    Center,
    End,
    Normal,
    Safe,
    SelfEnd,
    SelfStart,
    SpaceAround,
    SpaceBetween,
    SpaceEvenly,
    Start,
    Stretch,
    Unsafe,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AbsposContainingBlockInfo {
    pub(crate) rect: geometry::LogicalRect,
    pub(crate) inline_axis_mode: AbsposAxisMode,
    pub(crate) block_axis_mode: AbsposAxisMode,
    pub(crate) inline_alignment: Option<AbsposAlignment>,
    pub(crate) block_alignment: Option<AbsposAlignment>,
    pub(crate) derives_from_own_computed_values: bool,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AbsposLayoutInputs {
    pub(crate) containing_block: super::formatting_context::Node,
    pub(crate) inline_containing_block: super::formatting_context::Node,
    pub(crate) static_position_rect: StaticPositionRect,
    pub(crate) containing_block_info: AbsposContainingBlockInfo,
    pub(crate) resolved_anchor_insets: Option<super::formatting_context::ResolvedAnchorInsets>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct ContainingBlockSearch {
    pub(crate) is_fixed_position: bool,
    pub(crate) containing_block: super::formatting_context::Node,
    pub(crate) inline_containing_block: super::formatting_context::Node,
    pub(crate) frontier: super::formatting_context::Node,
}

impl ContainingBlockSearch {
    pub(crate) fn starting_at(out_of_flow_box: super::formatting_context::Node, is_fixed_position: bool) -> Self {
        Self {
            is_fixed_position,
            containing_block: super::node_data::NodeSlotId::INVALID,
            inline_containing_block: super::node_data::NodeSlotId::INVALID,
            frontier: out_of_flow_box,
        }
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PendingAbsposChild {
    pub(crate) child_box: super::formatting_context::Node,
    pub(crate) coordinate_space_box: super::formatting_context::Node,
    pub(crate) static_position_rect: StaticPositionRect,
    pub(crate) containing_block_info_override: Option<AbsposContainingBlockInfo>,
    pub(crate) containing_block_search: ContainingBlockSearch,
}

impl PendingAbsposChild {
    pub(crate) fn containing_block(&self) -> super::formatting_context::Node {
        self.containing_block_search.containing_block
    }

    pub(crate) fn inline_containing_block(&self) -> super::formatting_context::Node {
        self.containing_block_search.inline_containing_block
    }
}
