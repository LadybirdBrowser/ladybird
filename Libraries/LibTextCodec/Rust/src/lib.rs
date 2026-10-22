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

pub mod streaming_decoder;

pub use streaming_decoder::{DecodingFailed, StreamingDecoder};

use crate::rust_panic::abort_on_panic;
use ak::Utf16StringUnits;
use encoding_rs::CoderResult;
use encoding_rs::EncoderResult;
use encoding_rs::Encoding;
use std::ffi::c_void;

type FfiBytesFn = unsafe extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize);
type FfiUtf16Fn = unsafe extern "C" fn(ctx: *mut c_void, data: *const u16, len: usize);
type FfiCodePointFn = unsafe extern "C" fn(ctx: *mut c_void, code_point: u32);

pub struct TextCodecRustStreamingDecoder {
    decoder: StreamingDecoder,
}

/// Getting an encoding from a label, https://encoding.spec.whatwg.org/#concept-encoding-get
pub fn encoding_for_label(label: Utf16StringUnits<'_>) -> Option<&'static Encoding> {
    match label {
        Utf16StringUnits::Ascii(label) => Encoding::for_label(label),
        Utf16StringUnits::Utf16(label) => {
            // NB: Every label is ASCII, so a label with any other code unit is not one.
            let label: Option<Vec<u8>> = label.iter().map(|&unit| u8::try_from(unit).ok()).collect();
            Encoding::for_label(&label.filter(|label| label.is_ascii())?)
        }
    }
}

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

unsafe fn set_static_string(out: *mut *const u8, out_len: *mut usize, value: &'static str) -> bool {
    unsafe {
        if out.is_null() || out_len.is_null() {
            eprintln!("set_static_string: null output pointer");
            return false;
        }
        *out = value.as_ptr();
        *out_len = value.len();
        true
    }
}

/// # Safety
/// - `label`/`label_len` must be a valid byte slice.
/// - `out_name` and `out_name_len` must be valid writable pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_get_standardized_encoding(
    label: *const u8,
    label_len: usize,
    out_name: *mut *const u8,
    out_name_len: *mut usize,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            let Some(label) = bytes_from_raw(label, label_len) else {
                return false;
            };
            let Some(encoding) = Encoding::for_label(label) else {
                return false;
            };
            set_static_string(out_name, out_name_len, encoding.name())
        })
    }
}

/// # Safety
/// - `encoding_label`/`encoding_label_len` and `input`/`input_len` must be valid byte slices.
/// - `on_bytes` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_decode_to_utf8(
    encoding_label: *const u8,
    encoding_label_len: usize,
    input: *const u8,
    input_len: usize,
    fatal: bool,
    ctx: *mut c_void,
    on_bytes: FfiBytesFn,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            let Some(label) = bytes_from_raw(encoding_label, encoding_label_len) else {
                return false;
            };
            let Some(input) = bytes_from_raw(input, input_len) else {
                return false;
            };
            let Some(encoding) = Encoding::for_label(label) else {
                return false;
            };

            let (output, had_errors) = encoding.decode_without_bom_handling(input);
            if fatal && had_errors {
                return false;
            }
            on_bytes(ctx, output.as_bytes().as_ptr(), output.len());
            true
        })
    }
}

/// # Safety
/// - `encoding_label`/`encoding_label_len` and `input`/`input_len` must be valid byte slices.
/// - `on_utf16` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_decode_to_utf16(
    encoding_label: *const u8,
    encoding_label_len: usize,
    input: *const u8,
    input_len: usize,
    ctx: *mut c_void,
    on_utf16: FfiUtf16Fn,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            let Some(label) = bytes_from_raw(encoding_label, encoding_label_len) else {
                return false;
            };
            let Some(input) = bytes_from_raw(input, input_len) else {
                return false;
            };
            let Some(encoding) = Encoding::for_label(label) else {
                return false;
            };
            let Ok(output) = StreamingDecoder::new(encoding, false, true).decode(input, false) else {
                return false;
            };
            on_utf16(ctx, output.as_ptr(), output.len());
            true
        })
    }
}

/// # Safety
/// - `encoding_label`/`encoding_label_len` and `input`/`input_len` must be valid byte slices.
/// - `out_length` must be a valid writable pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_decoded_length_in_utf16_code_units(
    encoding_label: *const u8,
    encoding_label_len: usize,
    input: *const u8,
    input_len: usize,
    out_length: *mut usize,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            let Some(label) = bytes_from_raw(encoding_label, encoding_label_len) else {
                return false;
            };
            let Some(input) = bytes_from_raw(input, input_len) else {
                return false;
            };
            let Some(encoding) = Encoding::for_label(label) else {
                return false;
            };
            if out_length.is_null() {
                eprintln!("textcodec_rust_decoded_length_in_utf16_code_units: null output pointer");
                return false;
            }

            let mut decoder = encoding.new_decoder_without_bom_handling();
            let mut output = [0u16; 4096];
            let mut total_read = 0;
            let mut length = 0;
            loop {
                let (result, read, written, _) = decoder.decode_to_utf16(&input[total_read..], &mut output, true);
                total_read += read;
                length += written;
                if result == CoderResult::InputEmpty {
                    *out_length = length;
                    return true;
                }
            }
        })
    }
}

/// # Safety
/// - `encoding_label`/`encoding_label_len` must be a valid byte slice.
/// - The returned pointer must be freed with `textcodec_rust_streaming_decoder_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_streaming_decoder_new(
    encoding_label: *const u8,
    encoding_label_len: usize,
    fatal: bool,
    ignore_bom: bool,
) -> *mut TextCodecRustStreamingDecoder {
    unsafe {
        abort_on_panic(|| {
            let Some(label) = bytes_from_raw(encoding_label, encoding_label_len) else {
                return std::ptr::null_mut();
            };
            let Some(encoding) = Encoding::for_label(label) else {
                return std::ptr::null_mut();
            };
            let decoder = StreamingDecoder::new(encoding, fatal, ignore_bom);
            Box::into_raw(Box::new(TextCodecRustStreamingDecoder { decoder }))
        })
    }
}

/// # Safety
/// - `decoder` must be null or a pointer returned by `textcodec_rust_streaming_decoder_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_streaming_decoder_free(decoder: *mut TextCodecRustStreamingDecoder) {
    unsafe {
        abort_on_panic(|| {
            if !decoder.is_null() {
                drop(Box::from_raw(decoder));
            }
        });
    }
}

/// # Safety
/// - `decoder` must be a valid pointer returned by `textcodec_rust_streaming_decoder_new`.
/// - `input`/`input_len` must be a valid byte slice.
/// - `on_utf16` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_streaming_decoder_decode_to_utf16(
    decoder: *mut TextCodecRustStreamingDecoder,
    input: *const u8,
    input_len: usize,
    last: bool,
    ctx: *mut c_void,
    on_utf16: FfiUtf16Fn,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            if decoder.is_null() {
                eprintln!("textcodec_rust_streaming_decoder_decode_to_utf16: null decoder pointer");
                return false;
            }
            let Some(input) = bytes_from_raw(input, input_len) else {
                return false;
            };
            let Ok(output) = (*decoder).decoder.decode(input, !last) else {
                return false;
            };
            if !output.is_empty() {
                on_utf16(ctx, output.as_ptr(), output.len());
            }
            true
        })
    }
}

/// # Safety
/// - `encoding_label`/`encoding_label_len` and `input`/`input_len` must be valid byte slices.
/// - `on_bytes` and `on_error` must not retain data beyond their callback duration.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_encode_from_utf8(
    encoding_label: *const u8,
    encoding_label_len: usize,
    input: *const u8,
    input_len: usize,
    ctx: *mut c_void,
    on_bytes: FfiBytesFn,
    on_error: FfiCodePointFn,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            let Some(label) = bytes_from_raw(encoding_label, encoding_label_len) else {
                return false;
            };
            let Some(input) = bytes_from_raw(input, input_len) else {
                return false;
            };
            let Ok(input) = std::str::from_utf8(input) else {
                return false;
            };
            let Some(encoding) = Encoding::for_label(label) else {
                return false;
            };

            let mut encoder = encoding.new_encoder();
            let mut total_read = 0usize;

            // NB: A fixed-size buffer keeps the work per call proportional to the input it consumes. Sizing the
            //     buffer for the whole input would cost that much again after every unmappable code point.
            let mut output = [0u8; 4096];

            loop {
                let (result, read, written) =
                    encoder.encode_from_utf8_without_replacement(&input[total_read..], &mut output, true);
                total_read += read;

                if written > 0 {
                    on_bytes(ctx, output.as_ptr(), written);
                }

                match result {
                    EncoderResult::InputEmpty => return true,
                    EncoderResult::OutputFull => {}
                    EncoderResult::Unmappable(unmappable) => {
                        on_error(ctx, unmappable as u32);
                    }
                }
            }
        })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utf16(string: &str) -> Vec<u16> {
        string.encode_utf16().collect()
    }

    #[test]
    fn labels() {
        let name = |label: Utf16StringUnits<'_>| encoding_for_label(label).map(Encoding::name);
        assert_eq!(name(Utf16StringUnits::Ascii(b" Latin1\n")), Some("windows-1252"));
        assert_eq!(name(Utf16StringUnits::Ascii(b"nonsense")), None);
        assert_eq!(name(Utf16StringUnits::Utf16(&utf16("utf-8"))), Some("UTF-8"));
        assert_eq!(name(Utf16StringUnits::Utf16(&utf16("\u{FC}tf-8"))), None);
        assert_eq!(name(Utf16StringUnits::Utf16(&utf16("\u{175}tf-8"))), None);
    }
}
