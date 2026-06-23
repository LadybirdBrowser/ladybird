/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use encoding_rs::CoderResult;
use encoding_rs::DecoderResult;
use encoding_rs::EncoderResult;
use encoding_rs::Encoding;
use std::ffi::c_void;
use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

type FfiBytesFn = unsafe extern "C" fn(ctx: *mut c_void, data: *const u8, len: usize);
type FfiCodePointFn = unsafe extern "C" fn(ctx: *mut c_void, code_point: u32);

pub struct TextCodecRustStreamingDecoder {
    decoder: encoding_rs::Decoder,
}

fn abort_on_panic<F: FnOnce() -> R, R>(f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => {
            let message = if let Some(message) = payload.downcast_ref::<&str>() {
                (*message).to_string()
            } else if let Some(message) = payload.downcast_ref::<String>() {
                message.clone()
            } else {
                "unknown panic".to_string()
            };
            eprintln!("Rust panic at FFI boundary: {message}");
            std::process::abort();
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
        Some(std::slice::from_raw_parts(bytes, len))
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
    remove_bom: bool,
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

            let (output, had_errors) = if remove_bom {
                encoding.decode_with_bom_removal(input)
            } else {
                encoding.decode_without_bom_handling(input)
            };
            if fatal && had_errors {
                return false;
            }
            on_bytes(ctx, output.as_bytes().as_ptr(), output.len());
            true
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
    remove_bom: bool,
) -> *mut TextCodecRustStreamingDecoder {
    unsafe {
        abort_on_panic(|| {
            let Some(label) = bytes_from_raw(encoding_label, encoding_label_len) else {
                return std::ptr::null_mut();
            };
            let Some(encoding) = Encoding::for_label(label) else {
                return std::ptr::null_mut();
            };
            let decoder = if remove_bom {
                encoding.new_decoder_with_bom_removal()
            } else {
                encoding.new_decoder_without_bom_handling()
            };
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
/// - `on_bytes` must not retain `data` beyond the duration of the callback.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn textcodec_rust_streaming_decoder_decode_to_utf8(
    decoder: *mut TextCodecRustStreamingDecoder,
    input: *const u8,
    input_len: usize,
    last: bool,
    fatal: bool,
    ctx: *mut c_void,
    on_bytes: FfiBytesFn,
) -> bool {
    unsafe {
        abort_on_panic(|| {
            if decoder.is_null() {
                eprintln!("textcodec_rust_streaming_decoder_decode_to_utf8: null decoder pointer");
                return false;
            }
            let Some(input) = bytes_from_raw(input, input_len) else {
                return false;
            };
            let decoder = &mut *decoder;
            let mut output = String::with_capacity(if fatal {
                let Some(output_capacity) = decoder.decoder.max_utf8_buffer_length_without_replacement(input.len())
                else {
                    return false;
                };
                output_capacity
            } else {
                let Some(output_capacity) = decoder.decoder.max_utf8_buffer_length(input.len()) else {
                    return false;
                };
                output_capacity
            });

            if fatal {
                let (result, _) = decoder
                    .decoder
                    .decode_to_string_without_replacement(input, &mut output, last);
                if !output.is_empty() {
                    on_bytes(ctx, output.as_ptr(), output.len());
                }
                return matches!(result, DecoderResult::InputEmpty);
            }

            let (result, _, _) = decoder.decoder.decode_to_string(input, &mut output, last);
            if !output.is_empty() {
                on_bytes(ctx, output.as_ptr(), output.len());
            }

            matches!(result, CoderResult::InputEmpty)
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
            let Some(output_capacity) = encoder.max_buffer_length_from_utf8_without_replacement(input.len()) else {
                return false;
            };
            let mut output = Vec::with_capacity(output_capacity);

            loop {
                let (result, read) =
                    encoder.encode_from_utf8_to_vec_without_replacement(&input[total_read..], &mut output, true);
                total_read += read;

                if !output.is_empty() {
                    on_bytes(ctx, output.as_ptr(), output.len());
                    output.clear();
                }

                match result {
                    EncoderResult::InputEmpty => return true,
                    EncoderResult::OutputFull => return false,
                    EncoderResult::Unmappable(unmappable) => {
                        on_error(ctx, unmappable as u32);
                    }
                }
            }
        })
    }
}
