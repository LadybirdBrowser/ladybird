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
    /// A CSS `<number>`.
    Number { value: f64 },
    /// A CSS `<integer>`.
    Integer { value: i32 },
    /// A CSS `<angle>`. The unit is the C++ `enum class AngleUnit : u8`, opaque to Rust.
    Angle { value: f64, unit: u8 },
    /// A CSS `<flex>`. The unit is the C++ `enum class FlexUnit : u8`, opaque to Rust.
    Flex { value: f64, unit: u8 },
    /// A CSS `<frequency>`. The unit is the C++ `enum class FrequencyUnit : u8`, opaque to Rust.
    Frequency { value: f64, unit: u8 },
    /// A CSS `<length>`. The unit is the C++ `enum class LengthUnit : u8`, opaque to Rust.
    Length { value: f64, unit: u8 },
    /// A CSS `<percentage>`.
    Percentage { value: f64 },
    /// A CSS `<resolution>`. The unit is the C++ `enum class ResolutionUnit : u8`, opaque to Rust.
    Resolution { value: f64, unit: u8 },
    /// A CSS `<time>`. The unit is the C++ `enum class TimeUnit : u8`, opaque to Rust.
    Time { value: f64, unit: u8 },
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_keyword(keyword: u16) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Keyword { keyword })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_number(value: f64) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Number { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_integer(value: i32) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Integer { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_angle(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Angle { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_flex(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Flex { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_frequency(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Frequency { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_length(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Length { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_percentage(value: f64) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Percentage { value })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_resolution(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Resolution { value, unit })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_time(value: f64, unit: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::Time { value, unit })))
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
