/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The geometry scroll snap position selection runs over, collected from the layout of a snap
//! container and of the snap areas it captures. The selection itself lives in
//! `LibWeb/Compositor/ScrollSnapSelection.cpp`, so that the main thread and the compositor process
//! select from the same geometry.

use crate::css::computed_value_types::ComputedLengthBox;
use crate::css::computed_value_views::ComputedValuesView;
use crate::css::css_enums::{
    scroll_snap_align, scroll_snap_axis, scroll_snap_stop, scroll_snap_strictness, writing_mode,
};
use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::{NodeFlag, NodeKind, NodeSlotId};
use crate::layout::node_facts;
use crate::painting::chrome_geometry::{maximum_scroll_offset, minimum_scroll_offset};
use crate::painting::host::{FfiSnapAreaGeometry, FfiSnapAxes, FfiSnapContainerGeometry};
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::style_queries;
use crate::painting::visual_context::node_values::compute_transform;
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect};

/// The box whose style the snap properties of a scroll container come from. The properties
/// specified on the root element apply to the viewport rather than to its own box.
fn snap_style_source(arena: &LayoutNodeArena, snap_container: NodeSlotId) -> Option<NodeSlotId> {
    if arena.node_kind_if_live(snap_container) == Some(NodeKind::Viewport) {
        document_element_box_under(arena, snap_container)
    } else {
        Some(snap_container)
    }
}

fn snap_style(arena: &LayoutNodeArena, snap_container: NodeSlotId) -> Option<ComputedValuesView<'_>> {
    arena.node_style_if_live(snap_style_source(arena, snap_container)?)
}

// https://drafts.csswg.org/css-scroll-snap-1/#snap-axis
pub(crate) fn snap_axes_of_scroll_container(arena: &LayoutNodeArena, snap_container: NodeSlotId) -> FfiSnapAxes {
    snap_style(arena, snap_container).map_or_else(FfiSnapAxes::default, snap_axes_of_style)
}

fn snap_axes_of_style(style: ComputedValuesView<'_>) -> FfiSnapAxes {
    let snap_type = style.misc_reset();
    if snap_type.scroll_snap_strictness == scroll_snap_strictness::NONE {
        return FfiSnapAxes::default();
    }
    let horizontal_writing_mode = style.writing_mode() == writing_mode::HORIZONTAL_TB;
    match snap_type.scroll_snap_axis {
        scroll_snap_axis::X => FfiSnapAxes { x: true, y: false },
        scroll_snap_axis::Y => FfiSnapAxes { x: false, y: true },
        scroll_snap_axis::INLINE => FfiSnapAxes {
            x: horizontal_writing_mode,
            y: !horizontal_writing_mode,
        },
        scroll_snap_axis::BLOCK => FfiSnapAxes {
            x: !horizontal_writing_mode,
            y: horizontal_writing_mode,
        },
        _ => FfiSnapAxes { x: true, y: true },
    }
}

fn document_element_box_under(arena: &LayoutNodeArena, parent: NodeSlotId) -> Option<NodeSlotId> {
    let mut child = arena.data(parent).first_child.get();
    while !child.is_invalid() {
        let flags = arena.node_flags_if_live(child);
        if flags & NodeFlag::IsDocumentElement as u32 != 0 {
            return Some(child);
        }
        if flags & NodeFlag::Anonymous as u32 != 0
            && let Some(found) = document_element_box_under(arena, child)
        {
            return Some(found);
        }
        child = arena.data(child).next_sibling.get();
    }
    None
}

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-padding
// For a scroll snap container this region also defines the scroll snapport, the area of the
// scrollport that is used as the alignment container for the scroll snap areas when calculating
// snap positions.
pub(crate) fn scroll_snapport_rect(
    arena: &LayoutNodeArena,
    snap_container: NodeSlotId,
    scrollport: CssPixelRect,
) -> CssPixelRect {
    let Some(style) = snap_style(arena, snap_container) else {
        return scrollport;
    };
    shrink_by_scroll_padding(scrollport, &style.misc_reset().scroll_padding)
}

fn shrink_by_scroll_padding(scrollport: CssPixelRect, scroll_padding: &ComputedLengthBox) -> CssPixelRect {
    // Percentages refer to the corresponding dimension of the scroll container's scrollport.
    scrollport.shrunken(
        scroll_padding.top.to_px(scrollport.height),
        scroll_padding.right.to_px(scrollport.width),
        scroll_padding.bottom.to_px(scrollport.height),
        scroll_padding.left.to_px(scrollport.width),
    )
}

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-margin
fn inflate_by_scroll_margin(rect: CssPixelRect, scroll_margin: &ComputedLengthBox) -> CssPixelRect {
    let zero = CssPixels::from_raw(0);
    rect.inflated(
        scroll_margin.top.to_px(zero),
        scroll_margin.right.to_px(zero),
        scroll_margin.bottom.to_px(zero),
        scroll_margin.left.to_px(zero),
    )
}

/// The geometry of a snap container, or `None` for a scroll container that snaps in no axis.
pub(crate) fn snap_container_geometry(
    arena: &impl PaintableRowsRead,
    snap_container: NodeSlotId,
) -> Option<FfiSnapContainerGeometry> {
    if !arena.paintable_row_is_populated(snap_container) {
        return None;
    }
    let style = snap_style(arena, snap_container)?;
    let axes = snap_axes_of_style(style);
    if !axes.x && !axes.y {
        return None;
    }
    paintable_geometry::scrollable_overflow_rect(arena, snap_container)?;
    let scrollport = paintable_geometry::absolute_padding_box_rect(arena, snap_container);
    Some(FfiSnapContainerGeometry {
        snapport: shrink_by_scroll_padding(scrollport, &style.misc_reset().scroll_padding).into(),
        min_scroll_offset: minimum_scroll_offset(arena, snap_container).into(),
        max_scroll_offset: maximum_scroll_offset(arena, snap_container).into(),
        strictness: style.misc_reset().scroll_snap_strictness,
        axes,
        horizontal_writing_mode: style.writing_mode() == writing_mode::HORIZONTAL_TB,
    })
}

/// Calls back with the geometry of every snap area the snap container captures, in tree order.
pub(crate) fn for_each_snap_area(
    arena: &impl PaintableRowsRead,
    snap_container: NodeSlotId,
    mut callback: impl FnMut(NodeSlotId, FfiSnapAreaGeometry),
) {
    let Some(container_style) = snap_style(arena, snap_container) else {
        return;
    };
    let snapport = shrink_by_scroll_padding(
        paintable_geometry::absolute_padding_box_rect(arena, snap_container),
        &container_style.misc_reset().scroll_padding,
    );
    let container_writing_mode = WritingModeFacts::of(container_style);
    for_each_descendant_with_snap_alignment(arena, snap_container, &mut |snap_area, style| {
        if let Some(area) = snap_area_geometry(
            arena,
            snap_area,
            style,
            snap_container,
            snapport,
            container_writing_mode,
        ) {
            callback(snap_area, area);
        }
    });
}

fn for_each_descendant_with_snap_alignment<'a>(
    arena: &'a impl PaintableRowsRead,
    parent: NodeSlotId,
    callback: &mut impl FnMut(NodeSlotId, ComputedValuesView<'a>),
) {
    let mut child = arena.node_first_child_if_live(parent);
    while let Some(slot) = child {
        if let Some(style) = snap_alignment_style(arena, slot) {
            callback(slot, style);
        }
        for_each_descendant_with_snap_alignment(arena, slot, callback);
        child = arena.node_next_sibling_if_live(slot);
    }
}

/// The style of a box with a snap alignment that has a committed paintable.
fn snap_alignment_style(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> Option<ComputedValuesView<'_>> {
    let kind = arena.node_kind_if_live(slot)?;
    if node_facts::kind_is_text(kind) || !arena.paintable_row_is_populated(slot) {
        return None;
    }
    arena
        .node_style_if_live(slot)
        .filter(|style| style.has_scroll_snap_alignment())
}

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-area
fn snap_area_geometry(
    arena: &impl PaintableRowsRead,
    snap_area: NodeSlotId,
    style: ComputedValuesView<'_>,
    snap_container: NodeSlotId,
    snapport: CssPixelRect,
    container_writing_mode: WritingModeFacts,
) -> Option<FfiSnapAreaGeometry> {
    let misc = style.misc_reset();
    let rect = inflate_by_scroll_margin(
        captured_snap_area_rect(arena, snap_area, snap_container)?,
        &misc.scroll_margin,
    );

    // https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-align
    // Start and end alignments are resolved with respect to the writing mode of the snap container
    // unless the scroll snap area is larger than the snapport, in which case they are resolved with
    // respect to the writing mode of the box itself.
    let area_writing_mode = WritingModeFacts::of(style);
    let alignment_writing_mode = if area_is_larger_than_snapport(rect, snapport, area_writing_mode.horizontal) {
        area_writing_mode
    } else {
        container_writing_mode
    };
    let (align_x, align_y) = physical_snap_alignment(
        misc.scroll_snap_align_block,
        misc.scroll_snap_align_inline,
        alignment_writing_mode,
    );

    Some(FfiSnapAreaGeometry {
        node_id: arena.paintable_data(snap_area).node_identity,
        pseudo_element_type: arena.node_generated_for(snap_area),
        rect: rect.into(),
        align_x,
        align_y,
        always_stop: misc.scroll_snap_stop == scroll_snap_stop::ALWAYS,
    })
}

// The scroll snap area is determined by taking the transformed border box, finding its rectangular
// bounding box (axis-aligned in the scroll container's coordinate space), then adding the specified
// outsets.
/// `None` when the snap container does not capture the area.
fn captured_snap_area_rect(
    arena: &impl PaintableRowsRead,
    snap_area: NodeSlotId,
    snap_container: NodeSlotId,
) -> Option<CssPixelRect> {
    // Snap areas are captured by the nearest scroll container in their containing block chain, so
    // areas inside a nested scroll container may still belong to an outer container when they are
    // positioned. The boxes between an area and its container contribute transforms only, and
    // mapping the border box through each of them in turn lands it in the container's coordinate
    // space.
    let mut rect = map_rect_through_node_transform(
        arena,
        snap_area,
        paintable_geometry::absolute_border_box_rect(arena, snap_area),
    );
    let mut containing_block = arena.node_containing_block_if_live(snap_area);
    while let Some(block) = containing_block {
        if block == snap_container {
            return Some(rect);
        }
        // The box whose overflow was propagated to the viewport is left with a used overflow of
        // visible, so it is not a scroll container and cannot capture snap areas of its own.
        if style_queries::is_scroll_container(arena, block) {
            return None;
        }
        rect = map_rect_through_node_transform(arena, block, rect);
        containing_block = arena.node_containing_block_if_live(block);
    }
    None
}

fn map_rect_through_node_transform(
    arena: &impl PaintableRowsRead,
    node: NodeSlotId,
    rect: CssPixelRect,
) -> CssPixelRect {
    if !arena.paintable_row_is_populated(node) {
        return rect;
    }
    match compute_transform(arena, node, 1.0) {
        Some((transform, _is_invertible)) => map_rect_through_css_transform(transform.matrix, transform.origin, rect),
        None => rect,
    }
}

/// The axis-aligned bounding box of a rect mapped through a CSS transform about its origin.
pub(crate) fn map_rect_through_css_transform(
    matrix: FloatMatrix4x4,
    origin: FloatPoint,
    rect: CssPixelRect,
) -> CssPixelRect {
    let mapped = matrix
        .extract_2d_affine()
        .map_rect(
            FloatRect::new(
                rect.x.to_float(),
                rect.y.to_float(),
                rect.width.to_float(),
                rect.height.to_float(),
            )
            .translated(-origin.x, -origin.y),
        )
        .translated(origin.x, origin.y);
    CssPixelRect::new(
        CssPixels::nearest_value_for_f32(mapped.x),
        CssPixels::nearest_value_for_f32(mapped.y),
        CssPixels::nearest_value_for_f32(mapped.width),
        CssPixels::nearest_value_for_f32(mapped.height),
    )
}

/// The size an area is compared to the snapport in is the one it lays its content out along, so
/// that an area whose content no longer fits the snapport aligns the edge that content begins at.
/// This matches other engines.
pub(crate) fn area_is_larger_than_snapport(
    area: CssPixelRect,
    snapport: CssPixelRect,
    area_horizontal_writing_mode: bool,
) -> bool {
    if area_horizontal_writing_mode {
        area.width > snapport.width
    } else {
        area.height > snapport.height
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct WritingModeFacts {
    pub horizontal: bool,
    pub inline_axis_is_reverse: bool,
    pub block_axis_is_reverse: bool,
}

impl WritingModeFacts {
    fn of(style: ComputedValuesView<'_>) -> Self {
        Self {
            horizontal: style.writing_mode() == writing_mode::HORIZONTAL_TB,
            inline_axis_is_reverse: style.inline_axis_is_reverse(),
            block_axis_is_reverse: style.block_axis_is_reverse(),
        }
    }
}

// https://drafts.csswg.org/css-scroll-snap-1/#scroll-snap-align
// The two values specify the snapping alignment in the block axis and inline axis, respectively, as
// determined by the snap container's writing mode.
pub(crate) fn physical_snap_alignment(
    block_alignment: u8,
    inline_alignment: u8,
    writing_mode: WritingModeFacts,
) -> (u8, u8) {
    // start and end name the edges an axis begins and ends at, which are its lesser and greater
    // physical edges only while the axis runs in the same direction as the physical one.
    let alignment_along_axis = |axis_alignment: u8, axis_is_reverse: bool| {
        if !axis_is_reverse {
            return axis_alignment;
        }
        match axis_alignment {
            scroll_snap_align::START => scroll_snap_align::END,
            scroll_snap_align::END => scroll_snap_align::START,
            other => other,
        }
    };

    let (x_alignment, y_alignment, x_axis_is_reverse, y_axis_is_reverse) = if writing_mode.horizontal {
        (
            inline_alignment,
            block_alignment,
            writing_mode.inline_axis_is_reverse,
            writing_mode.block_axis_is_reverse,
        )
    } else {
        (
            block_alignment,
            inline_alignment,
            writing_mode.block_axis_is_reverse,
            writing_mode.inline_axis_is_reverse,
        )
    };
    (
        alignment_along_axis(x_alignment, x_axis_is_reverse),
        alignment_along_axis(y_alignment, y_axis_is_reverse),
    )
}

#[cfg(test)]
mod tests {
    use super::*;
    use libgfx_rust::{scale_matrix, translation_matrix};

    fn px(value: i32) -> CssPixels {
        CssPixels::from_integer(i64::from(value))
    }

    fn rect(x: i32, y: i32, width: i32, height: i32) -> CssPixelRect {
        CssPixelRect::new(px(x), px(y), px(width), px(height))
    }

    const HORIZONTAL_LTR: WritingModeFacts = WritingModeFacts {
        horizontal: true,
        inline_axis_is_reverse: false,
        block_axis_is_reverse: false,
    };

    #[test]
    fn alignment_follows_the_physical_axes_of_the_writing_mode() {
        let (start, end) = (scroll_snap_align::START, scroll_snap_align::END);

        assert_eq!(physical_snap_alignment(start, end, HORIZONTAL_LTR), (end, start));

        let horizontal_rtl = WritingModeFacts {
            inline_axis_is_reverse: true,
            ..HORIZONTAL_LTR
        };
        assert_eq!(physical_snap_alignment(start, start, horizontal_rtl), (end, start));

        let vertical_rl = WritingModeFacts {
            horizontal: false,
            inline_axis_is_reverse: false,
            block_axis_is_reverse: true,
        };
        assert_eq!(physical_snap_alignment(start, start, vertical_rl), (end, start));

        let center = scroll_snap_align::CENTER;
        assert_eq!(physical_snap_alignment(center, center, vertical_rl), (center, center));
    }

    #[test]
    fn an_area_is_compared_to_the_snapport_along_its_own_inline_axis() {
        let snapport = rect(0, 0, 200, 200);
        assert!(area_is_larger_than_snapport(rect(0, 0, 300, 100), snapport, true));
        assert!(!area_is_larger_than_snapport(rect(0, 0, 100, 300), snapport, true));
        assert!(area_is_larger_than_snapport(rect(0, 0, 100, 300), snapport, false));
    }

    #[test]
    fn a_rect_is_mapped_through_a_transform_about_its_origin() {
        let translated = map_rect_through_css_transform(
            translation_matrix(0.0, -60.0, 0.0),
            FloatPoint { x: 100.0, y: 200.0 },
            rect(0, 100, 200, 200),
        );
        assert_eq!(translated, rect(0, 40, 200, 200));

        let scaled = map_rect_through_css_transform(
            scale_matrix(0.5, 0.5, 1.0),
            FloatPoint { x: 100.0, y: 200.0 },
            rect(0, 100, 200, 200),
        );
        assert_eq!(scaled, rect(50, 150, 100, 100));
    }
}
