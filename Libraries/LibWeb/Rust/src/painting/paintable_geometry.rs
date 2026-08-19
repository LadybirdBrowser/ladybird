/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::CssPixels;
use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixelSize};
use crate::painting::paintable_arena::PaintableArena;
use crate::painting::paintable_data::*;

pub(crate) fn is_svg_paintable(kind: PaintableKind) -> bool {
    matches!(
        kind,
        PaintableKind::SVGGraphicsPaintable
            | PaintableKind::SVGPathPaintable
            | PaintableKind::SVGImagePaintable
            | PaintableKind::SVGMaskPaintable
            | PaintableKind::SVGClipPaintable
            | PaintableKind::SVGPatternPaintable
    )
}

pub fn content_size(data: &PaintableData) -> CssPixelSize {
    data.content_size.into()
}

pub fn absolute_rect(arena: &PaintableArena, slot: PaintableSlotId) -> CssPixelRect {
    if let Some(rect) = arena.memoized_absolute_rect(slot) {
        return rect;
    }
    let data = arena.data_ref(slot);
    let mut rect = CssPixelRect::from_location_and_size(data.offset.into(), data.content_size.into());
    let mut block = data.containing_block;
    while !block.is_invalid() && arena.is_live(block) {
        let block_data = arena.data_ref(block);
        if block_data.kind == PaintableKind::SVGSVGPaintable || is_svg_paintable(block_data.kind) {
            break;
        }
        rect = rect.translated_by(block_data.offset.into());
        if block_data.kind == PaintableKind::SVGForeignObjectPaintable {
            break;
        }
        block = block_data.containing_block;
    }
    arena.memoize_absolute_rect(slot, rect);
    rect
}

pub fn absolute_position(arena: &PaintableArena, slot: PaintableSlotId) -> CssPixelPoint {
    absolute_rect(arena, slot).location()
}

pub fn absolute_padding_box_rect(arena: &PaintableArena, slot: PaintableSlotId) -> CssPixelRect {
    let data = arena.data_ref(slot);
    let absolute = absolute_rect(arena, slot);
    if data.kind == PaintableKind::InlinePaintable {
        return CssPixelRect::from(data.local_padding_box_union).translated_by(absolute.location());
    }
    let padding = data.padding;
    CssPixelRect::new(
        absolute.x - padding.left,
        absolute.y - padding.top,
        data.content_size.width + padding.left + padding.right,
        data.content_size.height + padding.top + padding.bottom,
    )
}

pub fn absolute_border_box_rect(arena: &PaintableArena, slot: PaintableSlotId) -> CssPixelRect {
    let data = arena.data_ref(slot);
    if data.kind == PaintableKind::InlinePaintable {
        return CssPixelRect::from(data.local_border_box_union).translated_by(absolute_rect(arena, slot).location());
    }
    let padded = absolute_padding_box_rect(arena, slot);
    let mut border_top = data.border.top;
    let mut border_bottom = data.border.bottom;
    let mut border_left = data.border.left;
    let mut border_right = data.border.right;
    if data.uses_collapsing_borders_model {
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

pub fn scrollable_overflow_rect(arena: &PaintableArena, slot: PaintableSlotId) -> Option<CssPixelRect> {
    let data = arena.data_ref(slot);
    if data.has_overflow {
        return Some(data.overflow.rect.into());
    }
    if !data.has_cached_overflow {
        return None;
    }
    Some(CssPixelRect::from(data.cached_overflow.rect).translated_by(absolute_padding_box_rect(arena, slot).location()))
}

pub fn has_scrollable_overflow(data: &PaintableData) -> bool {
    if data.has_overflow {
        return data.overflow.has_scrollable_overflow;
    }
    data.has_cached_overflow && data.cached_overflow.has_scrollable_overflow
}
