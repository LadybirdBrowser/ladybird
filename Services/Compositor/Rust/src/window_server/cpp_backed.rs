/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! A window server that the C++ around the core reaches the system's compositor through.

use super::{Frame, FrameSublayer, MediaToShow, PlacedContent, RemoteLayer, SublayerContent, WindowServer};
use crate::cpp_object::CppObject;
use crate::raster_host::{
    FfiRemoteLayerFrame, FfiSublayer, FfiSublayerBackdrop, FfiSublayerGroup, FfiSublayerPlacement, FrameHandOff,
    RasterHost, SublayerContentKind,
};
use libcompositing_rust::display_list::layers::WindowServerCompositing;
use libcompositing_rust::display_list::window_server_placement::WindowServerPlacement;
use libcompositing_rust::ipc_types::Compositing_RemoteLayerHostHandle;
use libgfx_rust::IntSize;
use std::rc::Rc;

pub(super) struct CppBackedWindowServer {
    object: CppObject,
    raster: RasterHost,
}

impl CppBackedWindowServer {
    pub(super) fn create(raster: &RasterHost) -> Option<Rc<dyn WindowServer>> {
        let object = raster.create_window_server()?;
        Some(Rc::new(Self {
            object,
            raster: raster.clone(),
        }))
    }
}

impl WindowServer for CppBackedWindowServer {
    fn compositing(&self) -> WindowServerCompositing {
        let compositing = self.raster.window_server_compositing(&self.object);
        WindowServerCompositing {
            shows_tiles_of_painted_layers: compositing.shows_tiles_of_painted_layers,
            filters_what_is_below_layers: compositing.filters_what_is_below_layers,
        }
    }

    fn frame_hand_off(&self) -> FrameHandOff {
        self.raster.window_server_frame_hand_off(&self.object)
    }

    fn create_remote_layer(&self) -> Option<Rc<dyn RemoteLayer>> {
        let (object, host_handle) = self.raster.create_remote_layer(&self.object)?;
        Some(Rc::new(CppBackedRemoteLayer {
            object,
            host_handle,
            raster: self.raster.clone(),
        }))
    }

    fn hold_changes_on_screen(&self) {
        self.raster.window_server_hold_changes_on_screen(&self.object);
    }

    fn release_changes_held_off_screen(&self) {
        self.raster.window_server_release_changes_held_off_screen(&self.object);
    }
}

struct CppBackedRemoteLayer {
    object: CppObject,
    host_handle: Compositing_RemoteLayerHostHandle,
    raster: RasterHost,
}

impl RemoteLayer for CppBackedRemoteLayer {
    fn host_handle(&self) -> Compositing_RemoteLayerHostHandle {
        self.host_handle.clone()
    }

    fn can_show(&self, placement: &WindowServerPlacement, media: MediaToShow<'_>, device_pixel_ratio: f64) -> bool {
        let (surface, video_frame) = match media {
            MediaToShow::Surface(surface) => (Some(&**surface), None),
            MediaToShow::Video(frame) => (None, Some(frame)),
        };
        self.raster.remote_layer_can_show(
            &self.object,
            surface,
            video_frame,
            &sublayer_placement_of(placement),
            device_pixel_ratio,
        )
    }

    fn show_frame(&self, frame: Frame) -> bool {
        let sublayers: Vec<FfiSublayer> = frame.sublayers.iter().map(ffi_sublayer_of).collect();
        // The placements lend the contours of their paths for the call, and the sublayers keep the surfaces as they
        // are until the C++ layers hold them.
        let built_sublayers = self.raster.build_sublayers(&sublayers);
        let ffi_frame = FfiRemoteLayerFrame {
            store: frame
                .store
                .as_ref()
                .map_or(std::ptr::null(), |store| store.surface().as_ffi()),
            shows_no_store: frame.shows_no_store,
            painted_size: frame.painted_size,
            device_pixel_ratio: frame.device_pixel_ratio,
            has_fit_within: frame.fit_within.is_some(),
            fit_within: frame.fit_within.unwrap_or_default(),
        };
        self.raster
            .remote_layer_show_frame(&self.object, &ffi_frame, built_sublayers)
    }

    fn place_frame(&self, fit_within: Option<IntSize>) {
        self.raster.remote_layer_place_frame(&self.object, fit_within);
    }
}

/// The placement as the C++ side is handed it, whose path contours are lent from `placement`.
fn sublayer_placement_of(placement: &WindowServerPlacement) -> FfiSublayerPlacement {
    let rounded_clip = placement.rounded_clip;
    let path_clip = placement.path_clip.as_ref();
    FfiSublayerPlacement {
        content_rect: placement.content_rect,
        content_to_frame: placement.content_to_viewport.values,
        clip_rect: placement.clip_rect,
        has_rounded_clip: rounded_clip.is_some(),
        rounded_clip_rect: rounded_clip.map(|clip| clip.rect).unwrap_or_default(),
        rounded_clip_radius: rounded_clip.map_or(0.0, |clip| clip.radius),
        rounds_top_left: rounded_clip.is_some_and(|clip| clip.rounded_corners.top_left),
        rounds_top_right: rounded_clip.is_some_and(|clip| clip.rounded_corners.top_right),
        rounds_bottom_right: rounded_clip.is_some_and(|clip| clip.rounded_corners.bottom_right),
        rounds_bottom_left: rounded_clip.is_some_and(|clip| clip.rounded_corners.bottom_left),
        is_clipped_by_path: path_clip.is_some(),
        clip_path_identity: path_clip.map_or(0, |clip| clip.path.identity()),
        clip_path_fill_rule: path_clip.map(|clip| clip.fill_rule).unwrap_or_default(),
        clip_path_to_frame: path_clip.map_or([0.0; 6], |clip| clip.path_to_viewport.values),
        clip_path_contours: path_clip.map(|clip| clip.path.geometry_data()).unwrap_or_default(),
        scaling_mode: placement.scaling_mode,
    }
}

fn ffi_sublayer_of(sublayer: &FrameSublayer) -> FfiSublayer {
    let of_kind = |content_kind| FfiSublayer {
        group_index: sublayer
            .group_index
            .map_or(-1, |index| i32::try_from(index).unwrap_or(i32::MAX)),
        ..FfiSublayer::of_kind(sublayer.identity, content_kind)
    };
    match &sublayer.content {
        SublayerContent::Group(group) => FfiSublayer {
            group: FfiSublayerGroup {
                to_parent: group.to_parent.values,
                clips: group.clip_rect.is_some(),
                clip_rect: group.clip_rect.unwrap_or_default(),
                corner_radii: group.corner_radii,
                opacity: group.opacity,
            },
            ..of_kind(SublayerContentKind::Group)
        },
        SublayerContent::Tile {
            surface,
            rect,
            rect_in_surface,
            every_pixel_is_opaque,
        } => FfiSublayer {
            presentable_surface: surface.surface().as_ffi(),
            rect: *rect,
            rect_in_surface: *rect_in_surface,
            every_pixel_is_opaque: *every_pixel_is_opaque,
            ..of_kind(SublayerContentKind::Tile)
        },
        SublayerContent::Color { color, rect } => FfiSublayer {
            rect: *rect,
            color: *color,
            ..of_kind(SublayerContentKind::Color)
        },
        SublayerContent::Backdrop(backdrop) => {
            let filters = &backdrop.filters;
            FfiSublayer {
                backdrop: FfiSublayerBackdrop {
                    region: backdrop.region,
                    has_color_matrix_before_blur: filters.color_matrix_before_blur.is_some(),
                    color_matrix_before_blur: filters.color_matrix_before_blur.unwrap_or_default(),
                    has_blur: filters.blur_standard_deviation.is_some(),
                    blur_standard_deviation: filters.blur_standard_deviation.unwrap_or_default(),
                    has_color_matrix_after_blur: filters.color_matrix_after_blur.is_some(),
                    color_matrix_after_blur: filters.color_matrix_after_blur.unwrap_or_default(),
                },
                ..of_kind(SublayerContentKind::Backdrop)
            }
        }
        SublayerContent::Placed { content, placement } => match content {
            PlacedContent::Surface(lease) => FfiSublayer {
                presentable_surface: lease.surface().as_ffi(),
                placement: sublayer_placement_of(placement),
                ..of_kind(SublayerContentKind::PresentableSurface)
            },
            PlacedContent::Video(frame) => FfiSublayer {
                video_frame: frame.as_ffi(),
                placement: sublayer_placement_of(placement),
                ..of_kind(SublayerContentKind::Video)
            },
        },
    }
}
