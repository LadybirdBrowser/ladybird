/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stand-ins for the LibGfx font, shaping and path entry points, which a unit test links through a document's render
//! state, without the C++ runtime, but never reaches.

use libgfx_rust::text_layout::{DrawGlyph, TextType};
use std::ffi::c_void;

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_is_emoji_font(_font: *const c_void) -> bool {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_emoji_presentation_for_code_point(
    _code_point: u32,
    _next_code_point: u32,
    _has_next_code_point: bool,
) -> u8 {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_contains_glyph(_font: *const c_void, _code_point: u32) -> bool {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_glyph_id(_font: *const c_void, _code_point: u32) -> u32 {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_invisible_variant(_font: *const c_void) -> *const c_void {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_system_fallback_font(
    _code_point: u32,
    _weight: u16,
    _width: u16,
    _slope: u8,
    _prefer_color_emoji: bool,
    _point_size: f32,
) -> *const c_void {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_font_measure_text_width(
    _font: *const c_void,
    _text_utf16: *const u16,
    _length_in_code_units: usize,
) -> f32 {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_process_note_wanted_pending_face(_face_id: u64) {
    unreachable!("no unit test reads a font");
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
extern "C" fn ladybird_gfx_shape_text_uncached(
    _font: *const c_void,
    _text_utf16: *const u16,
    _length_in_code_units: usize,
    _text_type: TextType,
    _letter_spacing: f32,
    _word_spacing: f32,
    _sink: *mut c_void,
    _emit: unsafe extern "C" fn(*mut c_void, *const DrawGlyph, usize, f32, usize, f32),
) {
    unreachable!("no unit test shapes text");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_glyph_run_bounding_box(
    _font: *const c_void,
    _glyphs: *const DrawGlyph,
    _glyph_count: usize,
    _scale: f32,
    _out_rect: *mut f32,
) {
    unreachable!("no unit test shapes text");
}

#[unsafe(no_mangle)]
#[allow(clippy::too_many_arguments)]
extern "C" fn ladybird_gfx_glyph_run_glyph_intercepts(
    _font: *const c_void,
    _glyphs: *const DrawGlyph,
    _glyph_count: usize,
    _scale: f32,
    _y_top: f32,
    _y_bottom: f32,
    _sink: *mut c_void,
    _push: unsafe extern "C" fn(*mut c_void, f32),
) {
    unreachable!("no unit test shapes text");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_create_from_ops(_kinds: *const u8, _values: *const f32, _count: usize) -> *mut c_void {
    unreachable!("no unit test builds a path");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_create_from_glyph_runs(_runs: *const c_void, _run_count: usize) -> *mut c_void {
    unreachable!("no unit test builds a path");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_copy_transformed(_path: *const c_void, _affine_values: *const f32) -> *mut c_void {
    unreachable!("no unit test builds a path");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_length(_path: *const c_void) -> f32 {
    unreachable!("no unit test builds a path");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_place_glyph_runs_along(
    _path: *const c_void,
    _runs: *const c_void,
    _run_count: usize,
    _offset: f32,
) -> *mut c_void {
    unreachable!("no unit test builds a path");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_set_fill_type(_path: *mut c_void, _winding_rule: i32) {
    unreachable!("no unit test builds a path");
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_process_next_path_identity() -> u64 {
    unreachable!("no unit test builds a path");
}

#[unsafe(no_mangle)]
extern "C" fn web_render_clock_hand_pointer_move(
    _context: u64,
    _has_position: bool,
    _x: f32,
    _y: f32,
    _buttons: u32,
    _scrolled_since_frame: bool,
    _input_event_id: u64,
) {
    unreachable!("no unit test hands a render clock a pointer move");
}
