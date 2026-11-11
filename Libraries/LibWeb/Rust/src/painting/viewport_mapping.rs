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
use crate::render_state::DocumentHost;
use libcompositing_rust::ffi::tree_sharing_handle;
use libcompositing_rust::scrolling::{AccumulatedVisualContextTree, ScrollStateSnapshot};
use libgfx_rust::{FloatMatrix4x4, FloatPoint, FloatRect};
use std::ffi::c_void;
use std::rc::Rc;
use std::sync::Arc;

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

    /// Maps `rect`, of the line content of `node` that the line root `root` lays out, into the viewport through the
    /// context that content is painted in: that of the nearest self-painting inline box around `node`, or else the one
    /// `root` paints its descendants in, which a scroll container moves by its scroll offset.
    pub(crate) fn line_content_rect_to_viewport(
        &self,
        arena: &impl PaintRead,
        root: NodeSlotId,
        node: NodeSlotId,
        rect: CssPixelRect,
        space: ViewportSpace,
    ) -> CssPixelRect {
        if self.transforms.is_none() {
            return rect;
        }
        let painter =
            crate::painting::fragment_ownership::nearest_self_painting_inline_box(arena, node).unwrap_or(root);
        if !arena.paintable_row_is_populated(painter) {
            return CssPixelRect::default();
        }
        let context = arena.paintable_data(painter).accumulated_visual_context_for_descendants;
        self.rect_in_space_to_viewport(context.spatial, rect, space)
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

    /// The matrix that takes the local space of `spatial` into the viewport, both in device pixels, or the identity for
    /// a document that has no visual context tree.
    pub(crate) fn matrix_to_viewport(&self, spatial: SpatialNodeIndex, space: ViewportSpace) -> FloatMatrix4x4 {
        match &self.transforms {
            Some(transforms) => transforms.world_transforms.to_viewport_matrix(spatial, space.into()),
            None => FloatMatrix4x4::identity(),
        }
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

/// A [`ViewportMapping`] as C++ carries it for `'a`, which only [`render_state_viewport_mapping`] makes: C++ hands it
/// back as it got it, or one it left zeroed, which leaves geometry as it is. `visual_context_tree` is a tree handle
/// that stays live for `'a`. Where the document is scrolled to, and the world transforms of the tree there, are read
/// from `host` where the mapping is used.
#[repr(C)]
pub struct FfiViewportMapping<'a> {
    host: Option<&'a DocumentHost>,
    visual_context_tree: *const c_void,
    device_pixels_per_css_pixel: f32,
}

impl FfiViewportMapping<'_> {
    /// What the mapping maps through as the host has it now, kept for as long as an entry point reads the mapping.
    pub(crate) fn resolve(&self) -> ResolvedViewportMapping {
        let Some(host) = self.host.filter(|_| !self.visual_context_tree.is_null()) else {
            return ResolvedViewportMapping { transforms: None };
        };
        // SAFETY: C++ keeps the tree live for as long as it carries the mapping.
        let tree = unsafe { tree_sharing_handle(self.visual_context_tree) };
        let scroll_state = host.scroll_state();
        let world_transforms = scroll_state.world_transforms(&tree);
        ResolvedViewportMapping {
            transforms: Some(ResolvedTransforms {
                tree,
                scroll_state,
                world_transforms,
                device_pixels_per_css_pixel: self.device_pixels_per_css_pixel,
            }),
        }
    }
}

struct ResolvedTransforms {
    tree: AccumulatedVisualContextTree,
    scroll_state: Rc<ScrollStateSnapshot>,
    world_transforms: Arc<WorldTransforms>,
    device_pixels_per_css_pixel: f32,
}

/// A mapping C++ carries, with a share of its own in everything it maps through: nothing another mapping or a refresh
/// of the host's scroll state does takes that away while an entry point reads it.
pub(crate) struct ResolvedViewportMapping {
    transforms: Option<ResolvedTransforms>,
}

impl ResolvedViewportMapping {
    pub(crate) fn mapping(&self) -> ViewportMapping<'_> {
        match &self.transforms {
            Some(transforms) => ViewportMapping::new(
                transforms.tree.as_arc(),
                &transforms.world_transforms,
                transforms.scroll_state.device_offsets(),
                transforms.device_pixels_per_css_pixel,
            ),
            None => ViewportMapping::default(),
        }
    }
}

/// The mapping between the viewport of `host`'s document and the spaces of `visual_context_tree`, where the document's
/// boxes are scrolled to as the host has it when the mapping is used.
///
/// # Safety
///
/// `visual_context_tree` must be a live retained tree handle, which stays live for as long as C++ carries the mapping:
/// the caller keeps it, or it is the document's own, which the scroll state retains from here on, with the world
/// transforms it builds for it, until the host refreshes the scroll state or builds those of another tree.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_viewport_mapping<'a>(
    host: &'a DocumentHost,
    visual_context_tree: *const c_void,
    device_pixels_per_css_pixel: f32,
    out: &mut FfiViewportMapping<'a>,
) {
    // SAFETY: The caller lends a strong reference to the tree.
    let tree = unsafe { tree_sharing_handle(visual_context_tree) };
    host.scroll_state().world_transforms(&tree);
    *out = FfiViewportMapping {
        host: Some(host),
        visual_context_tree,
        device_pixels_per_css_pixel,
    };
}

/// The matrix that takes the local space of `spatial` into the viewport, both in device pixels.
#[unsafe(no_mangle)]
pub extern "C" fn viewport_mapping_matrix_to_viewport(
    mapping: FfiViewportMapping<'_>,
    spatial: SpatialNodeIndex,
    space: ViewportSpace,
    out: &mut FloatMatrix4x4,
) {
    *out = mapping.resolve().mapping().matrix_to_viewport(spatial, space);
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
        .resolve()
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
        .resolve()
        .mapping()
        .point_to_local(context, point.into(), space, ClipBehavior::Ignore)
    else {
        return false;
    };
    *out = to_css_point(local).into();
    true
}

#[cfg(test)]
mod tests {
    use super::*;
    use libcompositing_rust::node_slot_id::NodeSlotId;
    use libcompositing_rust::visual_context::scroll_state::NO_SCROLL_STATE_SLOT;
    use libcompositing_rust::visual_context::{ScrollData, SpatialData, TransformData, TransformDataRole};

    fn tree_with_scroller() -> (Arc<VisualContextTree>, SpatialNodeIndex) {
        let mut tree = VisualContextTree::create(TransformData {
            matrix: FloatMatrix4x4::identity(),
            origin: FloatPoint::default(),
            sorting_context_root_index: None,
            flattens_inherited_transform: false,
            role: TransformDataRole::CssTransform,
            synthetic_plane: false,
            establishes_sorting_context: false,
            bounds: FloatRect::default(),
        });
        let scroller = tree.append_spatial(
            SpatialData::Scroll(ScrollData {
                state_slot: NO_SCROLL_STATE_SLOT,
                owner_paintable: NodeSlotId::INVALID,
            }),
            VISUAL_VIEWPORT_NODE_INDEX,
        );
        (Arc::new(tree), scroller)
    }

    #[test]
    fn a_mapping_stands_after_a_mapping_over_another_tree_and_a_scroll_state_refresh() {
        let host = DocumentHost::for_test();
        let (tree, scroller) = tree_with_scroller();
        let (other_tree, _) = tree_with_scroller();
        let scrolled_to = |y: f32| vec![FloatPoint::default(), FloatPoint { x: 0.0, y }];
        let mapping_over = |tree: &Arc<VisualContextTree>| {
            let mut mapping = FfiViewportMapping {
                host: None,
                visual_context_tree: std::ptr::null(),
                device_pixels_per_css_pixel: 0.0,
            };
            // SAFETY: The test keeps the tree for as long as it uses the mapping.
            unsafe { render_state_viewport_mapping(&host, Arc::as_ptr(tree).cast(), 1.0, &mut mapping) };
            mapping
        };
        let ten = CssPixels::from_integer(10);
        let rect = CssPixelRect::new(CssPixels::default(), CssPixels::default(), ten, ten);
        let top_in_viewport = |mapping: &FfiViewportMapping<'_>| {
            mapping
                .resolve()
                .mapping()
                .rect_in_space_to_viewport(scroller, rect, ViewportSpace::LayoutViewport)
                .y
        };

        host.refresh_scroll_state(scrolled_to(-30.0));
        let mapping = mapping_over(&tree);
        assert_eq!(top_in_viewport(&mapping), CssPixels::from_integer(-30));

        // The scroll state keeps the world transforms of one tree, which are those of the other tree from here on.
        let other_mapping = mapping_over(&other_tree);
        assert_eq!(top_in_viewport(&other_mapping), CssPixels::from_integer(-30));
        assert_eq!(top_in_viewport(&mapping), CssPixels::from_integer(-30));

        host.refresh_scroll_state(scrolled_to(-50.0));
        assert_eq!(top_in_viewport(&mapping), CssPixels::from_integer(-50));
    }
}
