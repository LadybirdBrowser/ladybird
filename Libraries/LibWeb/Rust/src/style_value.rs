/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust-owned CSS style value data.
//!
//! The C++ StyleValue subclasses are being converted, one type at a time, to keep their data in
//! a Rust-owned [`StyleValueData`] allocation instead of C++ member variables. Each converted
//! subclass owns its allocation uniquely and destroys it with [`rust_style_value_destroy`]. The
//! layout of [`StyleValueData`] is exposed to C++ through cbindgen so that hot accessors compile
//! to inline field reads with no FFI call.

use crate::abort_on_panic;

/// The data of a single immutable CSS style value.
///
/// Variant payload fields are read directly by the corresponding C++ StyleValue subclass, so
/// changing a payload changes the C++ accessors reading it.
#[repr(C, u8)]
// NB: Variant payload fields are only read by C++ through the exposed layout.
#[allow(dead_code)]
pub enum StyleValueData {
    /// A CSS keyword. The value is the generated C++ `enum class Keyword : u16`, opaque to Rust.
    Keyword { keyword: u16 },
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_keyword(keyword: u16) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Keyword { keyword })))
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_destroy(value: *mut StyleValueData) {
    abort_on_panic(|| {
        if value.is_null() {
            return;
        }
        drop(unsafe { Box::from_raw(value) });
    });
}
