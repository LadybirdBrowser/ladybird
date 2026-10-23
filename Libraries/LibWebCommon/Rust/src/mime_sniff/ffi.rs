/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The C ABI through which LibWebCommon's MimeSniff::MimeType and MimeSniff::Resource reach the crate. C++ lends its
//! MIME types to the crate as a [`FfiMimeType`] of spans into its strings, and the crate lends its own back to a
//! callback the same way.

use core::ffi::c_void;

use ak::Span;

use super::mime_type::{Essence, MimeType, is_javascript_mime_type_essence_match};
use super::resource::{SniffingContext, sniff};
use crate::rust_panic::abort_on_panic;

/// A string that C++ lends as UTF-8 or as UTF-16, of `length` code units.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct FfiStringInput {
    pub utf8: *const u8,
    pub utf16: *const u16,
    pub length: usize,
}

impl FfiStringInput {
    /// The code points of the string, with each unpaired surrogate replaced by U+FFFD, or `None` for bytes that are
    /// not UTF-8.
    ///
    /// # Safety
    ///
    /// The pointer that is not null must lend `length` code units.
    unsafe fn code_points(self) -> Option<Vec<char>> {
        if self.length == 0 {
            return Some(Vec::new());
        }
        if !self.utf16.is_null() {
            // SAFETY: The caller lends `length` UTF-16 code units.
            let units = unsafe { ak::slice_from_raw_parts(self.utf16, self.length) };
            return Some(
                char::decode_utf16(units.iter().copied())
                    .map(|code_point| code_point.unwrap_or(char::REPLACEMENT_CHARACTER))
                    .collect(),
            );
        }
        // SAFETY: The caller lends `length` bytes.
        let bytes = unsafe { ak::slice_from_raw_parts(self.utf8, self.length) };
        Some(std::str::from_utf8(bytes).ok()?.chars().collect())
    }
}

#[repr(C)]
pub struct FfiMimeTypeParameter<'a> {
    pub name: Span<'a, u8>,
    pub value: Span<'a, u8>,
}

/// A MIME type whose strings, which are UTF-8, its lender keeps alive.
#[repr(C)]
pub struct FfiMimeType<'a> {
    pub type_: Span<'a, u8>,
    pub subtype: Span<'a, u8>,
    pub parameters: Span<'a, FfiMimeTypeParameter<'a>>,
}

pub(crate) type OnMimeType = unsafe extern "C" fn(ctx: *mut c_void, mime_type: *const FfiMimeType<'_>);
type OnString = unsafe extern "C" fn(ctx: *mut c_void, string: Span<'_, u8>);

#[repr(u8)]
pub enum MimeTypeGroup {
    Image,
    AudioOrVideo,
    Font,
    Xml,
    Html,
    JavaScript,
    Json,
}

fn str_from_span(span: Span<'_, u8>) -> &str {
    std::str::from_utf8(span.as_slice()).expect("C++ lends UTF-8 strings")
}

fn essence<'a>(type_: Span<'a, u8>, subtype: Span<'a, u8>) -> Essence<'a> {
    Essence {
        type_: str_from_span(type_),
        subtype: str_from_span(subtype),
    }
}

impl FfiMimeType<'_> {
    fn to_mime_type(&self) -> MimeType {
        let mut mime_type = MimeType::new(
            str_from_span(self.type_).to_owned(),
            str_from_span(self.subtype).to_owned(),
        );
        for parameter in self.parameters.as_slice() {
            mime_type.set_parameter(
                str_from_span(parameter.name).to_owned(),
                str_from_span(parameter.value).to_owned(),
            );
        }
        mime_type
    }
}

/// # Safety
///
/// `on_mime_type` must be safe to call with `ctx`.
pub(crate) unsafe fn lend_mime_type(mime_type: &MimeType, ctx: *mut c_void, on_mime_type: OnMimeType) {
    let parameters: Vec<_> = mime_type
        .parameters()
        .map(|(name, value)| FfiMimeTypeParameter {
            name: Span::new(name.as_bytes()),
            value: Span::new(value.as_bytes()),
        })
        .collect();
    let ffi_mime_type = FfiMimeType {
        type_: Span::new(mime_type.type_().as_bytes()),
        subtype: Span::new(mime_type.subtype().as_bytes()),
        parameters: Span::new(&parameters),
    };
    // SAFETY: The caller passes a callback for `ctx`, and the MIME type outlives the call.
    unsafe { on_mime_type(ctx, &raw const ffi_mime_type) };
}

/// Parses the input as a MIME type, and calls `on_mime_type` with the MIME type if that succeeds. Returns whether it
/// succeeded.
///
/// # Safety
///
/// `input` must lend its code units for the call, and `on_mime_type` must be safe to call with `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mime_sniff_parse_mime_type(
    input: FfiStringInput,
    ctx: *mut c_void,
    on_mime_type: OnMimeType,
) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller lends the code units of the input.
        let Some(mime_type) = unsafe { input.code_points() }.as_deref().and_then(MimeType::parse) else {
            return false;
        };
        // SAFETY: The caller passes a callback for `ctx`.
        unsafe { lend_mime_type(&mime_type, ctx, on_mime_type) };
        true
    })
}

/// Calls `on_serialization` with the serialization of the MIME type.
///
/// # Safety
///
/// `mime_type` must be a valid MIME type for the call, and `on_serialization` must be safe to call with `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mime_sniff_serialize_mime_type(
    mime_type: *const FfiMimeType<'_>,
    ctx: *mut c_void,
    on_serialization: OnString,
) {
    abort_on_panic(|| {
        // SAFETY: The caller lends a MIME type for the call.
        let serialization = unsafe { &*mime_type }.to_mime_type().serialize();
        // SAFETY: The caller passes a callback for `ctx`.
        unsafe { on_serialization(ctx, Span::new(serialization.as_bytes())) };
    });
}

/// Whether the MIME type with the type and subtype belongs to the group.
#[unsafe(no_mangle)]
pub extern "C" fn mime_sniff_mime_type_is_in_group(
    group: MimeTypeGroup,
    type_: Span<'_, u8>,
    subtype: Span<'_, u8>,
) -> bool {
    abort_on_panic(|| {
        let essence = essence(type_, subtype);
        match group {
            MimeTypeGroup::Image => essence.is_image(),
            MimeTypeGroup::AudioOrVideo => essence.is_audio_or_video(),
            MimeTypeGroup::Font => essence.is_font(),
            MimeTypeGroup::Xml => essence.is_xml(),
            MimeTypeGroup::Html => essence.is_html(),
            MimeTypeGroup::JavaScript => essence.is_javascript(),
            MimeTypeGroup::Json => essence.is_json(),
        }
    })
}

/// Calls `on_minimized` with the minimized MIME type of the MIME type with the type and subtype.
///
/// # Safety
///
/// `on_minimized` must be safe to call with `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mime_sniff_minimize_a_supported_mime_type(
    type_: Span<'_, u8>,
    subtype: Span<'_, u8>,
    ctx: *mut c_void,
    on_minimized: OnString,
) {
    abort_on_panic(|| {
        let minimized = essence(type_, subtype).minimize_a_supported_mime_type();
        // SAFETY: The caller passes a callback for `ctx`.
        unsafe { on_minimized(ctx, Span::new(minimized.as_bytes())) };
    });
}

/// # Safety
///
/// `string` must lend its code units for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mime_sniff_is_javascript_mime_type_essence_match(string: FfiStringInput) -> bool {
    abort_on_panic(|| {
        // SAFETY: The caller lends the code units of the string.
        unsafe { string.code_points() }.is_some_and(|code_points| is_javascript_mime_type_essence_match(&code_points))
    })
}

/// Calls `on_mime_type` with the computed MIME type of the resource whose data is `data`. `supplied_type` is null if
/// there is no supplied MIME type.
///
/// # Safety
///
/// `supplied_type` must be null or a valid MIME type for the call, `scheme` must be UTF-8, and `on_mime_type` must be
/// safe to call with `ctx`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn mime_sniff_sniff(
    data: Span<'_, u8>,
    sniffing_context: SniffingContext,
    scheme: Span<'_, u8>,
    supplied_type: *const FfiMimeType<'_>,
    no_sniff: bool,
    ctx: *mut c_void,
    on_mime_type: OnMimeType,
) {
    abort_on_panic(|| {
        // SAFETY: The caller lends a MIME type for the call, if any.
        let supplied_type = unsafe { supplied_type.as_ref() }.map(FfiMimeType::to_mime_type);
        let computed_mime_type = sniff(
            data.as_slice(),
            sniffing_context,
            str_from_span(scheme),
            supplied_type,
            no_sniff,
        );
        // SAFETY: The caller passes a callback for `ctx`.
        unsafe { lend_mime_type(&computed_mime_type, ctx, on_mime_type) };
    });
}
