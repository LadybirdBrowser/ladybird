/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::painting::display_list::commands::{FrameNodeIndex, ReplayClip, ReplayLayer, ReplayMask};
use crate::painting::display_list::replay::ReplayPainter;
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{FloatMatrix4x4, FloatVector3, IntRect, WindingRule};
use std::ffi::c_void;

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiDisplayListReplayCallbacks {
    pub context: *mut c_void,
    pub canvas_matrix: unsafe extern "C" fn(*mut c_void) -> FloatMatrix4x4,
    pub set_matrix: unsafe extern "C" fn(*mut c_void, *const FloatMatrix4x4),
    pub would_be_fully_clipped_by_painter: unsafe extern "C" fn(*mut c_void, IntRect) -> bool,
    pub push_clip: unsafe extern "C" fn(*mut c_void, *const ReplayClip),
    pub push_clip_path: unsafe extern "C" fn(*mut c_void, *const c_void, WindingRule),
    pub push_layer: unsafe extern "C" fn(*mut c_void, *const ReplayLayer),
    pub push_mask: unsafe extern "C" fn(*mut c_void, *const ReplayMask),
    pub pop_mask: unsafe extern "C" fn(*mut c_void, *const ReplayMask, FrameNodeIndex),
    pub pop: unsafe extern "C" fn(*mut c_void),
    pub push_device_space_plane_clip: unsafe extern "C" fn(*mut c_void, *const FloatVector3, usize),
    pub execute_run: unsafe extern "C" fn(*mut c_void, usize),
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

    fn push_clip_path(&mut self, path: &OwnedPath, winding_rule: WindingRule) {
        // SAFETY: The C++ painter reads the Gfx::Path synchronously; the tree keeps it alive.
        unsafe { (self.push_clip_path)(self.context, path.as_raw(), winding_rule) };
    }

    fn push_layer(&mut self, layer: &ReplayLayer) {
        // SAFETY: The C++ painter reads the layer and its filter bytes synchronously; the tree keeps them alive.
        unsafe { (self.push_layer)(self.context, layer) };
    }

    fn push_mask(&mut self, mask: &ReplayMask) {
        // SAFETY: The C++ painter reads the mask synchronously.
        unsafe { (self.push_mask)(self.context, mask) };
    }

    fn pop_mask(&mut self, mask: &ReplayMask, frame: FrameNodeIndex) {
        // SAFETY: The C++ painter reads the mask synchronously.
        unsafe { (self.pop_mask)(self.context, mask, frame) };
    }

    fn pop(&mut self) {
        // SAFETY: The C++ painter pops synchronously.
        unsafe { (self.pop)(self.context) };
    }

    fn push_device_space_plane_clip(&mut self, vertices: &[FloatVector3]) {
        // SAFETY: The C++ painter reads the vertices synchronously.
        unsafe { (self.push_device_space_plane_clip)(self.context, vertices.as_ptr(), vertices.len()) };
    }

    fn execute_run(&mut self, run_index: usize) {
        // SAFETY: The C++ painter plays the run's commands synchronously.
        unsafe { (self.execute_run)(self.context, run_index) };
    }
}
