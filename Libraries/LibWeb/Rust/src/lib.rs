/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

// The browser transfers HTML buffers to C++, so both sides must use the same allocator.
#[cfg(feature = "allocator")]
extern crate ladybird_allocator;

#[path = "../../../RustDemangle.rs"]
mod rust_demangle;

#[path = "../../../RustPanic.rs"]
mod rust_panic;

pub(crate) mod cow_column;
pub(crate) mod cow_map;
mod encoding_detection;
pub(crate) mod fork;
pub(crate) mod fork_vec;
#[cfg(test)]
mod gfx_test_stubs;
pub(crate) mod heap_size;
pub(crate) use libcompositing_rust::fast_hash;

pub(crate) mod css;
pub(crate) mod layout;
pub(crate) mod paint_stage;
pub(crate) mod painting;
pub(crate) mod render_state;
pub(crate) mod stage;
pub(crate) mod stage_thread;
pub(crate) mod style_layout_pool;
pub(crate) mod svg;

// What libweb_bindings_rust, whose interfaces are implemented in Rust, uses of this crate.
pub use css::font_feature_values::FontFeatureValuesRuleKind;
pub use css::media_list::{MediaList as NativeMediaList, MediaListDeleteResult};
pub use css::rule::cssom;
pub use css::rule::{NativeRule, NativeRuleList, NativeRuleType, RuleInsertionResult, RuleRemovalResult};
pub use css::style_sheet::NativeStyleSheetFlag;

/// The names of the CSS properties and of their legacy aliases, which have an accessor on CSSStyleProperties.
pub fn css_property_names() -> impl Iterator<Item = &'static str> {
    css::property_metadata::PROPERTY_NAME_LOOKUP
        .iter()
        .map(|(name, _)| *name)
}
pub use css::serialize::TextSink;
pub use css::typed_om;
pub use svg::gfx_path_from_path_data;

/// The red, green, blue and alpha of a CSS <color> in ASCII that needs nothing to be resolved against, or None for any
/// other value, as the color cache of the 2D canvas contexts parses it.
pub fn parse_simple_color_ascii(source: &[u8]) -> Option<[u8; 4]> {
    css::parser::color_parser::parse_simple_color(source.into())
}

/// The red, green, blue and alpha of a CSS <color> in UTF-16 that needs nothing to be resolved against, or None for any
/// other value, as the color cache of the 2D canvas contexts parses it.
pub fn parse_simple_color_utf16(source: &[u16]) -> Option<[u8; 4]> {
    css::parser::color_parser::parse_simple_color(source.into())
}

/// The red, green, blue and alpha of an 8-bit sRGB color converted to the predefined color space of a 2D canvas (the
/// underlying value of a Web::Bindings::PredefinedColorSpace), and clipped to its gamut, as the 'relative-colorimetric'
/// rendering intent does. https://html.spec.whatwg.org/multipage/canvas.html#predefinedcolorspace
pub fn srgb_color_in_predefined_color_space(color: [u8; 4], predefined_color_space: u8) -> [u8; 4] {
    css::color_resolution::srgb_color_in_predefined_color_space(color, predefined_color_space)
}

// NB: Only C++ calls into the HTML tokenizer and parser, so nothing in this crate names them. Linking the crate keeps
//     their extern "C" functions in the static library.
extern crate libweb_html_tokenizer;

use crate::rust_panic::abort_on_panic;

unsafe fn bytes_from_raw<'a>(bytes: *const u8, len: usize) -> Option<&'a [u8]> {
    unsafe {
        if len == 0 {
            return Some(&[]);
        }
        if bytes.is_null() {
            eprintln!("bytes_from_raw: null pointer with non-zero length {len}");
            return None;
        }
        Some(ak::slice_from_raw_parts(bytes, len))
    }
}
