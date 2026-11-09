/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Maps CSS-pixel geometry between the spaces of a document's boxes and its viewport through the world transforms replay
//! draws with, so that what script measures and hit testing finds is where the content is drawn.

use crate::css::css_pixels::{CssPixelPoint, CssPixelRect, CssPixels, FfiCssPixelPoint, FfiCssPixelRect};
use crate::layout::node_data::NodeSlotId;
use crate::painting::hit_test::to_css_point;
use crate::painting::paint_read::PaintRead;
use crate::painting::style_queries;
use crate::painting::visual_context::{
    ClipBehavior, ContextRef, IncludeVisualViewportTransform, SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX,
    VisualContextTree, WorldTransforms,
};
use crate::painting::visual_context::{basic_shapes, node_values};
use libcompositing_rust::ffi::{tree_from_handle, world_transforms_from_handle};
use libgfx_rust::{FloatPoint, FloatRect};
use std::ffi::c_void;

#[repr(u8)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum ViewportSpace {
    /// What client rects and IntersectionObserver report in, which a pinch zoom leaves as it is.
    LayoutViewport,
    /// Where input lands on the screen, which a pinch zoom scales and moves.
    VisualViewport,
}

impl From<ViewportSpace> for IncludeVisualViewportTransform {
    fn from(space: ViewportSpace) -> Self {
        match space {
            ViewportSpace::LayoutViewport => Self::No,
            ViewportSpace::VisualViewport => Self::Yes,
        }
    }
}

#[derive(Clone, Copy)]
struct Transforms<'a> {
    tree: &'a VisualContextTree,
    world_transforms: &'a WorldTransforms,
    scroll_offsets: &'a [FloatPoint],
    device_pixels_per_css_pixel: f32,
}

/// Maps through a document's visual context tree, or leaves geometry as it is for a document without one.
#[derive(Clone, Copy, Default)]
pub(crate) struct ViewportMapping<'a> {
    transforms: Option<Transforms<'a>>,
}

impl<'a> ViewportMapping<'a> {
    pub(crate) fn new(
        tree: &'a VisualContextTree,
        world_transforms: &'a WorldTransforms,
        scroll_offsets: &'a [FloatPoint],
        device_pixels_per_css_pixel: f32,
    ) -> Self {
        Self {
            transforms: Some(Transforms {
                tree,
                world_transforms,
                scroll_offsets,
                device_pixels_per_css_pixel,
            }),
        }
    }

    /// Maps `rect`, in the space `node` is painted in, into the viewport.
    pub(crate) fn rect_to_viewport(
        &self,
        arena: &impl PaintRead,
        node: NodeSlotId,
        rect: CssPixelRect,
        space: ViewportSpace,
    ) -> CssPixelRect {
        if self.transforms.is_none() {
            return rect;
        }
        if !arena.paintable_row_is_populated(node) {
            return CssPixelRect::default();
        }
        let spatial = arena.paintable_data(node).accumulated_visual_context.spatial;
        self.rect_in_space_to_viewport(spatial, rect, space)
    }

    /// Maps `rect`, in the local space of `spatial`, into the viewport.
    pub(crate) fn rect_in_space_to_viewport(
        &self,
        spatial: SpatialNodeIndex,
        rect: CssPixelRect,
        space: ViewportSpace,
    ) -> CssPixelRect {
        let Some(transforms) = &self.transforms else {
            return rect;
        };
        let pixel_ratio = transforms.device_pixels_per_css_pixel;
        let device_rect = FloatRect {
            x: rect.x.to_float() * pixel_ratio,
            y: rect.y.to_float() * pixel_ratio,
            width: rect.width.to_float() * pixel_ratio,
            height: rect.height.to_float() * pixel_ratio,
        };
        let mapped = transforms
            .world_transforms
            .transform_rect_to_viewport(spatial, device_rect, space.into());
        let inverse_pixel_ratio = 1.0 / pixel_ratio;
        CssPixelRect {
            x: CssPixels::nearest_value_for_f32(mapped.x * inverse_pixel_ratio),
            y: CssPixels::nearest_value_for_f32(mapped.y * inverse_pixel_ratio),
            width: CssPixels::nearest_value_for_f32(mapped.width * inverse_pixel_ratio),
            height: CssPixels::nearest_value_for_f32(mapped.height * inverse_pixel_ratio),
        }
    }

    /// The rect `node` clips its content to, in the viewport: its overflow clip and the bounding box of its clip-path.
    pub(crate) fn content_clip_rect(
        &self,
        arena: &impl PaintRead,
        node: NodeSlotId,
        space: ViewportSpace,
    ) -> Option<CssPixelRect> {
        let overflow_clip = node_values::overflow_clip_rect(arena, node);
        let clip_path_bounds =
            basic_shapes::compute_basic_shape_clip_path_data(arena, node, 1.0).map(|(_, bounds, _)| {
                CssPixelRect::new(
                    CssPixels::from_integer(bounds.x.into()),
                    CssPixels::from_integer(bounds.y.into()),
                    CssPixels::from_integer(bounds.width.into()),
                    CssPixels::from_integer(bounds.height.into()),
                )
            });
        let clip = match (overflow_clip, clip_path_bounds) {
            (Some(overflow_clip), Some(clip_path_bounds)) => overflow_clip.intersected(clip_path_bounds),
            (overflow_clip, clip_path_bounds) => overflow_clip.or(clip_path_bounds)?,
        };
        Some(self.rect_to_viewport(arena, node, clip, space))
    }

    /// `rect`, in the viewport, clipped by the content clip of every box on the containing block chain of `node` below
    /// `ancestor`, the clip of a scroll container passed through `inflate_scroll_container_clip` first.
    pub(crate) fn visible_rect_in_ancestor(
        &self,
        arena: &impl PaintRead,
        node: NodeSlotId,
        rect: CssPixelRect,
        ancestor: NodeSlotId,
        space: ViewportSpace,
        mut inflate_scroll_container_clip: impl FnMut(CssPixelRect) -> CssPixelRect,
    ) -> CssPixelRect {
        let mut visible = rect;
        if !arena.paintable_row_is_populated(node) {
            return visible;
        }
        let mut container = arena.node_containing_block_if_live(node);
        while let Some(container_slot) =
            container.filter(|slot| *slot != ancestor && arena.paintable_row_is_populated(*slot))
        {
            if let Some(mut clip) = self.content_clip_rect(arena, container_slot, space) {
                if style_queries::is_scroll_container(arena, container_slot) {
                    clip = inflate_scroll_container_clip(clip);
                }
                visible = visible.intersected(clip);
            }
            container = arena.node_containing_block_if_live(container_slot);
        }
        visible
    }

    /// Maps `point`, in the viewport, into the local space of `context`, or gives none where a clip that
    /// `clip_behavior` respects leaves it out or the context's transforms hide it. The point stays unrounded, so that a
    /// space scaled down far, such as SVG user units under a small viewBox, keeps its precision.
    pub(crate) fn point_to_local(
        &self,
        context: ContextRef,
        point: CssPixelPoint,
        space: ViewportSpace,
        clip_behavior: ClipBehavior,
    ) -> Option<FloatPoint> {
        let Some(transforms) = &self.transforms else {
            return Some(FloatPoint {
                x: point.x.to_float(),
                y: point.y.to_float(),
            });
        };
        let pixel_ratio = transforms.device_pixels_per_css_pixel;
        let mut screen_point = FloatPoint {
            x: point.x.to_float() * pixel_ratio,
            y: point.y.to_float() * pixel_ratio,
        };
        if space == ViewportSpace::LayoutViewport {
            let visual_viewport_transform = transforms.world_transforms.local[VISUAL_VIEWPORT_NODE_INDEX.0 as usize];
            screen_point = visual_viewport_transform.extract_2d_affine().map_point(screen_point);
        }
        let local = transforms.tree.transform_point_for_hit_test(
            transforms.world_transforms,
            context,
            screen_point,
            clip_behavior,
        )?;
        Some(FloatPoint {
            x: local.x / pixel_ratio,
            y: local.y / pixel_ratio,
        })
    }

    /// The offset by which sticky positioning moves `node`, as the sum of the offsets of the sticky positioned box and
    /// its sticky positioned ancestors.
    pub(crate) fn sticky_offset(&self, arena: &impl PaintRead, node: NodeSlotId) -> CssPixelPoint {
        self.offset_along_spatial_chain(arena, node, |transforms, spatial| {
            transforms
                .tree
                .accumulated_sticky_offset(spatial, transforms.scroll_offsets)
        })
    }

    /// The offset by which scrolling, sticky positioning and anchor scroll shifts move `node`, without its transforms.
    pub(crate) fn offset_to_viewport_ignoring_transforms(
        &self,
        arena: &impl PaintRead,
        node: NodeSlotId,
    ) -> CssPixelPoint {
        self.offset_along_spatial_chain(arena, node, |transforms, spatial| {
            transforms
                .world_transforms
                .offset_to_viewport_ignoring_transforms(transforms.tree, spatial)
        })
    }

    fn offset_along_spatial_chain(
        &self,
        arena: &impl PaintRead,
        node: NodeSlotId,
        device_offset_of_spatial_chain: impl Fn(&Transforms<'_>, SpatialNodeIndex) -> FloatPoint,
    ) -> CssPixelPoint {
        let Some(transforms) = &self.transforms else {
            return CssPixelPoint::default();
        };
        if !arena.paintable_row_is_populated(node) {
            return CssPixelPoint::default();
        }
        let spatial = arena.paintable_data(node).accumulated_visual_context.spatial;
        let offset = device_offset_of_spatial_chain(transforms, spatial);
        let inverse_pixel_ratio = 1.0 / transforms.device_pixels_per_css_pixel;
        CssPixelPoint::new(
            CssPixels::nearest_value_for_f32(offset.x * inverse_pixel_ratio),
            CssPixels::nearest_value_for_f32(offset.y * inverse_pixel_ratio),
        )
    }
}

/// A [`ViewportMapping`] as C++ lends it for `'a`: a non-null `visual_context_tree` is a live retained tree handle with
/// `world_transforms` a live table built from it, and `scroll_offsets` addresses `scroll_offsets_len` points. A null
/// tree leaves geometry as it is. Rust never makes one.
#[repr(C)]
pub struct FfiViewportMapping<'a> {
    visual_context_tree: *const c_void,
    world_transforms: *const c_void,
    scroll_offsets: *const FloatPoint,
    scroll_offsets_len: usize,
    device_pixels_per_css_pixel: f32,
    lent: std::marker::PhantomData<&'a [FloatPoint]>,
}

impl<'a> FfiViewportMapping<'a> {
    pub(crate) fn mapping(&self) -> ViewportMapping<'a> {
        if self.visual_context_tree.is_null() {
            return ViewportMapping::default();
        }
        // SAFETY: C++ lends a live tree, its world transforms and the scroll offsets for 'a.
        unsafe {
            ViewportMapping::new(
                tree_from_handle(self.visual_context_tree),
                world_transforms_from_handle(self.world_transforms),
                ak::slice_from_raw_parts(self.scroll_offsets, self.scroll_offsets_len),
                self.device_pixels_per_css_pixel,
            )
        }
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn viewport_mapping_rect_in_space_to_viewport(
    mapping: FfiViewportMapping<'_>,
    spatial: SpatialNodeIndex,
    rect: FfiCssPixelRect,
    space: ViewportSpace,
    out: &mut FfiCssPixelRect,
) {
    *out = mapping
        .mapping()
        .rect_in_space_to_viewport(spatial, rect.into(), space)
        .into();
}

/// Gives false where [`ViewportMapping::point_to_local`] gives none. Clips are ignored: C++ maps points over a box it hit
/// or captured, and the corners of a rect it clipped already.
#[unsafe(no_mangle)]
pub extern "C" fn viewport_mapping_point_to_local(
    mapping: FfiViewportMapping<'_>,
    context: ContextRef,
    point: FfiCssPixelPoint,
    space: ViewportSpace,
    out: &mut FfiCssPixelPoint,
) -> bool {
    let Some(local) = mapping
        .mapping()
        .point_to_local(context, point.into(), space, ClipBehavior::Ignore)
    else {
        return false;
    };
    *out = to_css_point(local).into();
    true
}
