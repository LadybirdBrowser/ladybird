/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::LayoutNodeArena;
use crate::layout::node_data::NodeSlotId;
use crate::layout::{
    formatting_context, grid_formatting_context, svg_formatting_context, table_formatting_context, used_values,
};
use crate::painting::node_painting;
use crate::painting::paintable_data::*;
use crate::painting::paintable_rows::PaintableRowsRead;

pub(crate) fn for_each_rendered_inline_box_piece(
    arena: &impl PaintableRowsRead,
    inline_paintable: NodeSlotId,
    mut visit: impl FnMut(u16, &InlineBoxPieceRecord, CssPixelRect),
) {
    let root = arena.paintable_data(inline_paintable).containing_block;
    if root.is_invalid() || !arena.paintable_row_is_populated(root) || !node_painting::has_lines(arena, root) {
        return;
    }
    let root_position = absolute_position(arena, root);
    let piece_indices = &arena.paintable_side_data(inline_paintable).piece_indices;
    let root_pieces = &arena.paintable_side_data(root).inline_box_pieces;
    for (position, piece_index) in piece_indices.iter().enumerate() {
        let piece = &root_pieces[*piece_index as usize];
        if piece.is_geometry_only_placeholder {
            continue;
        }
        let border_box_rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_position);
        visit(position as u16, piece, border_box_rect);
    }
}

pub(crate) fn committed_offset(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> used_values::FfiCssPixelPoint {
    let data = arena.paintable_data(slot);
    if node_painting::is_inline(arena, slot) {
        return data.offset;
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(used_values::FfiCssPixelPoint::default, |link| link.committed_offset)
    })
}

pub(crate) fn committed_content_size(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> used_values::FfiCssPixelSize {
    let data = arena.paintable_data(slot);
    if node_painting::is_inline(arena, slot) {
        return data.content_size;
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(used_values::FfiCssPixelSize::default, |link| {
            used_values::FfiCssPixelSize {
                width: link.fragment.content_inline_size,
                height: link.fragment.content_block_size,
            }
        })
    })
}

pub(crate) fn committed_margin(arena: &LayoutNodeArena, slot: NodeSlotId) -> FfiPixelBox {
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| FfiPixelBox {
            top: link.fragment.margin_top,
            right: link.fragment.margin_right,
            bottom: link.fragment.margin_bottom,
            left: link.fragment.margin_left,
        })
    })
}

pub(crate) fn committed_border(arena: &LayoutNodeArena, slot: NodeSlotId) -> FfiPixelBox {
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| FfiPixelBox {
            top: link.fragment.border_top,
            right: link.fragment.border_right,
            bottom: link.fragment.border_bottom,
            left: link.fragment.border_left,
        })
    })
}

pub(crate) fn committed_padding(arena: &LayoutNodeArena, slot: NodeSlotId) -> FfiPixelBox {
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| FfiPixelBox {
            top: link.fragment.padding_top,
            right: link.fragment.padding_right,
            bottom: link.fragment.padding_bottom,
            left: link.fragment.padding_left,
        })
    })
}

pub(crate) fn committed_inset(arena: &LayoutNodeArena, slot: NodeSlotId) -> FfiPixelBox {
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(FfiPixelBox::default, |link| FfiPixelBox {
            top: link.inset_top,
            right: link.inset_right,
            bottom: link.inset_bottom,
            left: link.inset_left,
        })
    })
}

pub(crate) fn committed_uses_collapsing_borders_model(arena: &LayoutNodeArena, slot: NodeSlotId) -> bool {
    arena.with_committed_fragment_link(slot, |link| {
        link.is_some_and(|link| link.fragment.uses_collapsing_borders_model)
    })
}

pub(crate) fn committed_grid_layout_data(
    arena: &LayoutNodeArena,
    slot: NodeSlotId,
) -> Option<std::rc::Rc<grid_formatting_context::GridLayoutData>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.grid_layout_data.clone())
    })
}

pub(crate) fn committed_flex_layout_data(
    arena: &LayoutNodeArena,
    slot: NodeSlotId,
) -> Option<std::rc::Rc<formatting_context::FlexLayoutData>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.flex_layout_data.clone())
    })
}

pub(crate) fn committed_used_grid_tracks(
    arena: &LayoutNodeArena,
    slot: NodeSlotId,
) -> Option<std::rc::Rc<grid_formatting_context::OwnedUsedGridTracks>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.used_grid_tracks.clone())
    })
}

pub(crate) fn committed_collapsed_table_borders(
    arena: &LayoutNodeArena,
    slot: NodeSlotId,
) -> Option<std::rc::Rc<table_formatting_context::OwnedCollapsedTableBorders>> {
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.collapsed_table_borders.clone())
    })
}

pub(crate) fn committed_svg_path(
    arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
) -> Option<std::rc::Rc<libgfx_rust::path::OwnedPath>> {
    if !arena.node_kind_if_live(slot).is_some_and(node_painting::is_svg_path) {
        return None;
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.computed_svg_path.clone())
    })
}

pub(crate) fn committed_containing_line_box_index(arena: &LayoutNodeArena, slot: NodeSlotId) -> Option<usize> {
    arena.with_committed_fragment_link(slot, |link| link.and_then(|link| link.containing_line_box_index))
}

pub(crate) fn committed_svg_viewport_transform(
    arena: &LayoutNodeArena,
    slot: NodeSlotId,
) -> Option<svg_formatting_context::FfiAffineTransform> {
    arena.with_committed_fragment_link(slot, |link| link.and_then(|link| link.fragment.svg_viewport_transform))
}

pub(crate) fn committed_svg_viewport_size(
    arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
) -> used_values::FfiCssPixelSize {
    if arena.node_kind_if_live(slot) != Some(crate::layout::node_data::NodeKind::SVGSVGBox) {
        return used_values::FfiCssPixelSize::default();
    }
    arena.with_committed_fragment_link(slot, |link| {
        link.and_then(|link| link.fragment.svg_viewport_size)
            .unwrap_or_default()
    })
}

pub(crate) fn committed_svg_view_box(
    arena: &impl PaintableRowsRead,
    slot: NodeSlotId,
) -> Option<svg_formatting_context::FfiSvgViewBox> {
    arena.with_committed_fragment_link(slot, |link| link.and_then(|link| link.fragment.svg_view_box))
}

pub(crate) fn committed_svg_viewport_percentage_basis(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> CssPixels {
    arena.with_committed_fragment_link(slot, |link| {
        link.map_or_else(CssPixels::default, |link| link.fragment.svg_viewport_percentage_basis)
    })
}

pub(crate) fn absolute_rect(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> CssPixelRect {
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
    while !block.is_invalid() && arena.paintable_row_is_populated(block) {
        let block_data = arena.paintable_data(block);
        let block_kind = arena.node_kind_if_live(block);
        if block_kind == Some(crate::layout::node_data::NodeKind::SVGSVGBox)
            || block_kind.is_some_and(node_painting::is_svg)
        {
            break;
        }
        rect = rect.translated_by(committed_offset(arena, block).into());
        if block_kind == Some(crate::layout::node_data::NodeKind::SVGForeignObjectBox) {
            break;
        }
        block = block_data.containing_block;
    }
    arena.memoize_absolute_rect(slot, rect);
    rect
}

pub(crate) fn absolute_position(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> CssPixelPoint {
    absolute_rect(arena, slot).location()
}

pub(crate) fn absolute_padding_box_rect(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> CssPixelRect {
    let data = arena.paintable_data(slot);
    let absolute = absolute_rect(arena, slot);
    if node_painting::is_inline(arena, slot) {
        return CssPixelRect::from(data.local_padding_box_union).translated_by(absolute.location());
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

pub(crate) fn absolute_border_box_rect(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> CssPixelRect {
    let data = arena.paintable_data(slot);
    if node_painting::is_inline(arena, slot) {
        return CssPixelRect::from(data.local_border_box_union).translated_by(absolute_rect(arena, slot).location());
    }
    let padded = absolute_padding_box_rect(arena, slot);
    let border = committed_border(arena, slot);
    let mut border_top = border.top;
    let mut border_bottom = border.bottom;
    let mut border_left = border.left;
    let mut border_right = border.right;
    if committed_uses_collapsing_borders_model(arena, slot) {
        let two = CssPixels::from_integer(2);
        border_top = border_top.div_as_fraction(two).round();
        border_bottom = border_bottom.div_as_fraction(two).round();
        border_left = border_left.div_as_fraction(two).round();
        border_right = border_right.div_as_fraction(two).round();
    }
    CssPixelRect::new(
        padded.x - border_left,
        padded.y - border_top,
        padded.width + border_left + border_right,
        padded.height + border_top + border_bottom,
    )
}

pub(crate) fn scrollable_overflow_rect(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> Option<CssPixelRect> {
    let data = arena.paintable_data(slot);
    if !data.overflow_measured_this_commit && !data.overflow_valid_across_recommits {
        return None;
    }
    Some(
        CssPixelRect::from(data.overflow_relative_to_padding_box.rect)
            .translated_by(absolute_padding_box_rect(arena, slot).location()),
    )
}

pub fn has_scrollable_overflow(data: &PaintableData) -> bool {
    (data.overflow_measured_this_commit || data.overflow_valid_across_recommits)
        && data.overflow_relative_to_padding_box.has_scrollable_overflow
}
