/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::css::css_pixels::{CssPixelRect, CssPixels};
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::painting::node_painting;
use crate::painting::paintable_geometry;
use crate::painting::paintable_rows::PaintableRowsRead;
use crate::painting::style_queries;
use libgfx_rust::matrix::multiply_affine;
use libgfx_rust::{AffineTransform, FloatRect, MaskKind};

pub(crate) fn first_child_paintable_of_kind(
    arena: &impl PaintableRowsRead,
    paintable: NodeSlotId,
    kind: NodeKind,
) -> Option<NodeSlotId> {
    let mut child = arena.node_first_child_if_live(paintable);
    while let Some(node) = child {
        if arena.node_kind_if_live(node) == Some(kind) {
            return arena.paintable_row_is_populated(node).then_some(node);
        }
        child = arena.node_next_sibling_if_live(node);
    }
    None
}

/// The object bounding box covers the target's geometry alone. The paintable's border box is not
/// that: SVG layout inflates it by the visible stroke width. So, take the bounding box from the
/// target's geometry path, which carries no stroke. A group or a foreign object has no single
/// geometry path, and we have no object bounding box for it, so its border box still stands in.
pub(crate) fn target_user_space_object_bounding_box(
    arena: &impl PaintableRowsRead,
    target: NodeSlotId,
) -> CssPixelRect {
    if arena.node_kind_if_live(target).is_some_and(node_painting::is_svg_path)
        && let Some(path) = paintable_geometry::committed_svg_path(arena, target)
    {
        let [x, y, width, height] = path.bounding_box();
        return CssPixelRect::new(
            CssPixels::nearest_value_for_f32(x),
            CssPixels::nearest_value_for_f32(y),
            CssPixels::nearest_value_for_f32(width),
            CssPixels::nearest_value_for_f32(height),
        );
    }
    paintable_geometry::absolute_border_box_rect(arena, target)
}

pub(crate) fn object_bounding_box_content_units_transform(
    arena: &impl PaintableRowsRead,
    target: NodeSlotId,
) -> AffineTransform {
    let bounding_box = target_user_space_object_bounding_box(arena, target);
    AffineTransform {
        values: [
            bounding_box.width.to_float(),
            0.0,
            0.0,
            bounding_box.height.to_float(),
            bounding_box.x.to_float(),
            bounding_box.y.to_float(),
        ],
    }
}

fn css_pixel_rect_from_float(rect: FloatRect) -> CssPixelRect {
    CssPixelRect::new(
        CssPixels::nearest_value_for_f32(rect.x),
        CssPixels::nearest_value_for_f32(rect.y),
        CssPixels::nearest_value_for_f32(rect.width),
        CssPixels::nearest_value_for_f32(rect.height),
    )
}

/// The masking area of the mask that `target` references, in the target's user space. Percentages
/// in a userSpaceOnUse masking area resolve against the SVG viewport. An empty area conveys that
/// a negative value or a value of zero disables rendering of the element; a mask layer with an
/// empty area clips the target away entirely.
pub(crate) fn mask_area(arena: &impl PaintableRowsRead, target: NodeSlotId) -> Option<CssPixelRect> {
    if !arena
        .node_kind_if_live(target)
        .is_some_and(node_painting::supports_svg_masking)
    {
        return None;
    }
    let mask_box = first_child_paintable_of_kind(arena, target, NodeKind::SVGMaskBox)?;
    let facts = paintable_geometry::committed_svg_mask_area_facts(arena, mask_box)?;
    let viewport_size =
        crate::painting::svg_viewport::nearest_svg_viewport_user_rect(arena, mask_box).map_or((0.0, 0.0), |rect| {
            (
                CssPixels::nearest_value_for_f32(rect.width).to_float(),
                CssPixels::nearest_value_for_f32(rect.height).to_float(),
            )
        });
    let target_bounding_box = target_user_space_object_bounding_box(arena, target);
    let masking_area = if facts.units_are_object_bounding_box {
        let target_x = target_bounding_box.x.to_float();
        let target_y = target_bounding_box.y.to_float();
        let target_width = target_bounding_box.width.to_float();
        let target_height = target_bounding_box.height.to_float();
        FloatRect::new(
            target_x + (facts.x.value * target_width),
            target_y + (facts.y.value * target_height),
            facts.width.value * target_width,
            facts.height.value * target_height,
        )
    } else {
        let user_space_masking_area = FloatRect::new(
            facts.x.resolve_relative_to(viewport_size.0),
            facts.y.resolve_relative_to(viewport_size.1),
            facts.width.resolve_relative_to(viewport_size.0),
            facts.height.resolve_relative_to(viewport_size.1),
        );
        if user_space_masking_area.is_empty() {
            return Some(CssPixelRect::default());
        }
        user_space_masking_area
    };
    if masking_area.is_empty() {
        return Some(CssPixelRect::default());
    }
    Some(css_pixel_rect_from_float(masking_area))
}

fn committed_svg_element_transform_or_identity(arena: &impl PaintableRowsRead, slot: NodeSlotId) -> AffineTransform {
    paintable_geometry::committed_svg_element_transform(arena, slot).map_or_else(AffineTransform::identity, Into::into)
}

/// https://drafts.csswg.org/css-masking-1/#ClipPathElement
/// If a child element is made invisible by display or visibility it does not contribute to the
/// clipping path.
fn contributes_to_clip_path(arena: &impl PaintableRowsRead, node: NodeSlotId) -> bool {
    arena.node_style_if_live(node).is_some_and(|style| {
        style.visibility() == crate::css::css_enums::visibility::VISIBLE && !style.display().is_none()
    })
}

struct ClipPathBoundingBox {
    min_x: CssPixels,
    min_y: CssPixels,
    max_x: CssPixels,
    max_y: CssPixels,
    has_points: bool,
}

impl ClipPathBoundingBox {
    fn add_point(&mut self, x: CssPixels, y: CssPixels) {
        if !self.has_points {
            self.min_x = x;
            self.min_y = y;
            self.max_x = x;
            self.max_y = y;
            self.has_points = true;
            return;
        }
        self.min_x = self.min_x.min(x);
        self.min_y = self.min_y.min(y);
        self.max_x = self.max_x.max(x);
        self.max_y = self.max_y.max(y);
    }
}

/// https://drafts.csswg.org/css-masking-1/#ClipPathElement
/// When the clipPath element contains multiple child elements, the silhouettes of the child
/// elements are logically OR'd together to create a single silhouette which is then used to
/// restrict the region onto which paint can be applied.
fn svg_clip_path_geometry_bounds(
    arena: &impl PaintableRowsRead,
    node: NodeSlotId,
    additional_transform: AffineTransform,
) -> Option<CssPixelRect> {
    if !contributes_to_clip_path(arena, node) {
        return None;
    }
    let kind = arena.node_kind_if_live(node)?;
    if node_painting::is_svg_path(kind) {
        let path = paintable_geometry::committed_svg_path(arena, node)?;
        let [x, y, width, height] = path.copy_transformed(additional_transform.values).bounding_box();
        return Some(css_pixel_rect_from_float(FloatRect::new(x, y, width, height)));
    }
    let mut bounding_box = ClipPathBoundingBox {
        min_x: CssPixels::default(),
        min_y: CssPixels::default(),
        max_x: CssPixels::default(),
        max_y: CssPixels::default(),
        has_points: false,
    };
    let mut child = arena.node_first_child_if_live(node);
    while let Some(child_node) = child {
        child = arena.node_next_sibling_if_live(child_node);
        let Some(child_kind) = arena.node_kind_if_live(child_node) else {
            continue;
        };
        if matches!(
            child_kind,
            NodeKind::SVGMaskBox | NodeKind::SVGClipBox | NodeKind::SVGPatternBox
        ) {
            continue;
        }
        if !arena.paintable_row_is_populated(child_node) || !node_painting::is_svg_paintable(child_kind) {
            continue;
        }
        let child_transform = multiply_affine(
            additional_transform,
            committed_svg_element_transform_or_identity(arena, child_node),
        );
        let Some(child_bounds) = svg_clip_path_geometry_bounds(arena, child_node, child_transform) else {
            continue;
        };
        bounding_box.add_point(child_bounds.x, child_bounds.y);
        bounding_box.add_point(child_bounds.right(), child_bounds.bottom());
    }
    if !bounding_box.has_points {
        return None;
    }
    Some(CssPixelRect::new(
        bounding_box.min_x,
        bounding_box.min_y,
        bounding_box.max_x - bounding_box.min_x,
        bounding_box.max_y - bounding_box.min_y,
    ))
}

// An empty clipping path completely clips away the element that had the clip-path property applied.
pub(crate) fn clip_area(arena: &impl PaintableRowsRead, target: NodeSlotId) -> Option<CssPixelRect> {
    if !arena
        .node_kind_if_live(target)
        .is_some_and(node_painting::supports_svg_masking)
    {
        return None;
    }
    let clip_box = first_child_paintable_of_kind(arena, target, NodeKind::SVGClipBox)?;
    let mut clip_path_transform = committed_svg_element_transform_or_identity(arena, clip_box);
    if paintable_geometry::committed_svg_resource_content_units_are_object_bounding_box(arena, clip_box) {
        clip_path_transform = multiply_affine(
            object_bounding_box_content_units_transform(arena, target),
            clip_path_transform,
        );
    }
    Some(svg_clip_path_geometry_bounds(arena, clip_box, clip_path_transform).unwrap_or_default())
}

pub(crate) fn mask_kind(arena: &impl PaintableRowsRead, target: NodeSlotId) -> MaskKind {
    use crate::css::css_enums::keyword;
    let Some(mask_box) = first_child_paintable_of_kind(arena, target, NodeKind::SVGMaskBox) else {
        return MaskKind::Alpha;
    };
    let Some(style) = arena.node_style_if_live(mask_box) else {
        return MaskKind::Alpha;
    };
    match style_queries::handle_value(&style.mask().mask_type) {
        Some(crate::css::style_value::StyleValueData::Keyword { keyword }) if *keyword == keyword::LUMINANCE => {
            MaskKind::Luminance
        }
        _ => MaskKind::Alpha,
    }
}
