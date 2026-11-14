/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::display_list::commands::{ClipMode, DisplayListCommandType, ReplayClip, ReplayLayer, ReplayMask};
use crate::display_list::replay::{NestedDisplayList, NestedDisplayLists, ReplayPainter, ReplaySelection};
use libgfx_rust::path::Path;
use libgfx_rust::path::ffi::PathGeometryData;
use libgfx_rust::{
    AffineTransform, Color, CompositingAndBlendingOperator, CornerRadii, FloatMatrix4x4, FloatRect, FloatVector3,
    IntRect, IntSize, MaskKind, ScalingMode, WindingRule,
};
use std::ffi::c_void;

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct RecordSpan {
    pub begin_offset: u32,
    pub end_offset: u32,
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiReplaySelection {
    pub record_spans: *const RecordSpan,
    pub record_span_count: usize,
    pub replays_only_record_spans: bool,
    pub skip_root_isolation_layer: bool,
}

impl FfiReplaySelection {
    /// # Safety
    ///
    /// When the selection replays only record spans, `record_spans` must point at `record_span_count` spans that stay
    /// live for the returned lifetime.
    pub unsafe fn as_selection<'a>(&self) -> ReplaySelection<'a> {
        ReplaySelection {
            // SAFETY: The caller guarantees the spans are live.
            record_spans: self
                .replays_only_record_spans
                .then(|| unsafe { ak::slice_from_raw_parts(self.record_spans, self.record_span_count) }),
            skip_root_isolation_layer: self.skip_root_isolation_layer,
        }
    }
}

/// A rect fill whose color the replay resolved for the frame.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct ResolvedFillRect {
    pub rect: IntRect,
    pub color: Color,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
}

/// A rounded rect fill whose color the replay resolved for the frame.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct ResolvedFillRectWithRoundedCorners {
    pub rect: IntRect,
    pub color: Color,
    pub corner_radii: CornerRadii,
}

/// A scrollbar as it is painted, somewhere between its thin and its expanded geometry.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct PresentedScrollbar {
    pub gutter_rect: FloatRect,
    pub thumb_rect: FloatRect,
    pub thumb_color: Color,
    pub thumb_outline_color: Color,
    pub track_color: Color,
    pub opacity: f32,
}

impl Default for PresentedScrollbar {
    fn default() -> Self {
        Self {
            gutter_rect: Default::default(),
            thumb_rect: Default::default(),
            thumb_color: Default::default(),
            thumb_outline_color: Default::default(),
            track_color: Default::default(),
            opacity: 1.0,
        }
    }
}

/// The frame of another context drawn from the surface that holds it: the part of the surface the frame is in, and
/// where it goes, with its origin already snapped where it lands on whole pixels.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct CompositedContextDraw {
    pub surface: *mut c_void,
    pub src_rect: FloatRect,
    pub dst_rect: FloatRect,
    pub scaling_mode: ScalingMode,
}

/// A group of records that is composited as one: inside `clip_rect`, with the opacity, the blend mode and the filter.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct IsolatedGroup {
    pub clip_rect: FloatRect,
    pub opacity: f32,
    pub compositing_and_blending_operator: CompositingAndBlendingOperator,
    pub filter_bytes: *const u8,
    pub filter_bytes_size: usize,
}

/// A raster of tile records that a command repeats, or that a pattern paints with. A cacheable raster stays the same
/// for as long as its key does.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C)]
pub struct TileRaster {
    pub key: u64,
    pub size: IntSize,
    pub cacheable: bool,
}

/// Where a display list nested in another is painted, and the size it was recorded at.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct NestedDisplayListPlacement {
    pub display_list_id: u64,
    pub rect: FloatRect,
    pub list_size: IntSize,
}

/// How the painter takes a nested display list.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum NestedDisplayListReplay {
    /// The painter drew the list from a raster it keeps, or it draws nothing of it.
    Skipped,
    /// The list is replayed into a raster the painter keeps, which it draws once the replay ends.
    IntoRaster,
    /// The list is replayed in place.
    Directly,
}

/// A display list nested in another, as the painter's resources hold it: the storage of its tape, the tree it was
/// recorded against, and whether a raster of it can differ from replaying it.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiNestedDisplayList {
    pub storage: *const c_void,
    pub visual_context_tree: *const c_void,
    pub root_group_color: Color,
    pub requires_direct_replay: bool,
}

/// The raster primitives the replay drives a C++ painter through. The painter knows nothing of the visual context
/// tree, of scroll offsets or of the frame: every value it gets is resolved for what it draws.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiRasterPainter {
    pub context: *mut c_void,
    pub canvas_matrix: unsafe extern "C" fn(*mut c_void) -> FloatMatrix4x4,
    pub set_matrix: unsafe extern "C" fn(*mut c_void, *const FloatMatrix4x4),
    pub would_be_fully_clipped_by_painter: unsafe extern "C" fn(*mut c_void, IntRect) -> bool,
    pub push_clip: unsafe extern "C" fn(*mut c_void, *const ReplayClip),
    /// Takes the contours of the path, the identity of the path, and the fill rule.
    pub push_clip_path: unsafe extern "C" fn(*mut c_void, *const PathGeometryData, u64, WindingRule),
    pub push_layer: unsafe extern "C" fn(*mut c_void, *const ReplayLayer),
    // Arguments: the mask, and the layer whose backdrop filter the mask's layer starts with, or null.
    pub push_mask: unsafe extern "C" fn(*mut c_void, *const ReplayMask, *const ReplayLayer),
    /// Starts the content that masks the layer `push_mask` opened; `pop_mask` closes both.
    pub begin_mask_content: unsafe extern "C" fn(*mut c_void, *const ReplayMask),
    pub pop_mask: unsafe extern "C" fn(*mut c_void),
    pub pop: unsafe extern "C" fn(*mut c_void),
    pub push_device_space_plane_clip: unsafe extern "C" fn(*mut c_void, *const FloatVector3, usize),
    pub push_transform: unsafe extern "C" fn(*mut c_void, *const AffineTransform),
    pub push_clip_path_bytes: unsafe extern "C" fn(*mut c_void, *const u8, usize, WindingRule, ClipMode),
    // Arguments: the command type, the command struct, and the payload its spans point into, with its size.
    pub play_command: unsafe extern "C" fn(*mut c_void, DisplayListCommandType, *const u8, *const u8, usize),
    pub fill_rect: unsafe extern "C" fn(*mut c_void, *const ResolvedFillRect),
    pub fill_rect_with_rounded_corners: unsafe extern "C" fn(*mut c_void, *const ResolvedFillRectWithRoundedCorners),
    pub paint_scrollbar: unsafe extern "C" fn(*mut c_void, *const PresentedScrollbar),
    pub draw_composited_context: unsafe extern "C" fn(*mut c_void, *const CompositedContextDraw),
    pub begin_isolated_group: unsafe extern "C" fn(*mut c_void, *const IsolatedGroup),
    pub begin_isolated_group_mask: unsafe extern "C" fn(*mut c_void, MaskKind),
    pub end_isolated_group_mask: unsafe extern "C" fn(*mut c_void),
    pub end_isolated_group: unsafe extern "C" fn(*mut c_void),
    /// Answers whether the tile records are to be replayed into the raster, which `end_tile_raster` then finishes.
    /// The command that draws the tiles comes next.
    pub begin_tile_raster: unsafe extern "C" fn(*mut c_void, *const TileRaster) -> bool,
    pub end_tile_raster: unsafe extern "C" fn(*mut c_void),
    /// Arguments: the placement, and whether a raster of the list can differ from replaying it. Unless the list is
    /// skipped, its replay is followed by `end_nested_display_list`.
    pub begin_nested_display_list:
        unsafe extern "C" fn(*mut c_void, *const NestedDisplayListPlacement, bool) -> NestedDisplayListReplay,
    pub end_nested_display_list: unsafe extern "C" fn(*mut c_void),
    /// Looks a nested display list up by its resource id, answering false where the resources hold none.
    pub nested_display_list: unsafe extern "C" fn(*mut c_void, u64, *mut FfiNestedDisplayList) -> bool,
}

impl FfiRasterPainter {
    /// The nested display lists that the resources of this painter hold.
    pub fn nested_display_lists(&self) -> FfiNestedDisplayLists {
        FfiNestedDisplayLists {
            context: self.context,
            nested_display_list: self.nested_display_list,
        }
    }
}

/// The nested display lists of a C++ painter's resources, looked up through the painter.
pub struct FfiNestedDisplayLists {
    context: *mut c_void,
    nested_display_list: unsafe extern "C" fn(*mut c_void, u64, *mut FfiNestedDisplayList) -> bool,
}

impl NestedDisplayLists for FfiNestedDisplayLists {
    fn nested_display_list(&self, display_list_id: u64) -> Option<NestedDisplayList<'_>> {
        let mut nested = FfiNestedDisplayList {
            storage: std::ptr::null(),
            visual_context_tree: std::ptr::null(),
            root_group_color: Color::TRANSPARENT,
            requires_direct_replay: true,
        };
        // SAFETY: The C++ painter answers synchronously.
        if !unsafe { (self.nested_display_list)(self.context, display_list_id, &raw mut nested) } {
            return None;
        }
        // SAFETY: The painter's resources keep the storage and the tree alive for as long as the replay runs.
        let (storage, tree) = unsafe {
            (
                crate::display_list::storage::storage_from_handle(nested.storage),
                crate::ffi::tree_from_handle(nested.visual_context_tree),
            )
        };
        Some(NestedDisplayList {
            storage,
            tree,
            root_group_color: nested.root_group_color,
            requires_direct_replay: nested.requires_direct_replay,
        })
    }
}

impl ReplayPainter for FfiRasterPainter {
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
        // SAFETY: The C++ painter reads the layer and its filter bytes synchronously; the tree keeps them alive.
        unsafe { (self.push_layer)(self.context, layer) };
    }

    fn push_mask(&mut self, mask: &ReplayMask, backdrop: Option<&ReplayLayer>) {
        let backdrop = backdrop.map_or(std::ptr::null(), |backdrop| backdrop as *const ReplayLayer);
        // SAFETY: The C++ painter reads the mask, the layer and its filter bytes synchronously; the tree keeps
        //         them alive.
        unsafe { (self.push_mask)(self.context, mask, backdrop) };
    }

    fn begin_mask_content(&mut self, mask: &ReplayMask) {
        // SAFETY: The C++ painter reads the mask synchronously.
        unsafe { (self.begin_mask_content)(self.context, mask) };
    }

    fn pop_mask(&mut self) {
        // SAFETY: The C++ painter pops synchronously.
        unsafe { (self.pop_mask)(self.context) };
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

    fn push_clip_path_bytes(&mut self, path_bytes: &[u8], winding_rule: WindingRule, mode: ClipMode) {
        // SAFETY: The C++ painter reads the serialized path synchronously.
        unsafe { (self.push_clip_path_bytes)(self.context, path_bytes.as_ptr(), path_bytes.len(), winding_rule, mode) };
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

    fn fill_rect(&mut self, fill: &ResolvedFillRect) {
        // SAFETY: The C++ painter reads the fill synchronously.
        unsafe { (self.fill_rect)(self.context, fill) };
    }

    fn fill_rect_with_rounded_corners(&mut self, fill: &ResolvedFillRectWithRoundedCorners) {
        // SAFETY: The C++ painter reads the fill synchronously.
        unsafe { (self.fill_rect_with_rounded_corners)(self.context, fill) };
    }

    fn paint_scrollbar(&mut self, scrollbar: &PresentedScrollbar) {
        // SAFETY: The C++ painter reads the scrollbar synchronously.
        unsafe { (self.paint_scrollbar)(self.context, scrollbar) };
    }

    fn draw_composited_context(&mut self, draw: &CompositedContextDraw) {
        // SAFETY: The C++ painter reads the draw synchronously.
        unsafe { (self.draw_composited_context)(self.context, draw) };
    }

    fn begin_isolated_group(&mut self, group: &IsolatedGroup) {
        // SAFETY: The C++ painter reads the group and its filter bytes synchronously; the tape keeps them alive.
        unsafe { (self.begin_isolated_group)(self.context, group) };
    }

    fn begin_isolated_group_mask(&mut self, kind: MaskKind) {
        // SAFETY: The C++ painter starts the mask synchronously.
        unsafe { (self.begin_isolated_group_mask)(self.context, kind) };
    }

    fn end_isolated_group_mask(&mut self) {
        // SAFETY: The C++ painter ends the mask synchronously.
        unsafe { (self.end_isolated_group_mask)(self.context) };
    }

    fn end_isolated_group(&mut self) {
        // SAFETY: The C++ painter ends the group synchronously.
        unsafe { (self.end_isolated_group)(self.context) };
    }

    fn begin_tile_raster(&mut self, tile: &TileRaster) -> bool {
        // SAFETY: The C++ painter answers synchronously.
        unsafe { (self.begin_tile_raster)(self.context, tile) }
    }

    fn end_tile_raster(&mut self) {
        // SAFETY: The C++ painter finishes the raster synchronously.
        unsafe { (self.end_tile_raster)(self.context) };
    }

    fn begin_nested_display_list(
        &mut self,
        placement: &NestedDisplayListPlacement,
        requires_direct_replay: bool,
    ) -> NestedDisplayListReplay {
        // SAFETY: The C++ painter answers synchronously.
        unsafe { (self.begin_nested_display_list)(self.context, placement, requires_direct_replay) }
    }

    fn end_nested_display_list(&mut self) {
        // SAFETY: The C++ painter finishes the list synchronously.
        unsafe { (self.end_nested_display_list)(self.context) };
    }
}
