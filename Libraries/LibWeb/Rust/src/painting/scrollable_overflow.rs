/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums::{flex_direction, flex_wrap, writing_mode};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;

pub(crate) struct PhysicalOverflowDirections {
    pub(crate) horizontal_axis_is_positive: bool,
    pub(crate) vertical_axis_is_positive: bool,
}

impl Default for PhysicalOverflowDirections {
    fn default() -> Self {
        Self {
            horizontal_axis_is_positive: true,
            vertical_axis_is_positive: true,
        }
    }
}

struct AxisDirection {
    is_horizontal: bool,
    is_reverse: bool,
}

// https://drafts.csswg.org/cssom-view/#overflow-directions
pub(crate) fn physical_overflow_directions(
    layout_arena: &LayoutNodeArena,
    node: NodeSlotId,
) -> PhysicalOverflowDirections {
    // A scrolling box of a viewport or element has two overflow directions, which are the block-end and inline-end
    // directions for that viewport or element.
    let Some(style) = layout_arena.node_style_if_live(node) else {
        return PhysicalOverflowDirections::default();
    };

    let mut inline_axis = AxisDirection {
        is_horizontal: style.writing_mode() == writing_mode::HORIZONTAL_TB,
        is_reverse: style.inline_axis_is_reverse(),
    };
    let mut block_axis = AxisDirection {
        is_horizontal: !inline_axis.is_horizontal,
        is_reverse: style.block_axis_is_reverse(),
    };

    let display = style.display();
    if display.is_flex_inside() {
        let container_flex_direction = style.flex_direction();
        let is_row_layout = matches!(
            container_flex_direction,
            flex_direction::ROW | flex_direction::ROW_REVERSE
        );
        let (main_axis, cross_axis) = if is_row_layout {
            (&mut inline_axis, &mut block_axis)
        } else {
            (&mut block_axis, &mut inline_axis)
        };

        if matches!(
            container_flex_direction,
            flex_direction::ROW_REVERSE | flex_direction::COLUMN_REVERSE
        ) {
            main_axis.is_reverse = !main_axis.is_reverse;
        }

        // AD-HOC: A legacy webkit box ignores `flex-wrap`, matching other engines.
        if !display.is_webkit_box_inside() && style.flex_wrap() == flex_wrap::WRAP_REVERSE {
            cross_axis.is_reverse = !cross_axis.is_reverse;
        }
    }

    let (horizontal_axis, vertical_axis) = if inline_axis.is_horizontal {
        (inline_axis, block_axis)
    } else {
        (block_axis, inline_axis)
    };
    PhysicalOverflowDirections {
        horizontal_axis_is_positive: !horizontal_axis.is_reverse,
        vertical_axis_is_positive: !vertical_axis.is_reverse,
    }
}
