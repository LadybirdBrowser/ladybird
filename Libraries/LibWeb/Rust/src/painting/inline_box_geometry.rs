/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The geometry of an inline box broken into pieces on its line root's lines, which the line root's committed inline
//! content holds.

use crate::css::css_pixels::{CssPixelPoint, CssPixelRect};
use crate::layout::node_data::NodeSlotId;
use crate::painting::paint_read::GeometryRead;
use crate::painting::paintable_geometry;

/// The rects an inline box's pieces unite to, relative to the content box of its line root's fragment that holds the
/// box.
pub(crate) struct InlineBoxUnions {
    pub(crate) content: CssPixelRect,
    pub(crate) padding: CssPixelRect,
    pub(crate) border: CssPixelRect,
}

fn unite(target: &mut Option<CssPixelRect>, rect: CssPixelRect) {
    let Some(current) = target else {
        *target = Some(rect);
        return;
    };
    if rect.is_empty() {
        return;
    }
    if current.is_empty() {
        *current = rect;
    } else {
        current.unite(rect);
    }
}

/// The unions of the pieces of the inline box `inline_box`, or None where it has no line root or no pieces.
pub(crate) fn unions(arena: &impl GeometryRead, inline_box: NodeSlotId) -> Option<InlineBoxUnions> {
    let root = arena.inline_pieces_root(inline_box)?;
    let root_positions = (paintable_geometry::fragment_count(arena, root) > 1)
        .then(|| paintable_geometry::LineRootPositions::new(arena, root));
    let origin = root_positions
        .as_ref()
        .map(|positions| positions.of_fragment(arena.committed_side_data(inline_box).containing_block_fragment));
    let padding_widths = paintable_geometry::committed_padding(arena, inline_box);
    let border_widths = paintable_geometry::committed_border(arena, inline_box);
    let side = arena.committed_side_data(root);
    let mut content_union = None;
    let mut padding_union = None;
    let mut border_union = None;
    for piece_index in side.inline_box_piece_indices(inline_box) {
        let piece = side.inline_box_pieces()[*piece_index as usize];
        let root_fragment_offset =
            root_positions
                .as_ref()
                .zip(origin)
                .map_or_else(CssPixelPoint::default, |(positions, origin)| {
                    let location = positions.of_line(side.lines(), piece.line_index as usize);
                    CssPixelPoint {
                        x: location.x - origin.x,
                        y: location.y - origin.y,
                    }
                });
        let border_rect = CssPixelRect::from(piece.border_box_rect).translated_by(root_fragment_offset);
        if piece.is_geometry_only_placeholder {
            let padding_rect = border_rect.inflated(
                padding_widths.top,
                padding_widths.right,
                padding_widths.bottom,
                padding_widths.left,
            );
            unite(&mut content_union, border_rect);
            unite(
                &mut border_union,
                padding_rect.inflated(
                    border_widths.top,
                    border_widths.right,
                    border_widths.bottom,
                    border_widths.left,
                ),
            );
            unite(&mut padding_union, padding_rect);
            continue;
        }
        let padding_rect = piece.shrunken_by_present_edges(border_rect, border_widths);
        unite(
            &mut content_union,
            piece.shrunken_by_present_edges(padding_rect, padding_widths),
        );
        unite(&mut padding_union, padding_rect);
        unite(&mut border_union, border_rect);
    }
    Some(InlineBoxUnions {
        content: content_union?,
        padding: padding_union.expect("padding union set alongside content union"),
        border: border_union.expect("border union set alongside content union"),
    })
}
