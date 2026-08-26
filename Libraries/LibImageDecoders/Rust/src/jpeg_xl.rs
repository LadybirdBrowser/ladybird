/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use jxl::api::states::{Initialized, WithImageInfo};
use jxl::api::{JxlDecoder, JxlDecoderOptions, JxlOutputBuffer, JxlPixelFormat, ProcessingResult, check_signature};
use jxl::headers::extra_channels::ExtraChannel;
use std::ffi::c_void;
use std::panic::AssertUnwindSafe;
use std::panic::catch_unwind;

#[repr(C)]
pub struct JPEGXLImageInfo {
    pub width: usize,
    pub height: usize,
    pub is_animated: bool,
    pub alpha_premultiplied: bool,
    pub loop_count: u32,
}

type JPEGXLGetFrameBuffer =
    unsafe extern "C" fn(context: *mut c_void, duration_ms: u32, buffer: *mut *mut u8, stride: *mut usize) -> bool;
type JPEGXLFrameDecoded = unsafe extern "C" fn(context: *mut c_void) -> bool;

pub struct JPEGXLDecoder {
    decoder: Option<JxlDecoder<WithImageInfo>>,
    input_offset: usize,
}

fn catch_panic_or<F: FnOnce() -> R, R>(fallback: R, f: F) -> R {
    match catch_unwind(AssertUnwindSafe(f)) {
        Ok(result) => result,
        Err(payload) => {
            if let Some(message) = payload.downcast_ref::<&str>() {
                eprintln!("Rust panic at JPEG XL FFI boundary: {message}");
            } else if let Some(message) = payload.downcast_ref::<String>() {
                eprintln!("Rust panic at JPEG XL FFI boundary: {message}");
            } else {
                eprintln!("Rust panic at JPEG XL FFI boundary");
            }
            fallback
        }
    }
}

unsafe fn bytes_from_raw<'a>(bytes: *const u8, len: usize) -> Option<&'a [u8]> {
    if len == 0 {
        return Some(&[]);
    }
    if bytes.is_null() {
        eprintln!("bytes_from_raw: null pointer with non-zero length {len}");
        return None;
    }

    // SAFETY: The caller guarantees that `bytes` is valid for `len` bytes.
    Some(unsafe { std::slice::from_raw_parts(bytes, len) })
}

fn decode_header(input: &mut &[u8], options: JxlDecoderOptions) -> Option<JxlDecoder<WithImageInfo>> {
    let decoder = JxlDecoder::<Initialized>::new(options);
    match decoder.process(input, None).ok()? {
        ProcessingResult::Complete { result } => Some(result),
        ProcessingResult::NeedsMoreInput { .. } => None,
    }
}

fn duration_in_milliseconds(duration: Option<f64>) -> u32 {
    duration.unwrap_or(0.0).round().clamp(0.0, u32::MAX as f64) as u32
}

/// # Safety
/// `data` must be valid for `data_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jpegxl_rust_sniff(data: *const u8, data_len: usize) -> bool {
    catch_panic_or(false, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return false;
        };
        matches!(check_signature(data), ProcessingResult::Complete { result: Some(_) })
    })
}

/// # Safety
/// - `data` must be valid for `data_len` bytes.
/// - `out_info` must be a valid writable pointer.
/// - The returned pointer must be freed with `jpegxl_rust_decoder_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jpegxl_rust_decoder_new(
    data: *const u8,
    data_len: usize,
    out_info: *mut JPEGXLImageInfo,
) -> *mut JPEGXLDecoder {
    catch_panic_or(std::ptr::null_mut(), || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return std::ptr::null_mut();
        };
        let Some(out_info) = (unsafe { out_info.as_mut() }) else {
            return std::ptr::null_mut();
        };

        let mut input = data;
        let mut options = JxlDecoderOptions::default();
        // All decoded frames are full-canvas, coalesced frames.
        options.coalescing = true;
        let Some(mut decoder) = decode_header(&mut input, options) else {
            return std::ptr::null_mut();
        };
        let basic_info = decoder.basic_info();
        let alpha_premultiplied = basic_info
            .extra_channels
            .iter()
            .find(|channel| channel.ec_type == ExtraChannel::Alpha)
            .is_some_and(|channel| channel.alpha_associated);

        *out_info = JPEGXLImageInfo {
            width: basic_info.size.0,
            height: basic_info.size.1,
            is_animated: basic_info.animation.is_some(),
            alpha_premultiplied,
            loop_count: basic_info.animation.as_ref().map_or(0, |animation| animation.num_loops),
        };
        decoder.set_pixel_format(JxlPixelFormat::rgba8(basic_info.extra_channels.len()));

        Box::into_raw(Box::new(JPEGXLDecoder {
            decoder: Some(decoder),
            input_offset: data.len() - input.len(),
        }))
    })
}

/// # Safety
/// `decoder` must be null or a pointer returned by `jpegxl_rust_decoder_new` that has not already
/// been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jpegxl_rust_decoder_free(decoder: *mut JPEGXLDecoder) {
    catch_panic_or((), || {
        if !decoder.is_null() {
            // SAFETY: The caller transfers ownership of a decoder allocated by
            // `jpegxl_rust_decoder_new`.
            drop(unsafe { Box::from_raw(decoder) });
        }
    });
}

/// # Safety
/// - `decoder` must be a valid pointer returned by `jpegxl_rust_decoder_new`.
/// - `data` must be the same image passed to `jpegxl_rust_decoder_new` and be valid for
///   `data_len` bytes.
/// - `get_frame_buffer` must initialize `buffer` and `stride` with writable storage for an RGBA8
///   image whose dimensions were returned by `jpegxl_rust_decoder_new`. The storage must remain
///   valid until `frame_decoded` is called or this function returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jpegxl_rust_decode(
    decoder: *mut JPEGXLDecoder,
    data: *const u8,
    data_len: usize,
    context: *mut c_void,
    get_frame_buffer: JPEGXLGetFrameBuffer,
    frame_decoded: JPEGXLFrameDecoded,
) -> bool {
    catch_panic_or(false, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return false;
        };
        let Some(decoder) = (unsafe { decoder.as_mut() }) else {
            return false;
        };
        let Some(mut input) = data.get(decoder.input_offset..) else {
            return false;
        };
        // The decoder is single-use. Taking it before processing also ensures that a panic cannot
        // leave partially mutated decoder state available for another call.
        let Some(mut decoder_state) = decoder.decoder.take() else {
            return false;
        };

        let (width, height) = decoder_state.basic_info().size;
        let Some(row_bytes) = width.checked_mul(4) else {
            return false;
        };

        loop {
            let Ok(ProcessingResult::Complete {
                result: decoder_with_frame_info,
            }) = decoder_state.process(&mut input, None)
            else {
                return false;
            };

            let duration_ms = duration_in_milliseconds(decoder_with_frame_info.frame_header().duration);
            let mut buffer = std::ptr::null_mut();
            let mut stride = 0;
            // SAFETY: The callback contract is documented on this function.
            if !unsafe { get_frame_buffer(context, duration_ms, &raw mut buffer, &raw mut stride) }
                || buffer.is_null()
                || stride < row_bytes
            {
                return false;
            }

            // SAFETY: The callback guarantees a writable, initialized allocation with `height`
            // rows, at least `row_bytes` bytes per row, and `stride` bytes between rows.
            let output_buffer = unsafe { JxlOutputBuffer::new_from_ptr(buffer, height, row_bytes, stride) };
            let Ok(ProcessingResult::Complete { result }) =
                decoder_with_frame_info.process(&mut input, &mut [output_buffer], None)
            else {
                return false;
            };
            decoder_state = result;

            // Commit the frame only after jxl-rs has successfully populated its output buffer.
            // SAFETY: The callback contract is documented on this function.
            if !unsafe { frame_decoded(context) } {
                return false;
            }

            if !decoder_state.has_more_frames() {
                return true;
            }
        }
    })
}

#[cfg(test)]
mod tests {
    use super::catch_panic_or;

    #[test]
    fn panic_at_ffi_boundary_is_contained() {
        assert!(!catch_panic_or(false, || panic!("expected test panic")));
    }
}
