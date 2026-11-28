/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rasterizing an image into a bitmap of the host's, such as a cursor, an image bitmap or a notification icon: a display
//! list that draws the image, which the compositor rasterizes into the bitmap.

use crate::css::css_pixels::CssPixelRect;
use crate::layout::node_data::{NodeKind, NodeSlotId};
use crate::paint_stage::CompositorPoster;
use crate::painting::display_list::builder::RecordedDisplayList;
use crate::painting::display_list::chunks::{Chunk, chunks_of};
use crate::painting::display_list::commands::{DisplayListCommandRun, DisplayListResourceId, ImageFrameResourceId};
use crate::painting::display_list::commands::{SpatialNodeIndex, VISUAL_VIEWPORT_NODE_INDEX};
use crate::painting::display_list::ids::allocate_display_list_id;
use crate::painting::display_list_resources::DisplayListResources;
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::paint_read::{GeometryRead, PaintRead, PaintSource};
use crate::painting::vector_image_cache::{FfiVectorImageKey, VectorImageKey};
use crate::painting::vector_image_records::{VectorImageRecords, display_list_holding_vector_image_records};
use crate::painting::visual_context::{
    ClipNode, ClipNodeData, EffectNodeData, SpatialData, VisualContextTree, WorldTransforms,
};
use crate::painting::{paintable_geometry, text_fragment};
use crate::render_state::DocumentHost;
use libcompositing_rust::ipc_types::Compositing_DisplayListProperties;
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::RasterizeDisplayList;
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{CompositingAndBlendingOperator, FloatPoint, FloatRect, IntSize, ScalingMode};
use libipc_rust::cpp_connection::{CppConnection, FfiConnection, FfiThreadSafeMessagePoster};
use std::ffi::c_void;
use std::sync::Arc;

/// What an image is rasterized as.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
#[expect(dead_code, reason = "C++ constructs the variants")]
pub enum FfiImageRasterizationKind {
    DecodedFrame,
    VectorImage,
    Gradient,
}

/// An image to rasterize, and where in the bitmap it goes.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiImageRasterization {
    pub kind: FfiImageRasterizationKind,
    pub device_pixels_per_css_pixel: f64,
    pub dest_rect: FloatRect,
    /// The `Gfx::DecodedImageFrame` a decoded frame draws.
    pub image_frame: *const c_void,
    pub scaling_mode: ScalingMode,
    /// The host of the document of the SVG image an SVG image draws, which keeps its rendering for `vector_image_key`.
    pub vector_image_host: *const DocumentHost,
    pub vector_image_key: FfiVectorImageKey,
    pub vector_image_size: IntSize,
    pub gradient_style_value: *const c_void,
    pub gradient_tile_size: crate::layout::used_values::FfiCssPixelSize,
    /// The `FfiColorResolutionStyle` the gradient's color stops resolve against.
    pub gradient_stop_color_resolution_style: *const c_void,
}

/// The display list that draws `image`, and what it draws besides its tape.
///
/// # Safety
///
/// What `image` points at must be live for the call.
unsafe fn record_image(image: &FfiImageRasterization) -> (RecordedDisplayList, DisplayListResources) {
    use crate::css::color_resolution::ColorResolutionInput;
    use crate::painting::display_list::device_pixels::DevicePixelConverter;
    use crate::painting::display_list::recorder::DisplayListRecorder;
    use crate::painting::record::paint::gradient_resolution::{
        record_resolved_gradient_fill, resolve_gradient_paint_with_input,
    };
    // Force-dark never reaches here: an image is darkened by classifying the image itself — not by inverting the fills
    // that raster it.
    let mut recorder = DisplayListRecorder::new(None);
    let mut resources = DisplayListResources::default();
    let dest_rect = image.dest_rect;
    match image.kind {
        FfiImageRasterizationKind::DecodedFrame => {
            // SAFETY: The caller keeps the frame live for the call.
            let frame = unsafe { ImageFrameHandle::retain(image.image_frame) };
            recorder.draw_scaled_decoded_image_frame(
                dest_rect,
                None,
                ImageFrameResourceId(frame.id()),
                image.scaling_mode,
                CompositingAndBlendingOperator::Normal,
                None,
                ForceDarkRole::None,
            );
            resources.image_frames.insert(frame.id(), frame);
        }
        FfiImageRasterizationKind::VectorImage => {
            // SAFETY: The caller keeps the host live for the call.
            let vector_image = unsafe { image.vector_image_host.as_ref() }
                .and_then(|host| host.vector_image(&VectorImageKey::of(&image.vector_image_key)))
                .unwrap_or_else(VectorImageRecords::empty);
            recorder.paint_vector_image_records(
                DisplayListResourceId(vector_image.id()),
                dest_rect,
                image.vector_image_size,
            );
            resources.vector_images.insert(vector_image.id(), vector_image);
        }
        FfiImageRasterizationKind::Gradient => {
            // SAFETY: The caller keeps the gradient style value and the style its color stops resolve against live for
            // the call.
            let (gradient_style_value, color_input) = unsafe {
                (
                    &*image
                        .gradient_style_value
                        .cast::<crate::css::style_value::StyleValueData>(),
                    ColorResolutionInput::for_ffi_style(image.gradient_stop_color_resolution_style.cast()),
                )
            };
            let resolved =
                resolve_gradient_paint_with_input(gradient_style_value, image.gradient_tile_size.into(), &color_input);
            record_resolved_gradient_fill(
                &mut recorder,
                DevicePixelConverter::new(image.device_pixels_per_css_pixel),
                &resolved,
                dest_rect,
                CompositingAndBlendingOperator::Normal,
                ForceDarkRole::None,
            );
        }
    }
    (recorder.into_builder().finish(), resources)
}

#[repr(u8)]
pub enum RasterizationOutcome {
    Rasterized,
    NotRasterized,
    CompositorLost,
}

/// Stages what the display list draws under `token` and has the compositor rasterize it into `target_bitmap`, against
/// `tree`, the visual context tree it was recorded against.
#[allow(clippy::too_many_arguments)]
pub(crate) fn rasterize(
    connection: &CppConnection,
    poster: &CompositorPoster,
    token: u64,
    display_list: &RecordedDisplayList,
    tree: Arc<VisualContextTree>,
    scroll_offsets: Vec<FloatPoint>,
    root_group_color: libgfx_rust::Color,
    resources: &DisplayListResources,
    target_bitmap: *const c_void,
) -> RasterizationOutcome {
    let display_list = display_list_holding_vector_image_records(display_list, &resources.vector_images);
    let resources = resources.with_vector_image_resources();
    let fonts: Vec<FontHandle> = resources.fonts.values().cloned().collect();
    let image_frames: Vec<ImageFrameHandle> = resources.image_frames.values().cloned().collect();
    // SAFETY: The caller keeps the bitmap live for the call.
    if !unsafe { poster.stage_rasterization(token, &fonts, &image_frames, target_bitmap) } {
        return RasterizationOutcome::NotRasterized;
    }
    // The list goes whole, as chunks named by what the compositor replays.
    let chunks: Vec<Chunk> = chunks_of(&display_list.bytes, &display_list.command_runs, &display_list.chunks)
        .map(|(chunk, records, runs)| {
            let chunk_start = chunk.byte_end - records.len() as u32;
            let runs = runs
                .iter()
                .map(|run| DisplayListCommandRun {
                    offset: run.offset - chunk_start,
                    ..*run
                })
                .collect();
            Chunk::named(records.to_vec(), runs)
        })
        .collect();
    let chunks: Vec<&Chunk> = chunks.iter().collect();
    let properties = Compositing_DisplayListProperties {
        id: allocate_display_list_id(),
        compatible_visual_context_tree_structural_epoch: tree.structural_epoch,
        surface_clear_color: None,
        root_group_color,
        window: None,
    };
    let chunks_in_paint_order = chunks.iter().map(|chunk| chunk.id).collect();
    let Some(display_list) = poster.share_display_list(chunks_in_paint_order, &chunks, properties, None) else {
        eprintln!(
            "WebContent: Could not place a {} byte display list to rasterize in shared memory",
            display_list.bytes.len()
        );
        return RasterizationOutcome::NotRasterized;
    };
    match connection.send_sync(RasterizeDisplayList {
        token,
        display_list,
        visual_context_tree: tree,
        scroll_offsets,
    }) {
        Some(response) if response.success => RasterizationOutcome::Rasterized,
        Some(_) => RasterizationOutcome::NotRasterized,
        None => RasterizationOutcome::CompositorLost,
    }
}

/// Has the compositor rasterize `image` into `target_bitmap`, a shareable `Gfx::Bitmap`, under `token`, and waits for it
/// to.
///
/// # Safety
///
/// `connection` must hold one reference to the live connection to the compositor of this thread, and `poster` one
/// reference to a live poster on its transport, which the caller gives up, and what `image` points at and
/// `target_bitmap` must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_rasterize_image(
    connection: FfiConnection,
    poster: FfiThreadSafeMessagePoster,
    token: u64,
    image: &FfiImageRasterization,
    target_bitmap: *const c_void,
) -> RasterizationOutcome {
    // SAFETY: Guaranteed by the caller.
    let (connection, poster) = unsafe { (CppConnection::adopt(connection), CompositorPoster::adopt(poster)) };
    // SAFETY: As above.
    let (display_list, resources) = unsafe { record_image(image) };
    rasterize(
        &connection,
        &poster,
        token,
        &display_list,
        Arc::new(VisualContextTree::create_identity()),
        Vec::new(),
        libgfx_rust::Color::TRANSPARENT,
        &resources,
        target_bitmap,
    )
}

/// Has the compositor rasterize the recording the document of `host` took in last into `target_bitmap`, a shareable
/// `Gfx::Bitmap` the size of the document's viewport, under `token`, and waits for it to.
///
/// # Safety
///
/// `connection` must hold one reference to the live connection to the compositor of this thread, and `poster` one
/// reference to a live poster on its transport, which the caller gives up, `host` must be a live document host, on its
/// document's thread, and `target_bitmap` must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_rasterize_last_recording(
    connection: FfiConnection,
    poster: FfiThreadSafeMessagePoster,
    token: u64,
    host: &DocumentHost,
    read: &crate::render_state::BegunRead,
    target_bitmap: *const c_void,
) -> RasterizationOutcome {
    // SAFETY: Guaranteed by the caller.
    let (connection, poster) = unsafe { (CppConnection::adopt(connection), CompositorPoster::adopt(poster)) };
    let recording =
        crate::painting::ffi::read_arena(host, read, (), |arena, ()| arena.paint_state().last_recording.clone());
    let Some((recording, tree)) = recording.and_then(|recording| {
        let tree = recording.visual_context_tree.clone()?;
        Some((recording, tree))
    }) else {
        return RasterizationOutcome::NotRasterized;
    };
    rasterize(
        &connection,
        &poster,
        token,
        &recording.display_list,
        tree,
        Vec::new(),
        recording.root_group_color,
        &recording.resources,
        target_bitmap,
    )
}

const UNBOUNDED_CLIP_RECT: libgfx_rust::FloatRect = libgfx_rust::FloatRect {
    x: -16_777_216.0,
    y: -16_777_216.0,
    width: 33_554_432.0,
    height: 33_554_432.0,
};

/// The outermost SVG root that contains `subtree`, or else `subtree`.
fn isolation_root(source: &PaintSource<'_>, subtree: NodeSlotId) -> NodeSlotId {
    std::iter::successors(source.node_parent_if_live(subtree), |&node| {
        source.node_parent_if_live(node)
    })
    .filter(|&node| source.node_kind_if_live(node) == Some(NodeKind::SVGSVGBox))
    .last()
    .unwrap_or(subtree)
}

pub(crate) fn tree_for_isolated_subtree(
    source: &PaintSource<'_>,
    subtree: NodeSlotId,
    scroll_offsets: &mut [FloatPoint],
) -> Option<VisualContextTree> {
    let mut tree = source.frame().paint_state().visual_context_tree.as_deref()?.clone();
    let root = isolation_root(source, subtree);
    let context = source.paintable_data(root).accumulated_visual_context;
    let own_nodes = source.frame().rows.visual_context_node_handles(root);

    let mut spatial = context.spatial;
    while spatial != VISUAL_VIEWPORT_NODE_INDEX {
        let node = &mut tree.spatial_nodes[spatial.0 as usize];
        if !own_nodes.spatial.contains(&spatial) {
            match &mut node.data {
                SpatialData::Transform(transform) => transform.matrix = libgfx_rust::FloatMatrix4x4::identity(),
                SpatialData::Perspective(perspective) => perspective.matrix = libgfx_rust::FloatMatrix4x4::identity(),
                _ => {}
            }
            if let Some(offset) = scroll_offsets.get_mut(spatial.0 as usize) {
                *offset = FloatPoint::default();
            }
        }
        spatial = node.parent;
    }

    let mut clip = context.clip;
    while !clip.is_none() {
        let node = &mut tree.clip_nodes[clip.0 as usize];
        if !own_nodes.clip_handles().any(|own| own == clip) {
            *node = ClipNode::new(ClipNodeData::rect_clip(UNBOUNDED_CLIP_RECT), node.parent, node.spatial);
        }
        clip = node.parent;
    }

    let mut effect = context.effect;
    while !effect.is_none() {
        let node = &mut tree.effect_nodes[effect.0 as usize];
        if !own_nodes.effects.contains(&effect) && !matches!(node.data, EffectNodeData::BackgroundColorAnimation) {
            node.data = EffectNodeData::layer_blending_with(CompositingAndBlendingOperator::Normal);
        }
        effect = node.parent;
    }
    Some(tree)
}

pub(crate) fn place_isolated_subtree_on_bitmap(tree: &mut VisualContextTree, origin: FloatPoint, scale: f32) {
    tree.set_visual_viewport_matrix_and_origin(
        libgfx_rust::matrix::scale_matrix(scale, scale, 1.0)
            .multiplied(libgfx_rust::matrix::translation_matrix(-origin.x, -origin.y, 0.0)),
        FloatPoint::default(),
    );
}

/// The bounds in device pixels of `subtree` and the boxes it contains.
pub(crate) fn isolated_subtree_bounds(
    source: &PaintSource<'_>,
    tree: &VisualContextTree,
    subtree: NodeSlotId,
    device_pixels_per_css_pixel: f64,
) -> Option<FloatRect> {
    let world = WorldTransforms::new(tree, &[], None);
    let in_root = |space: SpatialNodeIndex, rect: CssPixelRect| {
        let device_rect = FloatRect {
            x: (rect.x.to_double() * device_pixels_per_css_pixel) as f32,
            y: (rect.y.to_double() * device_pixels_per_css_pixel) as f32,
            width: (rect.width.to_double() * device_pixels_per_css_pixel) as f32,
            height: (rect.height.to_double() * device_pixels_per_css_pixel) as f32,
        };
        world.bounds_in_root_of_rect_drawn_in(space, device_rect)
    };
    let own_bounds = in_root(
        source.paintable_data(subtree).accumulated_visual_context.spatial,
        paintable_geometry::absolute_border_box_rect(source, subtree),
    )?;
    let mut bounds = FloatRect::default();
    source.for_each_node_in_layout_subtree_in_pre_order_with_pruning(subtree, |node| {
        let Some(kind) = source.node_kind_if_live(node) else {
            return false;
        };
        if matches!(
            kind,
            NodeKind::SVGClipBox | NodeKind::SVGMaskBox | NodeKind::SVGPatternBox
        ) {
            return false;
        }
        if !source.paintable_row_is_populated(node) || matches!(kind, NodeKind::TextNode | NodeKind::GeneratedTextNode)
        {
            return true;
        }
        let data = source.paintable_data(node);
        if let Some(rect) = in_root(
            data.accumulated_visual_context.spatial,
            paintable_geometry::absolute_border_box_rect(source, node),
        ) {
            bounds = bounds.united(rect);
        }
        let clips_what_it_contains = !source
            .frame()
            .rows
            .visual_context_node_handles(node)
            .descendant_clips
            .is_empty();
        if clips_what_it_contains {
            return false;
        }
        let text_space = data.accumulated_visual_context_for_descendants.spatial;
        for fragment in source.committed_side_data(node).fragments() {
            if let Some(rect) = in_root(text_space, text_fragment::absolute_rect(source, fragment)) {
                bounds = bounds.united(rect);
            }
        }
        true
    });
    if crate::painting::node_painting::is_fragmented_inline(source, subtree)
        && let Some(root) = source.inline_pieces_root(subtree)
    {
        let text_space = source
            .paintable_data(root)
            .accumulated_visual_context_for_descendants
            .spatial;
        for fragment in source.committed_side_data(root).fragments() {
            let is_text = matches!(
                source.node_kind_if_live(fragment.layout_node),
                Some(NodeKind::TextNode | NodeKind::GeneratedTextNode)
            );
            if is_text
                && source.is_inclusive_ancestor_of(subtree, fragment.layout_node)
                && let Some(rect) = in_root(text_space, text_fragment::absolute_rect(source, fragment))
            {
                bounds = bounds.united(rect);
            }
        }
    }
    Some(if bounds.is_empty() { own_bounds } else { bounds })
}

pub(crate) fn point_in_isolated_subtree(
    source: &PaintSource<'_>,
    tree: &VisualContextTree,
    scroll_offsets: &[FloatPoint],
    subtree: NodeSlotId,
    point: FloatPoint,
) -> Option<FloatPoint> {
    let page_tree = source.frame().paint_state().visual_context_tree.as_deref()?;
    let space = source.paintable_data(subtree).accumulated_visual_context.spatial;
    let at = |point: FloatPoint| FloatRect {
        x: point.x,
        y: point.y,
        width: 0.0,
        height: 0.0,
    };
    let in_space =
        WorldTransforms::new(page_tree, scroll_offsets, None).bounds_in_space_of_root_rect(space, at(point))?;
    let in_root = WorldTransforms::new(tree, &[], None).bounds_in_root_of_rect_drawn_in(
        space,
        at(FloatPoint {
            x: in_space.x,
            y: in_space.y,
        }),
    )?;
    Some(FloatPoint {
        x: in_root.x,
        y: in_root.y,
    })
}
