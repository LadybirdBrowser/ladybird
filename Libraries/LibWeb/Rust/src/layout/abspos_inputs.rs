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

impl StaticPositionAlignment {
    fn reversed(self) -> Self {
        match self {
            Self::Start => Self::End,
            Self::Center => Self::Center,
            Self::End => Self::Start,
        }
    }
}

impl StaticPositionRect {
    /// The rect, laid out in frame `from` within a box whose physical size is `container_size`, as frame `to` sees
    /// it.
    pub(crate) fn in_frame(
        mut self,
        from: geometry::Frame,
        to: geometry::Frame,
        container_size: crate::css::css_pixels::FfiCssPixelSize,
    ) -> Self {
        if from == to {
            return self;
        }
        let rect = crate::css::css_pixels::CssPixelRect {
            x: self.rect.offset.inline_offset,
            y: self.rect.offset.block_offset,
            width: self.rect.size.inline_size,
            height: self.rect.size.block_size,
        };
        let rect = from.rect_in(rect, to, container_size);
        self.rect = geometry::LogicalRect {
            offset: geometry::LogicalOffset {
                inline_offset: rect.x,
                block_offset: rect.y,
            },
            size: geometry::LogicalSize {
                inline_size: rect.width,
                block_size: rect.height,
            },
        };
        // NB: An alignment is towards the left or top of an axis of the frame, which may run along the other
        //     physical axis, or the other way along it, in another frame.
        let along_physical_axis = |frame: geometry::Frame, alignment: StaticPositionAlignment, horizontal: bool| {
            let reversed = if horizontal {
                frame.horizontal_axis_is_reversed()
            } else {
                frame.vertical_axis_is_reversed()
            };
            if reversed { alignment.reversed() } else { alignment }
        };
        let (physical_horizontal, physical_vertical) = {
            let horizontal = along_physical_axis(from, self.inline_alignment, true);
            let vertical = along_physical_axis(from, self.block_alignment, false);
            if from.is_transposed() {
                (vertical, horizontal)
            } else {
                (horizontal, vertical)
            }
        };
        let (frame_horizontal, frame_vertical) = if to.is_transposed() {
            (physical_vertical, physical_horizontal)
        } else {
            (physical_horizontal, physical_vertical)
        };
        self.inline_alignment = along_physical_axis(to, frame_horizontal, true);
        self.block_alignment = along_physical_axis(to, frame_vertical, false);
        self
    }
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
    /// Whether a position area made this the box's containing block.
    pub(crate) is_position_area: bool,
}

impl AbsposContainingBlockInfo {
    /// Whether a position area or anchor-center aligns the box in the inline and the block axis.
    /// https://drafts.csswg.org/css-anchor-position-1/#position-area
    /// https://drafts.csswg.org/css-anchor-position-1/#anchor-center
    pub(crate) fn anchor_aligned_axes(&self) -> (bool, bool) {
        if self.is_position_area {
            return (true, true);
        }
        (
            self.inline_alignment == Some(AbsposAlignment::AnchorCenter),
            self.block_alignment == Some(AbsposAlignment::AnchorCenter),
        )
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct AbsposLayoutInputs {
    pub(crate) containing_block: super::formatting_context::Node,
    pub(crate) inline_containing_block: super::formatting_context::Node,
    pub(crate) static_position_rect: StaticPositionRect,
    pub(crate) containing_block_info: AbsposContainingBlockInfo,
    pub(crate) resolved_anchor_insets: Option<super::formatting_context::ResolvedAnchorInsets>,
    pub(crate) resolved_anchor_sizes: super::style_values::ResolvedAnchorSizes,
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
