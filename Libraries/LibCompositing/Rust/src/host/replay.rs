/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::commands::{DisplayListCommandType, EffectNodeIndex, ReplayClip, ReplayLayer, ReplayMask};
use crate::display_list::replay::ReplayPainter;
use libgfx_rust::path::OwnedPath;
use libgfx_rust::{AffineTransform, FloatMatrix4x4, FloatVector3, IntRect, WindingRule};
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
    pub pop_mask: unsafe extern "C" fn(*mut c_void, *const ReplayMask, EffectNodeIndex),
    pub pop: unsafe extern "C" fn(*mut c_void),
    pub push_device_space_plane_clip: unsafe extern "C" fn(*mut c_void, *const FloatVector3, usize),
    pub push_transform: unsafe extern "C" fn(*mut c_void, *const AffineTransform),
    pub push_clip_path_bytes: unsafe extern "C" fn(*mut c_void, *const u8, usize, WindingRule),
    // Arguments: the command type, the command struct, and the payload its spans point into, with its size.
    pub play_command: unsafe extern "C" fn(*mut c_void, DisplayListCommandType, *const u8, *const u8, usize),
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

    fn pop_mask(&mut self, mask: &ReplayMask, effect: EffectNodeIndex) {
        // SAFETY: The C++ painter reads the mask synchronously.
        unsafe { (self.pop_mask)(self.context, mask, effect) };
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

    fn push_clip_path_bytes(&mut self, path_bytes: &[u8], winding_rule: WindingRule) {
        // SAFETY: The C++ painter reads the serialized path synchronously.
        unsafe { (self.push_clip_path_bytes)(self.context, path_bytes.as_ptr(), path_bytes.len(), winding_rule) };
    }

    fn play_command(&mut self, command_type: DisplayListCommandType, command: &[u8], payload: &[u8]) {
        // SAFETY: The C++ painter copies the command struct out of `command` and reads the payload synchronously.
        unsafe {
            (self.play_command)(
                self.context,
                command_type,
                command.as_ptr(),
                payload.as_ptr(),
                payload.len(),
            );
        }
    }
}
