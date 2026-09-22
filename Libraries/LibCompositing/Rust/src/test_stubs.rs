/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Stand-ins for the LibGfx path entry points, which the unit tests link without the C++ runtime.

use std::ffi::c_void;

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_destroy(path: *mut c_void) {
    if !path.is_null() {
        // SAFETY: Every stand-in path comes from the `Box<u8>` the deserializer stub leaked to the caller.
        drop(unsafe { Box::from_raw(path.cast::<u8>()) });
    }
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_serialize(
    _path: *const c_void,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    context: *mut c_void,
) {
    let stand_in_path_bytes = [7u8, 8, 9];
    // SAFETY: The serializer hands its own sink and append function; the bytes are live for the call.
    unsafe { append(context, stand_in_path_bytes.as_ptr(), stand_in_path_bytes.len()) };
}

// A stand-in path remembers the first byte it was created from, so tests can tell paths apart.
#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_create_from_serialized_bytes(bytes: *const u8, count: usize) -> *mut c_void {
    // SAFETY: The caller hands `count` readable bytes.
    let content = if count == 0 { 0u8 } else { unsafe { *bytes } };
    Box::into_raw(Box::new(content)).cast()
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_equals(a: *const c_void, b: *const c_void) -> bool {
    // SAFETY: Both stand-in paths are the leaked `Box<u8>` the stub above created.
    unsafe { *a.cast::<u8>() == *b.cast::<u8>() }
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_bounding_box(_path: *const c_void, out_x_y_width_height: *mut f32) {
    let stand_in_bounding_box = [0.0f32, 0.0, 1.0, 1.0];
    // SAFETY: The caller hands a writable array of four floats.
    unsafe { std::ptr::copy_nonoverlapping(stand_in_bounding_box.as_ptr(), out_x_y_width_height, 4) };
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_contains(_path: *const c_void, _x: f32, _y: f32, _winding_rule: i32) -> bool {
    false
}

#[unsafe(no_mangle)]
extern "C" fn ladybird_gfx_path_append_svg_string(
    _path: *const c_void,
    _append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
    _context: *mut c_void,
) {
}
