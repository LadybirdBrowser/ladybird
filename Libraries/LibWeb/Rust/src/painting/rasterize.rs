/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rasterizing an image into a bitmap of the host's, such as a cursor, an image bitmap or a notification icon: a display
//! list that draws the image, which the compositor rasterizes into the bitmap.

use crate::paint_stage::CompositorPoster;
use crate::painting::display_list::builder::RecordedDisplayList;
use crate::painting::display_list::commands::{DisplayListResourceId, ImageFrameResourceId};
use crate::painting::display_list::ids::allocate_display_list_id;
use crate::painting::display_list::storage::DisplayListStorage;
use crate::painting::display_list_resources::{DisplayListResources, NestedDisplayList};
use crate::painting::force_dark::ForceDarkRole;
use crate::painting::vector_image_cache::{FfiVectorImageKey, VectorImageKey};
use crate::painting::visual_context::VisualContextTree;
use crate::render_state::DocumentHost;
use libcompositing_rust::ipc_types::compositor_web_content_server::messages::RasterizeDisplayList;
use libcompositing_rust::ipc_types::{Compositing_DisplayListProperties, Compositing_DisplayListResourceTransaction};
use libgfx_rust::font::FontHandle;
use libgfx_rust::image_frame::ImageFrameHandle;
use libgfx_rust::{CompositingAndBlendingOperator, FloatRect, IntSize, ScalingMode};
use libipc_rust::cpp_connection::FfiThreadSafeMessagePoster;
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
            let display_list = unsafe { image.vector_image_host.as_ref() }
                .and_then(|host| host.vector_image(&VectorImageKey::of(&image.vector_image_key)))
                .unwrap_or_else(NestedDisplayList::empty);
            recorder.paint_nested_display_list(
                DisplayListResourceId(display_list.id()),
                dest_rect,
                image.vector_image_size,
            );
            resources.nested_display_lists.insert(display_list.id(), display_list);
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

/// Stages what the display list draws under `token` and hands it to the compositor to rasterize into `target_bitmap`.
/// Answers whether the compositor could still be reached.
fn post_rasterization(
    poster: &CompositorPoster,
    token: u64,
    display_list: RecordedDisplayList,
    resources: DisplayListResources,
    target_bitmap: *const c_void,
) -> bool {
    let resources = resources.with_nested_resources();
    let fonts: Vec<FontHandle> = resources.fonts.values().cloned().collect();
    let image_frames: Vec<ImageFrameHandle> = resources.image_frames.values().cloned().collect();
    let video_sinks: Vec<(u64, u64)> = resources
        .video_sinks
        .iter()
        .map(|(id, handle)| (*id, *handle))
        .collect();
    // SAFETY: The caller keeps the bitmap live for the call.
    if !unsafe { poster.stage_rasterization(token, &fonts, &image_frames, &video_sinks, target_bitmap) } {
        return false;
    }
    let tree = Arc::new(VisualContextTree::create_identity());
    let storage = DisplayListStorage::recorded(Arc::new(display_list), tree.clone());
    let Some(display_list_buffer) =
        poster.shared_buffer_with(storage.packed_size(), |destination| storage.write_packed(destination))
    else {
        eprintln!(
            "WebContent: Could not place a {} byte display list to rasterize in shared memory",
            storage.bytes().len()
        );
        return false;
    };
    poster.post(RasterizeDisplayList {
        token,
        display_list_buffer,
        tape_size: storage.bytes().len() as u64,
        run_count: storage.command_runs().len() as u64,
        display_list_properties: Compositing_DisplayListProperties {
            id: allocate_display_list_id(),
            compatible_visual_context_tree_structural_epoch: tree.structural_epoch,
            surface_clear_color: None,
            root_group_color: libgfx_rust::Color::TRANSPARENT,
        },
        visual_context_tree: tree,
        nested_display_lists: Compositing_DisplayListResourceTransaction {
            display_lists: resources
                .nested_display_lists
                .values()
                .map(|display_list| display_list.resource.clone())
                .collect(),
            ..Default::default()
        },
    })
}

/// Has the compositor rasterize `image` into `target_bitmap`, a shareable `Gfx::Bitmap`, through `poster`, under `token`,
/// which the compositor's answer names. Answers whether the image went to the compositor.
///
/// # Safety
///
/// `poster` must hold one reference to a live poster on the transport of the connection to the compositor, which the
/// caller gives up, and what `image` points at and `target_bitmap` must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn render_state_rasterize_image(
    poster: FfiThreadSafeMessagePoster,
    token: u64,
    image: &FfiImageRasterization,
    target_bitmap: *const c_void,
) -> bool {
    // SAFETY: Guaranteed by the caller.
    let poster = unsafe { CompositorPoster::adopt(poster) };
    // SAFETY: As above.
    let (display_list, resources) = unsafe { record_image(image) };
    post_rasterization(&poster, token, display_list, resources, target_bitmap)
}
