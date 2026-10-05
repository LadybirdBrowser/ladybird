/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

pub mod bsp_tree;
pub mod color;
pub mod corner_radii;
pub mod filter;
pub mod font;
pub mod font_catalog;
pub mod geometry;
pub mod image_frame;
pub mod matrix;
pub mod paint_enums;
pub mod path;
pub mod text_layout;
pub mod yuv;

pub use color::*;
pub use corner_radii::*;
pub use geometry::*;
pub use matrix::*;
pub use paint_enums::*;

/// Reports this copy of the crate to LibGfx.
///
/// The crate is compiled into more than one library, so there is more than one of everything in
/// it. That is only a problem for state, which is why none of it lives here (see
/// `LibGfx/RustProcessState.cpp`), but a test should be able to prove that the shared state is not
/// one of the copies. Each copy has a marker of its own, and registering its address is how LibGfx
/// counts them.
///
/// `MARKER` is immutable and per copy on purpose.
#[unsafe(no_mangle)]
pub extern "C" fn ladybird_gfx_register_rust_crate_copy() {
    static MARKER: u8 = 0;
    // SAFETY: The address of a `static` outlives the process.
    unsafe { ladybird_gfx_process_note_crate_copy((&raw const MARKER).cast()) };
}

unsafe extern "C" {
    fn ladybird_gfx_process_note_crate_copy(marker: *const std::ffi::c_void);
}

// The handles that layout and painting keep in their output, so that output can cross threads.
const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<font::FontHandle>();
    assert_send_and_sync::<path::OwnedPath>();
    assert_send_and_sync::<image_frame::ImageFrameHandle>();
    assert_send_and_sync::<text_layout::GlyphBuffer>();
};
