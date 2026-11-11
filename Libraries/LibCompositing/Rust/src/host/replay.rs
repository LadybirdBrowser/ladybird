/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::commands::{DisplayListCommandType, DrawIsolatedGroup, ReplayClip, ReplayMask};
use crate::display_list::filter_graph::{FilterGraph, LoweredFilterGraph};
use crate::display_list::played::{PlayedCommand, PlayedLayer, PlayedPayload};
use crate::display_list::replay::{ReplayLayer, ReplayPainter};
use libgfx_rust::path::Path;
use libgfx_rust::path::ffi::PathGeometryData;
use libgfx_rust::{AffineTransform, FloatMatrix4x4, FloatVector3, IntRect, IntSize, MaskKind, WindingRule};
use std::ffi::c_void;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiDisplayListReplayCallbacks {
    pub context: *mut c_void,
    pub canvas_matrix: unsafe extern "C" fn(*mut c_void) -> FloatMatrix4x4,
    pub set_matrix: unsafe extern "C" fn(*mut c_void, *const FloatMatrix4x4),
    pub would_be_fully_clipped_by_painter: unsafe extern "C" fn(*mut c_void, IntRect) -> bool,
    pub push_clip: unsafe extern "C" fn(*mut c_void, *const ReplayClip),
    /// Takes the contours of the path, the identity of the path, and the fill rule.
    pub push_clip_path: unsafe extern "C" fn(*mut c_void, *const PathGeometryData, u64, WindingRule),
    pub push_layer: unsafe extern "C" fn(*mut c_void, *const PlayedLayer),
    // Arguments: the mask, and the layer whose backdrop filter the mask's layer starts with, or null.
    pub push_mask: unsafe extern "C" fn(*mut c_void, *const ReplayMask, *const PlayedLayer),
    pub begin_mask_content: unsafe extern "C" fn(*mut c_void, *const ReplayMask),
    pub end_mask: unsafe extern "C" fn(*mut c_void),
    // Arguments: the group, and its filter graph, which has no nodes for no filter.
    pub begin_isolated_group: unsafe extern "C" fn(*mut c_void, *const DrawIsolatedGroup, *const FilterGraph),
    pub begin_isolated_group_mask: unsafe extern "C" fn(*mut c_void, MaskKind),
    pub end_isolated_group: unsafe extern "C" fn(*mut c_void),
    pub use_cached_tile_raster: unsafe extern "C" fn(*mut c_void, u64, IntSize) -> bool,
    pub begin_tile_raster: unsafe extern "C" fn(*mut c_void, IntSize) -> bool,
    // Takes the key to cache the raster by, or null to not cache it.
    pub end_tile_raster: unsafe extern "C" fn(*mut c_void, *const u64),
    pub pop: unsafe extern "C" fn(*mut c_void),
    pub push_device_space_plane_clip: unsafe extern "C" fn(*mut c_void, *const FloatVector3, usize),
    pub push_transform: unsafe extern "C" fn(*mut c_void, *const AffineTransform),
    /// Takes the contours of a path that is not kept beyond the clip, and the fill rule.
    pub push_uncached_clip_path: unsafe extern "C" fn(*mut c_void, *const PathGeometryData, WindingRule),
    /// Takes the command type, the command, whose type the command type picks, and what its payload holds.
    pub play_command: unsafe extern "C" fn(*mut c_void, DisplayListCommandType, *const c_void, *const PlayedPayload),
}

impl ReplayPainter for FfiDisplayListReplayCallbacks {
    fn canvas_matrix(&mut self) -> FloatMatrix4x4 {
        // SAFETY: The C++ painter answers synchronously.
        unsafe { (self.canvas_matrix)(self.context) }
    }

    fn set_matrix(&mut self, matrix: &FloatMatrix4x4) {
        // SAFETY: The C++ painter reads the matrix synchronously.
        unsafe { (self.set_matrix)(self.context, matrix) };
    }

    fn would_be_fully_clipped_by_painter(&mut self, rect: IntRect) -> bool {
        // SAFETY: The C++ painter answers synchronously.
        unsafe { (self.would_be_fully_clipped_by_painter)(self.context, rect) }
    }

    fn push_clip(&mut self, clip: &ReplayClip) {
        // SAFETY: The C++ painter reads the clip synchronously.
        unsafe { (self.push_clip)(self.context, clip) };
    }

    fn push_clip_path(&mut self, path: &Path, winding_rule: WindingRule) {
        let data = path.geometry_data();
        // SAFETY: The C++ painter reads the contours synchronously; the tree keeps the path alive.
        unsafe { (self.push_clip_path)(self.context, &raw const data, path.identity(), winding_rule) };
    }

    fn push_layer(&mut self, layer: &ReplayLayer) {
        let filter = LoweredFilterGraph::read(layer.filter);
        let backdrop_filter = LoweredFilterGraph::read(layer.backdrop_filter);
        // SAFETY: The C++ painter reads the layer and its filter graphs synchronously.
        unsafe { (self.push_layer)(self.context, &PlayedLayer::of(layer, &filter, &backdrop_filter)) };
    }

    fn push_mask(&mut self, mask: &ReplayMask, backdrop: Option<&ReplayLayer>) {
        let filters = backdrop.map(|backdrop| {
            (
                LoweredFilterGraph::read(backdrop.filter),
                LoweredFilterGraph::read(backdrop.backdrop_filter),
            )
        });
        let backdrop = backdrop
            .zip(filters.as_ref())
            .map(|(backdrop, (filter, backdrop_filter))| PlayedLayer::of(backdrop, filter, backdrop_filter));
        let backdrop = backdrop.as_ref().map_or(std::ptr::null(), std::ptr::from_ref);
        // SAFETY: The C++ painter reads the mask, the layer and its filter graphs synchronously.
        unsafe { (self.push_mask)(self.context, mask, backdrop) };
    }

    fn begin_mask_content(&mut self, mask: &ReplayMask) {
        // SAFETY: The C++ painter reads the mask synchronously.
        unsafe { (self.begin_mask_content)(self.context, mask) };
    }

    fn end_mask(&mut self) {
        // SAFETY: The C++ painter closes the mask synchronously.
        unsafe { (self.end_mask)(self.context) };
    }

    fn begin_isolated_group(&mut self, group: &DrawIsolatedGroup, filter: &[u8]) {
        let filter = LoweredFilterGraph::read(filter);
        // SAFETY: The C++ painter reads the group and its filter graph synchronously.
        unsafe { (self.begin_isolated_group)(self.context, group, &filter.graph()) };
    }

    fn begin_isolated_group_mask(&mut self, kind: MaskKind) {
        // SAFETY: The C++ painter opens the mask synchronously.
        unsafe { (self.begin_isolated_group_mask)(self.context, kind) };
    }

    fn end_isolated_group(&mut self) {
        // SAFETY: The C++ painter closes the group synchronously.
        unsafe { (self.end_isolated_group)(self.context) };
    }

    fn use_cached_tile_raster(&mut self, key: u64, size: IntSize) -> bool {
        // SAFETY: The C++ painter answers synchronously.
        unsafe { (self.use_cached_tile_raster)(self.context, key, size) }
    }

    fn begin_tile_raster(&mut self, size: IntSize) -> bool {
        // SAFETY: The C++ painter answers synchronously.
        unsafe { (self.begin_tile_raster)(self.context, size) }
    }

    fn end_tile_raster(&mut self, cache_key: Option<u64>) {
        let cache_key = cache_key.as_ref().map_or(std::ptr::null(), |key| key as *const u64);
        // SAFETY: The C++ painter reads the key synchronously.
        unsafe { (self.end_tile_raster)(self.context, cache_key) };
    }

    fn pop(&mut self) {
        // SAFETY: The C++ painter pops synchronously.
        unsafe { (self.pop)(self.context) };
    }

    fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]) {
        // SAFETY: The C++ painter reads the vertices synchronously.
        unsafe { (self.push_device_space_plane_clip)(self.context, vertices.as_ptr(), vertices.len()) };
    }

    fn push_transform(&mut self, transform: &AffineTransform) {
        // SAFETY: The C++ painter reads the transform synchronously.
        unsafe { (self.push_transform)(self.context, transform) };
    }

    fn push_uncached_clip_path(&mut self, path: &Path, winding_rule: WindingRule) {
        let data = path.geometry_data();
        // SAFETY: The C++ painter reads the contours synchronously.
        unsafe { (self.push_uncached_clip_path)(self.context, &raw const data, winding_rule) };
    }

    fn play_command(&mut self, command: &PlayedCommand, payload: &PlayedPayload) {
        // SAFETY: The C++ painter reads the command and the payload synchronously.
        unsafe { (self.play_command)(self.context, command.command_type(), command.as_ffi_pointer(), payload) };
    }
}
