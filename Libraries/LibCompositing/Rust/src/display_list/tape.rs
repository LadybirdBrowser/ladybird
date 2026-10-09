/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How records and runs are laid out on a display list tape. Only this crate reads these; C++ sees the command
//! structs and opaque tapes, so these types stay out of the generated headers.

use super::commands::{ClipMode, ContextRef, DisplayListCommandType, DisplayListDataSpan};
use crate::ffi_bytes_fields;
use crate::ffi_enum_bytes;
use libgfx_rust::*;

// Keep command payloads aligned, including inline object arrays.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(C, align(8))]
pub struct DisplayListCommandHeader {
    pub command_type: DisplayListCommandType,
    pub has_bounding_rect: bool,
    pub inline_clip_count: u8,
    pub has_inline_transform: bool,
    pub payload_size: u32,
    pub bounding_rect: IntRect,
}
ffi_bytes_fields!(DisplayListCommandHeader {
    command_type,
    has_bounding_rect,
    inline_clip_count,
    has_inline_transform,
    payload_size,
    bounding_rect
});
const _: () = assert!(std::mem::size_of::<DisplayListCommandHeader>() == 24);

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(u8)]
pub enum InlineClipKind {
    #[default]
    Rect,
    RoundedRect,
    Path,
}
ffi_enum_bytes!(InlineClipKind as u8);

// A clip applied by the player around a single command dispatch, stored as a
// fixed-size entry at the tail of the command payload.
#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DisplayListInlineClip {
    pub clip_rect_or_path_device_bounds: FloatRect,
    pub corner_radii: CornerRadii,
    pub path_data: DisplayListDataSpan,
    pub path_winding_rule: WindingRule,
    pub kind: InlineClipKind,
    pub mode: ClipMode,
}
ffi_bytes_fields!(DisplayListInlineClip {
    clip_rect_or_path_device_bounds,
    corner_radii,
    path_data,
    path_winding_rule,
    kind,
    mode
});
pub const INLINE_CLIP_ENTRY_SIZE: usize = std::mem::size_of::<DisplayListInlineClip>();
const _: () = assert!(INLINE_CLIP_ENTRY_SIZE == 64);

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(C)]
pub struct DisplayListInlineTransform {
    pub transform: AffineTransform,
    pub padding: [u32; 2],
}
ffi_bytes_fields!(DisplayListInlineTransform { transform, padding });
pub const INLINE_TRANSFORM_ENTRY_SIZE: usize = std::mem::size_of::<DisplayListInlineTransform>();
const _: () = assert!(INLINE_TRANSFORM_ENTRY_SIZE == 32);

// A maximal sequence of consecutive commands sharing one visual context, summarized as the tape is
// built so that replay can enter a context, cull, and depth-sort per run. The run owns the visual
// context of its commands, including records nested inside groups. Runs cover the whole tape.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct DisplayListCommandRun {
    pub offset: u32,
    pub size: u32,
    pub context: ContextRef,
    // Union of the draw commands' bounding rects in the run's spatial node space. Every draw command
    // has one, so this bounds everything the run draws before its visual context applies.
    pub ink_bounds: IntRect,
    pub has_compositor_metadata: bool,
}
const _: () = assert!(std::mem::size_of::<DisplayListCommandRun>() == 40);
