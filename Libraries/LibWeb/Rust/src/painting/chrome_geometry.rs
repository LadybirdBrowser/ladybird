/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_enums;
use crate::css::css_pixels::{CssPixelFraction, CssPixelPoint, CssPixelRect, CssPixels};
use crate::layout::node_data::NodeSlotId;
use crate::painting::display_list::commands::VISUAL_VIEWPORT_NODE_INDEX;
use crate::painting::ffi::{FfiChromeMetrics, ScrollDirection};
use crate::painting::host::{FfiHitTestQueryCallbacks, FfiRecordingInputs, FfiRootBackgroundSource};
use crate::painting::paintable_data::{PaintableFlag, PaintableKind};
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::style_queries;
use libgfx_rust::Color;

#[derive(Clone, Copy, Debug)]
pub(crate) struct ScrollbarData {
    pub(crate) gutter_rect: CssPixelRect,
    pub(crate) thumb_rect: CssPixelRect,
    pub(crate) track_rect: CssPixelRect,
    pub(crate) thumb_travel_to_scroll_ratio: CssPixelFraction,
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct ScrollbarScrollState {
    pub(crate) device_scroll_offset: f32,
    pub(crate) device_pixels_per_css_pixel: f64,
}

#[derive(Clone, Copy, Debug, Default)]
pub(crate) struct PhysicalAxes {
    pub(crate) horizontal: bool,
    pub(crate) vertical: bool,
}

impl PhysicalAxes {
    fn along(self, direction: ScrollDirection) -> bool {
        match direction {
            ScrollDirection::Horizontal => self.horizontal,
            ScrollDirection::Vertical => self.vertical,
        }
    }
}

fn primary_size(rect: CssPixelRect, direction: ScrollDirection) -> CssPixels {
    match direction {
        ScrollDirection::Horizontal => rect.width,
        ScrollDirection::Vertical => rect.height,
    }
}

fn primary_offset(point: CssPixelPoint, direction: ScrollDirection) -> CssPixels {
    match direction {
        ScrollDirection::Horizontal => point.x,
        ScrollDirection::Vertical => point.y,
    }
}

struct AxisView<'r> {
    primary_offset: &'r mut CssPixels,
    primary_size: &'r mut CssPixels,
    secondary_offset: &'r mut CssPixels,
    secondary_size: &'r mut CssPixels,
}

fn axis_view(rect: &mut CssPixelRect, direction: ScrollDirection) -> AxisView<'_> {
    match direction {
        ScrollDirection::Horizontal => AxisView {
            primary_offset: &mut rect.x,
            primary_size: &mut rect.width,
            secondary_offset: &mut rect.y,
            secondary_size: &mut rect.height,
        },
        ScrollDirection::Vertical => AxisView {
            primary_offset: &mut rect.y,
            primary_size: &mut rect.height,
            secondary_offset: &mut rect.x,
            secondary_size: &mut rect.width,
        },
    }
}

pub(crate) fn is_chrome_mirrored(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> bool {
    arena.node_style_if_live(slot).is_some_and(|style| {
        let writing_mode = style.writing_mode();
        (writing_mode == css_enums::writing_mode::HORIZONTAL_TB && style.direction() == css_enums::direction::RTL)
            || writing_mode == css_enums::writing_mode::VERTICAL_RL
            || writing_mode == css_enums::writing_mode::SIDEWAYS_RL
    })
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
    viewport_wheel_overflow_x: u8,
    viewport_wheel_overflow_y: u8,
) -> PhysicalAxes {
    let Some(style) = arena.node_style_if_live(slot) else {
        return PhysicalAxes::default();
    };
    let box_values = style.box_values();
    let is_viewport = arena.paintable_data(slot).kind == PaintableKind::ViewportPaintable;
    let (overflow_x, overflow_y) = if is_viewport {
        (viewport_wheel_overflow_x, viewport_wheel_overflow_y)
    } else {
        (box_values.overflow_x, box_values.overflow_y)
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

pub(crate) fn minimum_scroll_offset(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> CssPixelPoint {
    let Some(overflow) = paintable_geometry::scrollable_overflow_rect(arena, slot) else {
        return CssPixelPoint::default();
    };
    let scrollport = paintable_geometry::absolute_padding_box_rect(arena, slot);
    let zero = CssPixels::from_raw(0);
    CssPixelPoint::new(
        (overflow.left() - scrollport.left()).min(zero),
        (overflow.top() - scrollport.top()).min(zero),
    )
}

pub(crate) fn maximum_scroll_offset(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> CssPixelPoint {
    let Some(overflow) = paintable_geometry::scrollable_overflow_rect(arena, slot) else {
        return CssPixelPoint::default();
    };
    let scrollport = paintable_geometry::absolute_padding_box_rect(arena, slot);
    let zero = CssPixels::from_raw(0);
    CssPixelPoint::new(
        (overflow.right() - scrollport.right()).max(zero),
        (overflow.bottom() - scrollport.bottom()).max(zero),
    )
}

pub(crate) fn scrollbar_is_enlarged(
    arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    direction: ScrollDirection,
) -> bool {
    let flag = match direction {
        ScrollDirection::Horizontal => PaintableFlag::HorizontalScrollbarEnlarged,
        ScrollDirection::Vertical => PaintableFlag::VerticalScrollbarEnlarged,
    };
    arena.paintable_data(slot).has_flag(flag)
}

pub(crate) struct ChromeGeometry<'a, Arena: PaintableRowsRead> {
    pub(crate) arena: &'a Arena,
    pub(crate) metrics: FfiChromeMetrics,
    pub(crate) viewport_wheel_overflow_x: u8,
    pub(crate) viewport_wheel_overflow_y: u8,
}

impl<'a, Arena: PaintableRowsRead> ChromeGeometry<'a, Arena> {
    pub(crate) fn for_recording(arena: &'a Arena, inputs: &FfiRecordingInputs) -> Self {
        Self {
            arena,
            metrics: inputs.chrome_metrics,
            viewport_wheel_overflow_x: inputs.viewport_wheel_overflow_x,
            viewport_wheel_overflow_y: inputs.viewport_wheel_overflow_y,
        }
    }

    pub(crate) fn for_hit_test_query(arena: &'a Arena, callbacks: &FfiHitTestQueryCallbacks) -> Self {
        Self {
            arena,
            metrics: callbacks.chrome_metrics,
            viewport_wheel_overflow_x: callbacks.viewport_wheel_overflow_x,
            viewport_wheel_overflow_y: callbacks.viewport_wheel_overflow_y,
        }
    }

    fn wheel_scrollable_axes(&self, slot: NodeSlotId) -> PhysicalAxes {
        wheel_scrollable_axes(
            self.arena,
            slot,
            self.viewport_wheel_overflow_x,
            self.viewport_wheel_overflow_y,
        )
    }

    pub(crate) fn absolute_resizer_rect(&self, slot: NodeSlotId) -> Option<CssPixelRect> {
        if !has_resizer(self.arena, slot) {
            return None;
        }
        let padding_rect = paintable_geometry::absolute_padding_box_rect(self.arena, slot);
        let gripper_size = self.metrics.resize_gripper_size;
        let x = if is_chrome_mirrored(self.arena, slot) {
            padding_rect.x
        } else {
            padding_rect.right() - gripper_size
        };
        Some(CssPixelRect::new(
            x,
            padding_rect.bottom() - gripper_size,
            gripper_size,
            gripper_size,
        ))
    }

    pub(crate) fn resizer_contains(&self, slot: NodeSlotId, point: CssPixelPoint) -> bool {
        if !self.arena.paintable_row_is_populated(slot) {
            return false;
        }
        let Some(mut rect) = self.absolute_resizer_rect(slot) else {
            return false;
        };
        let border = paintable_geometry::committed_border(self.arena, slot);
        if is_chrome_mirrored(self.arena, slot) {
            rect.x -= border.left;
            rect.width += border.left;
        } else {
            rect.width += border.right;
        }
        rect.height += border.bottom;
        rect.contains_point(point)
    }

    fn available_scrollbar_length(&self, slot: NodeSlotId, direction: ScrollDirection) -> CssPixels {
        if !self.arena.paintable_row_is_populated(slot) {
            return CssPixels::from_raw(0);
        }
        let mut length = primary_size(
            paintable_geometry::absolute_padding_box_rect(self.arena, slot),
            direction,
        );
        let axes = self.wheel_scrollable_axes(slot);
        let other_axis_scrolls = match direction {
            ScrollDirection::Horizontal => axes.vertical,
            ScrollDirection::Vertical => axes.horizontal,
        };
        if has_resizer(self.arena, slot) {
            length -= self.metrics.resize_gripper_size;
        } else if other_axis_scrolls {
            length -= self.metrics.scroll_gutter_thickness;
        }
        length
    }

    pub(crate) fn absolute_scrollbar_rect(
        &self,
        slot: NodeSlotId,
        direction: ScrollDirection,
        with_gutter: bool,
    ) -> Option<CssPixelRect> {
        if !self.arena.paintable_row_is_populated(slot) {
            return None;
        }
        let axes = self.wheel_scrollable_axes(slot);
        if !axes.along(direction) {
            return None;
        }
        let style = self.arena.node_style_if_live(slot)?;
        if style.misc_reset().scrollbar_width == css_enums::scrollbar_width::NONE {
            return None;
        }

        let metrics = self.metrics;
        let adjusting_for_resizer = has_resizer(self.arena, slot);
        let mirrored = is_chrome_mirrored(self.arena, slot);
        let rect_thickness = if with_gutter {
            metrics.scroll_gutter_thickness
        } else {
            metrics.scroll_thumb_thickness_thin + metrics.scroll_thumb_padding_thin
        };
        let zero = CssPixels::from_raw(0);
        let mut rect = paintable_geometry::absolute_padding_box_rect(self.arena, slot);
        match direction {
            ScrollDirection::Horizontal => {
                if !adjusting_for_resizer && axes.vertical {
                    rect.width = (rect.width - metrics.scroll_gutter_thickness).max(zero);
                    if mirrored {
                        rect.x += metrics.scroll_gutter_thickness;
                    }
                } else if adjusting_for_resizer {
                    rect.width = self.available_scrollbar_length(slot, direction);
                    if mirrored {
                        rect.x += metrics.resize_gripper_size;
                    }
                }
                rect.y = (rect.bottom() - rect_thickness).max(zero);
                rect.height = rect_thickness;
            }
            ScrollDirection::Vertical => {
                if adjusting_for_resizer {
                    rect.height = self.available_scrollbar_length(slot, direction);
                }
                if !mirrored {
                    rect.x = (rect.right() - rect_thickness).max(zero);
                }
                rect.width = rect_thickness;
            }
        }
        Some(rect)
    }

    pub(crate) fn compute_scrollbar_data(
        &self,
        slot: NodeSlotId,
        direction: ScrollDirection,
        enlarged: bool,
        scroll_state: Option<ScrollbarScrollState>,
    ) -> Option<ScrollbarData> {
        let arena = self.arena;
        let metrics = self.metrics;
        if !arena.paintable_row_is_populated(slot) {
            return None;
        }
        let style = arena.node_style_if_live(slot)?;
        let overflow = match direction {
            ScrollDirection::Horizontal => style.box_values().overflow_x,
            ScrollDirection::Vertical => style.box_values().overflow_y,
        };
        if overflow != css_enums::overflow::SCROLL && !self.wheel_scrollable_axes(slot).along(direction) {
            return None;
        }
        if arena.paintable_data(slot).own_scroll_node_index == VISUAL_VIEWPORT_NODE_INDEX {
            return None;
        }
        let overflow_length = primary_size(paintable_geometry::scrollable_overflow_rect(arena, slot)?, direction);
        if overflow_length == CssPixels::from_raw(0) {
            return None;
        }
        let scrollbar_rect = self.absolute_scrollbar_rect(slot, direction, enlarged)?;
        let (thumb_thickness, thumb_margin) = if enlarged {
            (
                metrics.scroll_thumb_thickness,
                (metrics.scroll_gutter_thickness - metrics.scroll_thumb_thickness) / 2,
            )
        } else {
            (metrics.scroll_thumb_thickness_thin, metrics.scroll_thumb_padding_thin)
        };
        let usable_length = (primary_size(scrollbar_rect, direction) - thumb_margin * 2).max(CssPixels::from_raw(0));
        let scrollport_size = primary_size(paintable_geometry::absolute_padding_box_rect(arena, slot), direction);
        let min_thumb_length = usable_length.min(metrics.scroll_thumb_min_length);
        let thumb_length = usable_length
            .mul_by_fraction(CssPixelFraction::ratio_of(scrollport_size, overflow_length))
            .max(min_thumb_length);
        let ratio = if overflow_length > scrollport_size {
            CssPixelFraction::ratio_of(usable_length - thumb_length, overflow_length - scrollport_size)
        } else {
            CssPixelFraction::zero()
        };
        let mut thumb_rect = scrollbar_rect;
        let thumb = axis_view(&mut thumb_rect, direction);
        *thumb.primary_size = thumb_length;
        *thumb.secondary_size = thumb_thickness;
        let minimum_offset = primary_offset(minimum_scroll_offset(arena, slot), direction);
        *thumb.primary_offset += thumb_margin - minimum_offset.mul_by_fraction(ratio);
        if enlarged || (direction == ScrollDirection::Vertical && is_chrome_mirrored(arena, slot)) {
            *thumb.secondary_offset += thumb_margin;
        }
        if let Some(scroll_state) = scroll_state {
            let scroll_offset = CssPixels::nearest_value_for_f32(
                scroll_state.device_scroll_offset / scroll_state.device_pixels_per_css_pixel as f32,
            );
            *thumb.primary_offset += scroll_offset.mul_by_fraction(ratio);
        }
        Some(ScrollbarData {
            gutter_rect: if enlarged {
                scrollbar_rect
            } else {
                CssPixelRect::default()
            },
            thumb_rect,
            track_rect: scrollbar_rect,
            thumb_travel_to_scroll_ratio: ratio,
        })
    }
}

fn is_canvas_background_source(
    arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    root_background_source: FfiRootBackgroundSource,
) -> bool {
    style_queries::node_is_root_element(arena, slot)
        || (root_background_source.use_body_background_properties && root_background_source.body_layout_node == slot)
}

pub(crate) fn scrollbar_colors_for_paint(
    arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
    root_background_source: FfiRootBackgroundSource,
    canvas_background_color: Color,
) -> (Color, Color) {
    let Some(style) = arena.node_style_if_live(slot) else {
        return (Color::TRANSPARENT, Color::TRANSPARENT);
    };
    let colors = style.inherited_ui().scrollbar_color;
    if !colors.is_auto {
        return (Color(colors.thumb_color), Color(colors.track_color));
    }

    let mut ancestors = Vec::new();
    let mut current = Some(slot);
    while let Some(ancestor) = current {
        ancestors.push(ancestor);
        current = arena.node_parent_if_live(ancestor);
    }
    let mut background_color = canvas_background_color;
    for ancestor in ancestors.into_iter().rev() {
        if is_canvas_background_source(arena, ancestor, root_background_source) {
            continue;
        }
        let Some(style) = arena.node_style_if_live(ancestor) else {
            continue;
        };
        let color = Color(style.background().background_color);
        if color.alpha() != 0 {
            background_color = background_color.blend(color);
        }
    }
    let black_thumb = Color::from_rgb(0, 0, 0).with_alpha(128);
    let white_thumb = Color::from_rgb(255, 255, 255).with_alpha(128);
    let black_contrast = background_color.contrast_ratio(background_color.blend(black_thumb));
    let white_contrast = background_color.contrast_ratio(background_color.blend(white_thumb));
    let thumb = if black_contrast >= white_contrast {
        black_thumb
    } else {
        white_thumb
    };
    (thumb, thumb.with_alpha(25))
}
