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

use std::ffi::c_void;

use crate::abort_on_panic;

unsafe extern "C" {
    fn ladybird_style_value_unref(style_value: *const c_void);
    fn ladybird_utf16_fly_string_unref(raw: usize);
}

/// A strong reference to a C++ StyleValue held from Rust-owned value data.
///
/// While the StyleValue subclasses are converted one type at a time, nested values still point
/// at C++ objects; dropping the Rust allocation releases the reference. Once every type is
/// converted these become references between Rust allocations instead.
#[repr(C)]
pub struct RetainedStyleValue {
    pointer: *const c_void,
}

impl Drop for RetainedStyleValue {
    fn drop(&mut self) {
        // A null pointer represents an absent optional reference.
        if !self.pointer.is_null() {
            unsafe { ladybird_style_value_unref(self.pointer) };
        }
    }
}

/// A retained AK::Utf16FlyString, stored as its one-word raw representation. Owns one reference
/// to the underlying string data unless it is a short string, which needs none; the C++ bridge
/// handles both cases.
#[repr(C)]
pub struct RetainedUtf16FlyString {
    raw: usize,
}

impl Drop for RetainedUtf16FlyString {
    fn drop(&mut self) {
        unsafe { ladybird_utf16_fly_string_unref(self.raw) };
    }
}

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
    /// A CSS `<ratio>`, e.g. `16 / 9`. The numerator and denominator are style values.
    Ratio {
        numerator: RetainedStyleValue,
        denominator: RetainedStyleValue,
    },
    /// A unicode-range, e.g. `U+0025-00FF`.
    UnicodeRange { min_code_point: u32, max_code_point: u32 },
    /// A CSS `<opacity-value>`: a number, percentage or calculated style value.
    OpacityValue { value: RetainedStyleValue },
    /// One edge of a CSS `<position>`: an optional edge keyword (the C++ `enum class
    /// PositionEdge : u8`, opaque to Rust) and an optional offset style value (null when absent).
    Edge {
        has_edge: bool,
        edge: u8,
        offset: RetainedStyleValue,
    },
    /// The guaranteed-invalid value: https://drafts.csswg.org/css-variables/#guaranteed-invalid-value
    GuaranteedInvalid,
    /// An absent optional value in a shorthand.
    EmptyOptional,
    /// grid-auto-flow.
    GridAutoFlow { row: bool, dense: bool },
    /// text-underline-position. Both fields are C++ `enum class ... : u8` values, opaque to Rust.
    TextUnderlinePosition { horizontal: u8, vertical: u8 },
    /// contrast-color() with its retained color style value.
    ContrastColor { color: RetainedStyleValue },
    /// superellipse() with its retained parameter style value.
    Superellipse { parameter: RetainedStyleValue },
    /// A pending-substitution value retaining the shorthand value it came from.
    PendingSubstitution {
        original_shorthand_value: RetainedStyleValue,
    },
    /// scrollbar-color with retained thumb and track color values.
    ScrollbarColor {
        thumb_color: RetainedStyleValue,
        track_color: RetainedStyleValue,
    },
    /// rect() with four retained edge style values.
    Rect {
        top: RetainedStyleValue,
        right: RetainedStyleValue,
        bottom: RetainedStyleValue,
        left: RetainedStyleValue,
    },
    /// A CSS `<string>`.
    String { string: RetainedUtf16FlyString },
    /// A CSS `<custom-ident>`.
    CustomIdent { custom_ident: RetainedUtf16FlyString },
    /// A border-radius rect of four retained corner radius style values.
    BorderRadiusRect {
        top_left: RetainedStyleValue,
        top_right: RetainedStyleValue,
        bottom_right: RetainedStyleValue,
        bottom_left: RetainedStyleValue,
    },
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

/// Takes ownership of one strong reference to each of the numerator and denominator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_ratio(
    numerator: *const c_void,
    denominator: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Ratio {
            numerator: RetainedStyleValue { pointer: numerator },
            denominator: RetainedStyleValue { pointer: denominator },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_unicode_range(
    min_code_point: u32,
    max_code_point: u32,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::UnicodeRange {
            min_code_point,
            max_code_point,
        }))
    })
}

/// Takes ownership of one strong reference to the value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_opacity_value(value: *const c_void) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::OpacityValue {
            value: RetainedStyleValue { pointer: value },
        }))
    })
}

/// Takes ownership of one strong reference to the offset if it is non-null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_edge(
    has_edge: bool,
    edge: u8,
    offset: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Edge {
            has_edge,
            edge,
            offset: RetainedStyleValue { pointer: offset },
        }))
    })
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_guaranteed_invalid() -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::GuaranteedInvalid)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_empty_optional() -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::EmptyOptional)))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_grid_auto_flow(row: bool, dense: bool) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::GridAutoFlow { row, dense })))
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_text_underline_position(horizontal: u8, vertical: u8) -> *mut StyleValueData {
    abort_on_panic(|| Box::into_raw(Box::new(StyleValueData::TextUnderlinePosition { horizontal, vertical })))
}

/// Takes ownership of one strong reference to the color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_contrast_color(color: *const c_void) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ContrastColor {
            color: RetainedStyleValue { pointer: color },
        }))
    })
}

/// Takes ownership of one strong reference to the parameter.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_superellipse(parameter: *const c_void) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Superellipse {
            parameter: RetainedStyleValue { pointer: parameter },
        }))
    })
}

/// Takes ownership of one strong reference to the original shorthand value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_pending_substitution(
    original_shorthand_value: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::PendingSubstitution {
            original_shorthand_value: RetainedStyleValue {
                pointer: original_shorthand_value,
            },
        }))
    })
}

/// Takes ownership of one strong reference to each color.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_scrollbar_color(
    thumb_color: *const c_void,
    track_color: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::ScrollbarColor {
            thumb_color: RetainedStyleValue { pointer: thumb_color },
            track_color: RetainedStyleValue { pointer: track_color },
        }))
    })
}

/// Takes ownership of one strong reference to each edge.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_rect(
    top: *const c_void,
    right: *const c_void,
    bottom: *const c_void,
    left: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::Rect {
            top: RetainedStyleValue { pointer: top },
            right: RetainedStyleValue { pointer: right },
            bottom: RetainedStyleValue { pointer: bottom },
            left: RetainedStyleValue { pointer: left },
        }))
    })
}

/// Takes ownership of one strong reference to each corner radius.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_style_value_create_border_radius_rect(
    top_left: *const c_void,
    top_right: *const c_void,
    bottom_right: *const c_void,
    bottom_left: *const c_void,
) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::BorderRadiusRect {
            top_left: RetainedStyleValue { pointer: top_left },
            top_right: RetainedStyleValue { pointer: top_right },
            bottom_right: RetainedStyleValue { pointer: bottom_right },
            bottom_left: RetainedStyleValue { pointer: bottom_left },
        }))
    })
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_string(string: usize) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::String {
            string: RetainedUtf16FlyString { raw: string },
        }))
    })
}

/// Takes ownership of one leaked reference to the string.
#[unsafe(no_mangle)]
pub extern "C" fn rust_style_value_create_custom_ident(custom_ident: usize) -> *mut StyleValueData {
    abort_on_panic(|| {
        Box::into_raw(Box::new(StyleValueData::CustomIdent {
            custom_ident: RetainedUtf16FlyString { raw: custom_ident },
        }))
    })
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
