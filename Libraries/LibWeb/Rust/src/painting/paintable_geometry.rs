/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::geometry::Frame;
use crate::layout::node_data::NodeSlotId;
use crate::layout::{
    formatting_context, grid_formatting_context, svg_formatting_context, table_formatting_context, used_values,
};
use crate::painting::node_painting;
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_data::*;

/// The frame the box was laid out in, in which its committed fragment holds its geometry.
pub(crate) fn committed_frame(arena: &impl GeometryRead, slot: NodeSlotId) -> Frame {
    arena.paintable_data(slot).frame()
}

/// The physical rect of a box's content box within its containing block's content box, from the rect its fragment
/// link holds in the frame the box was laid out in.
pub(crate) fn physical_content_rect_in_containing_block(
    arena: &impl GeometryRead,
    frame: Frame,
    containing_block: NodeSlotId,
    rect_in_frame: CssPixelRect,
) -> CssPixelRect {
    if frame.is_physical() {
        return rect_in_frame;
    }
    let containing_block_size = if containing_block.is_invalid() || !arena.paintable_row_is_populated(containing_block)
    {
        used_values::FfiCssPixelSize::default()
    } else {
        committed_content_size(arena, containing_block)
    };
    frame.rect_to_physical(rect_in_frame, containing_block_size)
}

fn link_content_rect(link: &crate::layout::fragment_tree::FragmentLink) -> CssPixelRect {
    CssPixelRect::new(
        link.committed_offset.x,
        link.committed_offset.y,
        link.fragment.content_inline_size,
        link.fragment.content_block_size,
    )
}

pub(crate) fn committed_offset(arena: &impl GeometryRead, slot: NodeSlotId) -> used_values::FfiCssPixelPoint {
    let data = arena.paintable_data(slot);
    if node_painting::is_fragmented_inline(arena, slot) {
        return data.offset;
    }
    let frame = data.frame();
    if !frame.is_physical() {
        let containing_block = data.containing_block;
        return arena
            .with_committed_fragment_link(slot, |link| link.map(link_content_rect))
            .map_or_else(used_values::FfiCssPixelPoint::default, |rect| {
                physical_content_rect_in_containing_block(arena, frame, containing_block, rect)
                    .location()
                    .into()
            });
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(used_values::FfiCssPixelPoint::default, |link| link.committed_offset)
    })
}

pub(crate) fn committed_content_size(arena: &impl GeometryRead, slot: NodeSlotId) -> used_values::FfiCssPixelSize {
    let data = arena.paintable_data(slot);
    if node_painting::is_fragmented_inline(arena, slot) {
        return data.content_size;
    }
    let frame = data.frame();
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(used_values::FfiCssPixelSize::default, |link| {
            frame.size_to_physical(link.fragment.content_inline_size, link.fragment.content_block_size)
        })
    })
}

pub(crate) fn sides_to_physical(frame: Frame, sides: FfiPixelBox) -> FfiPixelBox {
    if frame.is_physical() {
        return sides;
    }
    let [top, right, bottom, left] =
        frame.sides_in([sides.top, sides.right, sides.bottom, sides.left], Frame::PHYSICAL);
    FfiPixelBox {
        top,
        right,
        bottom,
        left,
    }
}

pub(crate) fn committed_margin(arena: &impl GeometryRead, slot: NodeSlotId) -> FfiPixelBox {
    let frame = committed_frame(arena, slot);
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| {
            sides_to_physical(
                frame,
                FfiPixelBox {
                    top: link.fragment.margin_top,
                    right: link.fragment.margin_right,
                    bottom: link.fragment.margin_bottom,
                    left: link.fragment.margin_left,
                },
            )
        })
    })
}

fn border_of(fragment: &crate::layout::fragment_tree::Fragment, frame: Frame) -> FfiPixelBox {
    sides_to_physical(
        frame,
        FfiPixelBox {
            top: fragment.border_top,
            right: fragment.border_right,
            bottom: fragment.border_bottom,
            left: fragment.border_left,
        },
    )
}

fn padding_of(fragment: &crate::layout::fragment_tree::Fragment, frame: Frame) -> FfiPixelBox {
    sides_to_physical(
        frame,
        FfiPixelBox {
            top: fragment.padding_top,
            right: fragment.padding_right,
            bottom: fragment.padding_bottom,
            left: fragment.padding_left,
        },
    )
}

pub(crate) fn committed_border(arena: &impl GeometryRead, slot: NodeSlotId) -> FfiPixelBox {
    let frame = committed_frame(arena, slot);
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| border_of(&link.fragment, frame))
    })
}

pub(crate) fn committed_padding(arena: &impl GeometryRead, slot: NodeSlotId) -> FfiPixelBox {
    let frame = committed_frame(arena, slot);
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| padding_of(&link.fragment, frame))
    })
}

pub(crate) fn committed_inset(arena: &impl GeometryRead, slot: NodeSlotId) -> FfiPixelBox {
    let frame = committed_frame(arena, slot);
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| {
            sides_to_physical(
                frame,
                FfiPixelBox {
                    top: link.inset_top,
                    right: link.inset_right,
                    bottom: link.inset_bottom,
                    left: link.inset_left,
                },
            )
        })
    })
}

pub(crate) fn committed_uses_collapsing_borders_model(arena: &impl GeometryRead, slot: NodeSlotId) -> bool {
    arena.with_committed_fragment_link(slot, |link| {
        link.is_some_and(|link| link.fragment.uses_collapsing_borders_model)
    })
}

/// For a table cell: whether every column it spans has 'visibility: collapse', which removes the cell from the display
/// along with the columns (CSS 2.2 §17.5.5).
pub(crate) fn committed_hidden_by_collapsed_columns(arena: &impl GeometryRead, slot: NodeSlotId) -> bool {
    arena.with_committed_fragment_link(slot, |link| {
        link.is_some_and(|link| link.fragment.hidden_by_collapsed_columns)
    })
}

/// For a table cell or a table-column(-group) box: the first grid column it occupies and the number of grid columns
/// it spans.
pub(crate) fn committed_table_column_range(arena: &impl GeometryRead, slot: NodeSlotId) -> (u32, u32) {
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or((0, 0), |link| {
            (link.fragment.table_column_index, link.fragment.table_column_span)
        })
    })
}

pub(crate) fn committed_grid_layout_data(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<std::sync::Arc<grid_formatting_context::GridLayoutData>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.grid_layout_data.clone())
    })
}

pub(crate) fn committed_multicol_layout_data(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<std::sync::Arc<crate::layout::fragmentation::MulticolLayoutData>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.multicol_layout_data.clone())
    })
}

pub(crate) fn committed_flex_layout_data(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<std::sync::Arc<formatting_context::FlexLayoutData>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.flex_layout_data.clone())
    })
}

pub(crate) fn committed_used_grid_tracks(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<std::sync::Arc<grid_formatting_context::OwnedUsedGridTracks>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.used_grid_tracks.clone())
    })
}

pub(crate) fn committed_collapsed_table_borders(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<std::sync::Arc<table_formatting_context::OwnedCollapsedTableBorders>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.collapsed_table_borders.clone())
    })
}

pub(crate) fn committed_svg_path(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<std::sync::Arc<libgfx_rust::path::Path>> {
    if !arena.node_kind_if_live(slot).is_some_and(node_painting::is_svg_path) {
        return None;
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.computed_svg_path.clone())
    })
}

/// The glyph cell of every code unit of a text content element's own character data, as its box
/// committed them; None for any other box, and for a text box that has not been committed.
pub(crate) fn committed_svg_text_character_cells(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<std::sync::Arc<Vec<svg_formatting_context::FfiSvgTextCharacterCell>>> {
    if !matches!(
        arena.node_kind_if_live(slot),
        Some(crate::layout::node_data::NodeKind::SVGTextBox | crate::layout::node_data::NodeKind::SVGTextPathBox)
    ) {
        return None;
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg_text_character_cells.clone())
    })
}

pub(crate) fn committed_containing_line_box_index(arena: &impl GeometryRead, slot: NodeSlotId) -> Option<usize> {
    arena.with_committed_fragment_link(slot, |link| link.and_then(|link| link.containing_line_box_index))
}

pub(crate) fn committed_svg_viewport_transform(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<svg_formatting_context::FfiAffineTransform> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg.as_ref()?.viewport_transform)
    })
}

pub(crate) fn committed_svg_viewport_size(arena: &impl GeometryRead, slot: NodeSlotId) -> used_values::FfiCssPixelSize {
    if arena.node_kind_if_live(slot) != Some(crate::layout::node_data::NodeKind::SVGSVGBox) {
        return used_values::FfiCssPixelSize::default();
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg.as_ref()?.viewport_size)
            .unwrap_or_default()
    })
}

pub(crate) fn committed_svg_element_transform(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<svg_formatting_context::FfiAffineTransform> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg.as_ref()?.element_transform)
    })
}

pub(crate) fn committed_svg_additional_element_transform(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<svg_formatting_context::FfiAffineTransform> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg.as_ref()?.additional_element_transform)
    })
}

pub(crate) fn committed_svg_mask_area_facts(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<svg_formatting_context::SvgMaskAreaFacts> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg.as_ref()?.mask_area_facts)
    })
}

pub(crate) fn committed_svg_view_box(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> Option<svg_formatting_context::FfiSvgViewBox> {
    arena.with_committed_fragment_link(slot, |link| link.and_then(|link| link.fragment.svg.as_ref()?.view_box))
}

pub(crate) fn committed_svg_resource_content_units_are_object_bounding_box(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> bool {
    arena.with_committed_fragment_link(slot, |link| {
        link.is_some_and(|link| {
            link.fragment
                .svg
                .as_ref()
                .is_some_and(|svg| svg.resource_content_units_are_object_bounding_box)
        })
    })
}

pub(crate) fn committed_svg_viewport_percentage_basis(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixels {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg.as_ref())
            .map_or_else(CssPixels::default, |svg| svg.viewport_percentage_basis)
    })
}

pub(crate) fn absolute_rect_or_default(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixelRect {
    if !arena.paintable_row_is_populated(slot) {
        return CssPixelRect::default();
    }
    absolute_rect(arena, slot)
}

pub(crate) fn absolute_rect(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixelRect {
    if let Some(rect) = arena.memoized_absolute_rect(slot) {
        return rect;
    }
    let data = arena.paintable_data(slot);
    let mut rect = CssPixelRect::from_location_and_size(
        committed_offset(arena, slot).into(),
        committed_content_size(arena, slot).into(),
    );
    if arena.node_kind_if_live(slot).is_some_and(node_painting::is_svg) {
        arena.memoize_absolute_rect(slot, rect);
        return rect;
    }
    let mut block = data.containing_block;
    let mut block_fragment = containing_block_fragment(arena, slot);
    while !block.is_invalid() && arena.paintable_row_is_populated(block) {
        if block_fragment != 0 {
            rect = rect.translated_by(absolute_fragment_rect(arena, block, block_fragment as usize).location());
            break;
        }
        let block_data = arena.paintable_data(block);
        let block_kind = arena.node_kind_if_live(block);
        if block_kind == Some(crate::layout::node_data::NodeKind::SVGSVGBox)
            || block_kind.is_some_and(node_painting::is_svg)
        {
            break;
        }
        if block_kind == Some(crate::layout::node_data::NodeKind::SVGForeignObjectBox) {
            rect = rect.translated_by(committed_offset(arena, block).into());
            break;
        }
        // A containing block whose rect this geometry already memoized carries its own containing blocks.
        if let Some(block_rect) = arena.memoized_absolute_rect(block) {
            rect = rect.translated_by(block_rect.location());
            break;
        }
        rect = rect.translated_by(committed_offset(arena, block).into());
        block_fragment = containing_block_fragment(arena, block);
        block = block_data.containing_block;
    }
    arena.memoize_absolute_rect(slot, rect);
    rect
}

fn containing_block_fragment(arena: &impl GeometryRead, slot: NodeSlotId) -> u32 {
    if !arena
        .paintable_data(slot)
        .has_flag(crate::painting::paintable_data::PaintableFlag::InLaterContainingBlockFragment)
    {
        return 0;
    }
    arena.committed_side_data(slot).containing_block_fragment
}

pub(crate) fn fragment_count(arena: &impl GeometryRead, slot: NodeSlotId) -> usize {
    if !arena
        .paintable_data(slot)
        .has_flag(crate::painting::paintable_data::PaintableFlag::HasLaterBoxFragments)
    {
        return 1;
    }
    1 + arena.committed_side_data(slot).box_fragments().len()
}

pub(crate) fn absolute_fragment_rect(arena: &impl GeometryRead, slot: NodeSlotId, index: usize) -> CssPixelRect {
    if index == 0 {
        return absolute_rect(arena, slot);
    }
    let Some((rect, containing_block_fragment)) = with_later_box_fragment(arena, slot, index, |fragment| {
        let link = &fragment.link;
        (
            CssPixelRect::new(
                link.committed_offset.x,
                link.committed_offset.y,
                link.fragment.content_inline_size,
                link.fragment.content_block_size,
            ),
            fragment.containing_block_fragment,
        )
    }) else {
        return absolute_rect(arena, slot);
    };
    let block = arena.paintable_data(slot).containing_block;
    if block.is_invalid() || !arena.paintable_row_is_populated(block) {
        return rect;
    }
    rect.translated_by(absolute_fragment_rect(arena, block, containing_block_fragment as usize).location())
}

fn with_later_box_fragment<R>(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
    index: usize,
    read: impl FnOnce(&CommittedBoxFragment) -> R,
) -> Option<R> {
    arena.committed_side_data(slot).box_fragments().get(index - 1).map(read)
}

pub(crate) fn unbroken_content_size(arena: &impl GeometryRead, slot: NodeSlotId) -> used_values::FfiCssPixelSize {
    let mut size = committed_content_size(arena, slot);
    for fragment in arena.committed_side_data(slot).box_fragments() {
        size.height += fragment.link.fragment.content_block_size;
    }
    size
}

pub(crate) fn unbroken_box_model(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> (FfiPixelBox, FfiPixelBox, FfiPixelBox) {
    let (mut margin, mut padding, mut border) = (
        committed_margin(arena, slot),
        committed_padding(arena, slot),
        committed_border(arena, slot),
    );
    if let Some(last) = arena.committed_side_data(slot).box_fragments().last() {
        let fragment = &last.link.fragment;
        margin.bottom = fragment.margin_bottom;
        padding.bottom = fragment.padding_bottom;
        border.bottom = fragment.border_bottom;
    }
    (margin, padding, border)
}

pub(crate) fn unbroken_border_box_size(arena: &impl GeometryRead, slot: NodeSlotId) -> used_values::FfiCssPixelSize {
    let first_fragment = first_fragment_border_box_rect(arena, slot);
    if fragment_count(arena, slot) == 1 {
        return used_values::FfiCssPixelSize {
            width: first_fragment.width,
            height: first_fragment.height,
        };
    }
    let content = unbroken_content_size(arena, slot);
    let (_, padding, border) = unbroken_box_model(arena, slot);
    used_values::FfiCssPixelSize {
        width: first_fragment.width,
        height: content.height + padding.top + padding.bottom + border.top + border.bottom,
    }
}

pub(crate) fn committed_fragment_padding(arena: &impl GeometryRead, slot: NodeSlotId, index: usize) -> FfiPixelBox {
    if index == 0 {
        return committed_padding(arena, slot);
    }
    with_later_box_fragment(arena, slot, index, |fragment| {
        padding_of(&fragment.link.fragment, committed_frame(arena, slot))
    })
    .unwrap_or_default()
}

pub(crate) fn committed_fragment_border(arena: &impl GeometryRead, slot: NodeSlotId, index: usize) -> FfiPixelBox {
    if index == 0 {
        return committed_border_box_edges(arena, slot);
    }
    with_later_box_fragment(arena, slot, index, |fragment| {
        border_of(&fragment.link.fragment, committed_frame(arena, slot))
    })
    .unwrap_or_default()
}

pub(crate) fn committed_fragment_present_edges(arena: &impl GeometryRead, slot: NodeSlotId, index: usize) -> u8 {
    let edges_of = |fragment: &crate::layout::fragment_tree::Fragment| {
        let mut edges = PIECE_EDGE_LEFT | PIECE_EDGE_RIGHT;
        if !fragment.continues_from_previous_fragment {
            edges |= PIECE_EDGE_TOP;
        }
        if !fragment.continues_in_next_fragment {
            edges |= PIECE_EDGE_BOTTOM;
        }
        edges
    };
    if index == 0 {
        return arena.with_committed_fragment_link(slot, |link| {
            link.map_or(crate::painting::record::paint::border::ALL_PIECE_EDGES, |link| {
                edges_of(&link.fragment)
            })
        });
    }
    with_later_box_fragment(arena, slot, index, |fragment| edges_of(&fragment.link.fragment)).unwrap_or(0)
}

pub(crate) fn absolute_fragment_padding_box_rect(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
    index: usize,
) -> CssPixelRect {
    if index == 0 {
        return first_fragment_padding_box_rect(arena, slot);
    }
    let padding = committed_fragment_padding(arena, slot, index);
    absolute_fragment_rect(arena, slot, index).inflated(padding.top, padding.right, padding.bottom, padding.left)
}

pub(crate) fn absolute_fragment_border_box_rect(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
    index: usize,
) -> CssPixelRect {
    if index == 0 {
        return first_fragment_border_box_rect(arena, slot);
    }
    let border = committed_fragment_border(arena, slot, index);
    absolute_fragment_padding_box_rect(arena, slot, index).inflated(
        border.top,
        border.right,
        border.bottom,
        border.left,
    )
}

#[derive(Clone)]
pub(crate) struct LineRootPositions(smallvec::SmallVec<[CssPixelPoint; 1]>);

impl LineRootPositions {
    pub(crate) fn new(arena: &impl GeometryRead, root: NodeSlotId) -> Self {
        Self(
            (0..fragment_count(arena, root))
                .map(|index| absolute_fragment_rect(arena, root, index).location())
                .collect(),
        )
    }

    pub(crate) fn of_fragment(&self, box_fragment: u32) -> CssPixelPoint {
        self.0.get(box_fragment as usize).copied().unwrap_or(self.0[0])
    }

    pub(crate) fn of_line(
        &self,
        lines: &[crate::layout::inline_content::LineRecord],
        line_index: usize,
    ) -> CssPixelPoint {
        if self.0.len() == 1 {
            return self.0[0];
        }
        self.of_fragment(box_fragment_of_line(lines, line_index))
    }
}

fn box_fragment_of_line(lines: &[crate::layout::inline_content::LineRecord], line_index: usize) -> u32 {
    lines.get(line_index).map_or(0, |line| line.box_fragment)
}

pub(crate) fn absolute_line_root_position(
    arena: &impl GeometryRead,
    root: NodeSlotId,
    line_index: usize,
) -> CssPixelPoint {
    let box_fragment = box_fragment_of_line(arena.committed_side_data(root).lines(), line_index);
    absolute_fragment_rect(arena, root, box_fragment as usize).location()
}

pub(crate) fn absolute_position(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixelPoint {
    absolute_rect(arena, slot).location()
}

pub(crate) fn absolute_padding_box_rect(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixelRect {
    let mut rect = first_fragment_padding_box_rect(arena, slot);
    if node_painting::is_fragmented_inline(arena, slot) {
        return rect;
    }
    for index in 1..fragment_count(arena, slot) {
        rect.unite(absolute_fragment_padding_box_rect(arena, slot, index));
    }
    rect
}

pub(crate) fn first_fragment_padding_box_rect(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixelRect {
    let absolute = absolute_rect(arena, slot);
    if node_painting::is_fragmented_inline(arena, slot) {
        return CssPixelRect::from(arena.paintable_data(slot).local_padding_box_union)
            .translated_by(absolute.location());
    }
    let padding = committed_padding(arena, slot);
    let content_size = committed_content_size(arena, slot);
    CssPixelRect::new(
        absolute.x - padding.left,
        absolute.y - padding.top,
        content_size.width + padding.left + padding.right,
        content_size.height + padding.top + padding.bottom,
    )
}

/// The border widths that lie within the box's border box. In the collapsing borders model, a collapsed border is
/// centered on the grid line between two boxes, so only a part of it belongs to each: "the border-box of the table
/// includes half of the table border" and, for cells, "half the width of the collapsed border on each side".
/// https://www.w3.org/TR/CSS22/tables.html#collapsing-borders
/// The border is split the same way layout splits it when placing the boxes (`UsedValues::border_left_collapsed`):
/// the part before the grid line goes to the box before the line, the part after it to the box after the line.
pub(crate) fn committed_border_box_edges(arena: &impl GeometryRead, slot: NodeSlotId) -> FfiPixelBox {
    let border = committed_border(arena, slot);
    let Some(is_table_box) = arena.with_committed_fragment_link(slot, |link| {
        link.filter(|link| link.fragment.uses_collapsing_borders_model)
            .map(|link| link.fragment.is_collapsed_borders_table_box)
    }) else {
        return border;
    };
    FfiPixelBox {
        top: used_values::collapsed_border_share(border.top, true, is_table_box),
        right: used_values::collapsed_border_share(border.right, false, is_table_box),
        bottom: used_values::collapsed_border_share(border.bottom, false, is_table_box),
        left: used_values::collapsed_border_share(border.left, true, is_table_box),
    }
}

pub(crate) fn absolute_border_box_rect(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixelRect {
    let mut rect = first_fragment_border_box_rect(arena, slot);
    if node_painting::is_fragmented_inline(arena, slot) {
        return rect;
    }
    for index in 1..fragment_count(arena, slot) {
        rect.unite(absolute_fragment_border_box_rect(arena, slot, index));
    }
    rect
}

pub(crate) fn absolute_border_box_rects_of_fragments(
    arena: &impl GeometryRead,
    slot: NodeSlotId,
) -> smallvec::SmallVec<[CssPixelRect; 1]> {
    let count = fragment_count(arena, slot);
    if count == 1 || node_painting::is_fragmented_inline(arena, slot) {
        return smallvec::smallvec![absolute_border_box_rect(arena, slot)];
    }
    (0..count)
        .map(|index| absolute_fragment_border_box_rect(arena, slot, index))
        .collect()
}

pub(crate) fn first_fragment_border_box_rect(arena: &impl GeometryRead, slot: NodeSlotId) -> CssPixelRect {
    if node_painting::is_fragmented_inline(arena, slot) {
        return CssPixelRect::from(arena.paintable_data(slot).local_border_box_union)
            .translated_by(absolute_rect(arena, slot).location());
    }
    let padded = first_fragment_padding_box_rect(arena, slot);
    let border = committed_border_box_edges(arena, slot);
    CssPixelRect::new(
        padded.x - border.left,
        padded.y - border.top,
        padded.width + border.left + border.right,
        padded.height + border.top + border.bottom,
    )
}

/// The overflow the pass measured. Reading it never measures it.
pub(crate) fn scrollable_overflow_rect(arena: &impl GeometryRead, slot: NodeSlotId) -> Option<CssPixelRect> {
    if !arena.paintable_row_is_populated(slot) {
        return None;
    }
    let cache = arena.committed_side_data(slot);
    if !cache.overflow_valid_across_recommits {
        return None;
    }
    Some(
        CssPixelRect::from(cache.overflow_relative_to_padding_box.rect)
            .translated_by(first_fragment_padding_box_rect(arena, slot).location()),
    )
}

pub(crate) fn has_scrollable_overflow(arena: &impl GeometryRead, slot: NodeSlotId) -> bool {
    if !arena.paintable_row_is_populated(slot) {
        return false;
    }
    let cache = arena.committed_side_data(slot);
    cache.overflow_valid_across_recommits && cache.overflow_relative_to_padding_box.has_scrollable_overflow
}
