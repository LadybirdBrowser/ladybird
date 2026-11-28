/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The HarfBuzz functions, types and values that text layout uses, as HarfBuzz 10.2 declares them.
//!
//! The crate does not link HarfBuzz itself: every library and executable that links the crate links the HarfBuzz that
//! CMake found for LibGfx, so that there is one HarfBuzz in a process. LibGfx/TextLayout.cpp checks the layouts and
//! values here against the HarfBuzz headers.

use std::ffi::{c_int, c_uint, c_void};

/// `hb_buffer_t`
#[repr(C)]
pub struct HbBuffer {
    _private: [u8; 0],
}

/// `hb_font_t`
#[repr(C)]
pub struct HbFont {
    _private: [u8; 0],
}

/// `hb_draw_funcs_t`
#[repr(C)]
pub struct HbDrawFuncs {
    _private: [u8; 0],
}

/// `hb_unicode_funcs_t`
#[repr(C)]
pub struct HbUnicodeFuncs {
    _private: [u8; 0],
}

/// `hb_glyph_info_t`
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HbGlyphInfo {
    pub codepoint: u32,
    pub mask: u32,
    pub cluster: u32,
    pub var1: u32,
    pub var2: u32,
}

/// `hb_glyph_position_t`
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HbGlyphPosition {
    pub x_advance: i32,
    pub y_advance: i32,
    pub x_offset: i32,
    pub y_offset: i32,
    pub var: u32,
}

/// `hb_feature_t`
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HbFeature {
    pub tag: u32,
    pub value: u32,
    pub start: c_uint,
    pub end: c_uint,
}

/// `hb_glyph_extents_t`, in font units with y pointing up.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct HbGlyphExtents {
    pub x_bearing: i32,
    pub y_bearing: i32,
    pub width: i32,
    pub height: i32,
}

/// `hb_draw_state_t`
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct HbDrawState {
    pub path_open: c_int,
    pub path_start_x: f32,
    pub path_start_y: f32,
    pub current_x: f32,
    pub current_y: f32,
    pub reserved: [u32; 7],
}

/// `hb_draw_line_to_func_t`
pub type HbDrawLineToFunc = unsafe extern "C" fn(
    draw_funcs: *mut HbDrawFuncs,
    draw_data: *mut c_void,
    state: *mut HbDrawState,
    to_x: f32,
    to_y: f32,
    user_data: *mut c_void,
);

/// `hb_draw_quadratic_to_func_t`
pub type HbDrawQuadraticToFunc = unsafe extern "C" fn(
    draw_funcs: *mut HbDrawFuncs,
    draw_data: *mut c_void,
    state: *mut HbDrawState,
    control_x: f32,
    control_y: f32,
    to_x: f32,
    to_y: f32,
    user_data: *mut c_void,
);

/// `hb_draw_cubic_to_func_t`
pub type HbDrawCubicToFunc = unsafe extern "C" fn(
    draw_funcs: *mut HbDrawFuncs,
    draw_data: *mut c_void,
    state: *mut HbDrawState,
    control1_x: f32,
    control1_y: f32,
    control2_x: f32,
    control2_y: f32,
    to_x: f32,
    to_y: f32,
    user_data: *mut c_void,
);

/// `hb_destroy_func_t`
pub type HbDestroyFunc = unsafe extern "C" fn(user_data: *mut c_void);

// NB: The enumerations below are C enums, which are as wide as an int. They are passed and returned as u32.

/// `HB_DIRECTION_LTR` of `hb_direction_t`
pub const DIRECTION_LTR: u32 = 4;
/// `HB_DIRECTION_RTL` of `hb_direction_t`
pub const DIRECTION_RTL: u32 = 5;

/// `HB_SCRIPT_COMMON` of `hb_script_t`, the tag `Zyyy`
pub const SCRIPT_COMMON: u32 = 0x5A797979;
/// `HB_SCRIPT_INHERITED`, the tag `Zinh`
pub const SCRIPT_INHERITED: u32 = 0x5A696E68;
/// `HB_SCRIPT_LATIN`, the tag `Latn`
pub const SCRIPT_LATIN: u32 = 0x4C61746E;
/// `HB_SCRIPT_ARABIC`, the tag `Arab`
pub const SCRIPT_ARABIC: u32 = 0x41726162;
/// `HB_SCRIPT_HANIFI_ROHINGYA`, the tag `Rohg`
pub const SCRIPT_HANIFI_ROHINGYA: u32 = 0x526F6867;
/// `HB_SCRIPT_MANDAIC`, the tag `Mand`
pub const SCRIPT_MANDAIC: u32 = 0x4D616E64;
/// `HB_SCRIPT_MONGOLIAN`, the tag `Mong`
pub const SCRIPT_MONGOLIAN: u32 = 0x4D6F6E67;
/// `HB_SCRIPT_NKO`, the tag `Nkoo`
pub const SCRIPT_NKO: u32 = 0x4E6B6F6F;
/// `HB_SCRIPT_PHAGS_PA`, the tag `Phag`
pub const SCRIPT_PHAGS_PA: u32 = 0x50686167;
/// `HB_SCRIPT_SYRIAC`, the tag `Syrc`
pub const SCRIPT_SYRIAC: u32 = 0x53797263;

/// `HB_GLYPH_FLAG_UNSAFE_TO_BREAK` of `hb_glyph_flags_t`
pub const GLYPH_FLAG_UNSAFE_TO_BREAK: u32 = 0x1;
/// `HB_GLYPH_FLAG_DEFINED`, every flag the version defines
pub const GLYPH_FLAG_DEFINED: u32 = 0x7;

/// `HB_FEATURE_GLOBAL_END`
pub const FEATURE_GLOBAL_END: c_uint = 0xFFFFFFFF;

/// `HB_TAG()`: the four characters of a tag, the first in the highest byte.
#[must_use]
pub const fn tag(characters: [u8; 4]) -> u32 {
    u32::from_be_bytes(characters)
}

/// `hb_glyph_info_get_glyph_flags()`, as hb-buffer.h defines it in a macro.
#[inline]
#[must_use]
pub fn glyph_info_glyph_flags(info: &HbGlyphInfo) -> u32 {
    info.mask & GLYPH_FLAG_DEFINED
}

pub use ffi::*;

// NB: cbindgen would copy these declarations into RustFFI.h, next to the ones in hb.h that some of its includers see.
/// cbindgen:ignore
mod ffi {
    use super::*;

    unsafe extern "C" {
        pub fn hb_buffer_create() -> *mut HbBuffer;
        pub fn hb_buffer_destroy(buffer: *mut HbBuffer);
        pub fn hb_buffer_add_codepoints(
            buffer: *mut HbBuffer,
            text: *const u32,
            text_length: c_int,
            item_offset: c_uint,
            item_length: c_int,
        );
        pub fn hb_buffer_add_utf8(
            buffer: *mut HbBuffer,
            text: *const u8,
            text_length: c_int,
            item_offset: c_uint,
            item_length: c_int,
        );
        pub fn hb_buffer_add_latin1(
            buffer: *mut HbBuffer,
            text: *const u8,
            text_length: c_int,
            item_offset: c_uint,
            item_length: c_int,
        );
        pub fn hb_buffer_add_utf16(
            buffer: *mut HbBuffer,
            text: *const u16,
            text_length: c_int,
            item_offset: c_uint,
            item_length: c_int,
        );
        pub fn hb_buffer_set_script(buffer: *mut HbBuffer, script: u32);
        pub fn hb_buffer_set_direction(buffer: *mut HbBuffer, direction: u32);
        pub fn hb_buffer_get_direction(buffer: *const HbBuffer) -> u32;
        pub fn hb_buffer_guess_segment_properties(buffer: *mut HbBuffer);
        pub fn hb_buffer_get_glyph_infos(buffer: *mut HbBuffer, length: *mut c_uint) -> *mut HbGlyphInfo;
        pub fn hb_buffer_get_glyph_positions(buffer: *mut HbBuffer, length: *mut c_uint) -> *mut HbGlyphPosition;

        pub fn hb_shape(font: *mut HbFont, buffer: *mut HbBuffer, features: *const HbFeature, num_features: c_uint);

        pub fn hb_font_get_scale(font: *mut HbFont, x_scale: *mut c_int, y_scale: *mut c_int);
        pub fn hb_font_get_glyph_extents(font: *mut HbFont, glyph: u32, extents: *mut HbGlyphExtents) -> c_int;
        pub fn hb_font_draw_glyph(font: *mut HbFont, glyph: u32, draw_funcs: *mut HbDrawFuncs, draw_data: *mut c_void);

        pub fn hb_draw_funcs_create() -> *mut HbDrawFuncs;
        pub fn hb_draw_funcs_set_line_to_func(
            draw_funcs: *mut HbDrawFuncs,
            func: HbDrawLineToFunc,
            user_data: *mut c_void,
            destroy: Option<HbDestroyFunc>,
        );
        pub fn hb_draw_funcs_set_quadratic_to_func(
            draw_funcs: *mut HbDrawFuncs,
            func: HbDrawQuadraticToFunc,
            user_data: *mut c_void,
            destroy: Option<HbDestroyFunc>,
        );
        pub fn hb_draw_funcs_set_cubic_to_func(
            draw_funcs: *mut HbDrawFuncs,
            func: HbDrawCubicToFunc,
            user_data: *mut c_void,
            destroy: Option<HbDestroyFunc>,
        );
        pub fn hb_draw_funcs_make_immutable(draw_funcs: *mut HbDrawFuncs);

        pub fn hb_unicode_funcs_get_default() -> *mut HbUnicodeFuncs;
        pub fn hb_unicode_script(unicode_funcs: *mut HbUnicodeFuncs, code_point: u32) -> u32;
    }
}
