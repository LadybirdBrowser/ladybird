/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums;
use crate::layout::node_data::NodeSlotId;
use crate::painting::paintable_data::PaintableKind;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct PhysicalAxes {
    pub(crate) horizontal: bool,
    pub(crate) vertical: bool,
}

pub(crate) fn physical_resize_axes(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> PhysicalAxes {
    let Some(style) = arena.node_style_if_live(slot) else {
        return PhysicalAxes::default();
    };
    let box_values = style.box_values();
    if box_values.resize == css_enums::resize::NONE {
        return PhysicalAxes::default();
    }
    if style.display().is_inline_outside() && style.display().is_flow_inside() {
        return PhysicalAxes::default();
    }

    let horizontal_writing_mode = style.writing_mode() == css_enums::writing_mode::HORIZONTAL_TB;
    let overflow_allows_resize =
        |overflow| overflow != css_enums::overflow::VISIBLE && overflow != css_enums::overflow::CLIP;
    PhysicalAxes {
        horizontal: overflow_allows_resize(box_values.overflow_x)
            && (box_values.resize == css_enums::resize::BOTH
                || box_values.resize == css_enums::resize::HORIZONTAL
                || (box_values.resize == css_enums::resize::INLINE && horizontal_writing_mode)
                || (box_values.resize == css_enums::resize::BLOCK && !horizontal_writing_mode)),
        vertical: overflow_allows_resize(box_values.overflow_y)
            && (box_values.resize == css_enums::resize::BOTH
                || box_values.resize == css_enums::resize::VERTICAL
                || (box_values.resize == css_enums::resize::INLINE && !horizontal_writing_mode)
                || (box_values.resize == css_enums::resize::BLOCK && horizontal_writing_mode)),
    }
}

pub(crate) fn has_resizer(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> bool {
    if !arena.paintable_row_is_populated(slot)
        || arena.paintable_data(slot).kind == PaintableKind::ViewportPaintable
        || arena.node_is_generated_for_pseudo_element(slot)
    {
        return false;
    }
    let axes = physical_resize_axes(arena, slot);
    axes.horizontal || axes.vertical
}

pub(crate) fn wheel_scrollable_axes(
    arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    viewport_overflow_x: u8,
    viewport_overflow_y: u8,
) -> PhysicalAxes {
    let Some(style) = arena.node_style_if_live(slot) else {
        return PhysicalAxes::default();
    };
    let box_values = style.box_values();
    let is_viewport = arena.paintable_data(slot).kind == PaintableKind::ViewportPaintable;
    let overflow_x = if is_viewport {
        viewport_overflow_x
    } else {
        box_values.overflow_x
    };
    let overflow_y = if is_viewport {
        viewport_overflow_y
    } else {
        box_values.overflow_y
    };
    let allows_wheel_scrolling =
        |overflow| overflow == css_enums::overflow::AUTO || overflow == css_enums::overflow::SCROLL;
    let mut axes = PhysicalAxes {
        horizontal: allows_wheel_scrolling(overflow_x),
        vertical: allows_wheel_scrolling(overflow_y),
    };
    if !axes.horizontal && !axes.vertical {
        return axes;
    }

    let Some(scrollable_overflow) = paintable_geometry::scrollable_overflow_rect(arena, slot) else {
        return PhysicalAxes::default();
    };
    let scrollport = paintable_geometry::absolute_padding_box_rect(arena, slot);
    axes.horizontal &= scrollable_overflow.width > scrollport.width;
    axes.vertical &= scrollable_overflow.height > scrollport.height;
    axes
}
