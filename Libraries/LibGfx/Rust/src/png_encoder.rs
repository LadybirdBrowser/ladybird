/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use png::{BitDepth, ColorType, Compression, DeflateCompression, Encoder, Filter, Info, chunk};
use std::borrow::Cow;
use std::ffi::c_void;
use std::io::{self, Write};

#[repr(C)]
pub struct PNGEncodeOptions {
    /// The data for an iCCP chunk, or null.
    pub icc_profile: *const u8,
    pub icc_profile_len: usize,
    pub has_cicp: bool,
    pub cicp_color_primaries: u8,
    pub cicp_transfer_function: u8,
    pub cicp_matrix_coefficients: u8,
    pub cicp_video_full_range_flag: u8,
    /// Encodes much faster, at the cost of a larger file.
    pub fast: bool,
    /// The color values of the pixels are premultiplied by their alpha, which PNG does not store them as.
    pub premultiplied: bool,
}

const STREAM_BUFFER_SIZE: usize = 256 * 1024;

type PNGWriteCallback = unsafe extern "C" fn(context: *mut c_void, data: *const u8, len: usize) -> bool;

struct CallbackWriter {
    context: *mut c_void,
    write: PNGWriteCallback,
}

impl Write for CallbackWriter {
    fn write(&mut self, data: &[u8]) -> io::Result<usize> {
        // SAFETY: The callback contract is documented on `png_rust_encode`.
        if !unsafe { (self.write)(self.context, data.as_ptr(), data.len()) } {
            return Err(io::ErrorKind::OutOfMemory.into());
        }
        Ok(data.len())
    }

    fn flush(&mut self) -> io::Result<()> {
        Ok(())
    }
}

/// Whether every pixel of a row of RGBA or BGRA pixels is opaque.
fn is_opaque(row: &[u8]) -> bool {
    // NB: A fold over every pixel, rather than a search that stops at the first translucent one, vectorizes.
    row.as_chunks::<4>()
        .0
        .iter()
        .fold(u8::MAX, |alpha, pixel| alpha & pixel[3])
        == u8::MAX
}

/// Divides the color values of an RGBA pixel by its alpha, rounding to the nearest value.
fn unpremultiplied(pixel: [u8; 4]) -> [u8; 4] {
    let [red, green, blue, alpha] = pixel;
    match alpha {
        u8::MAX => pixel,
        0 => [0, 0, 0, 0],
        _ => {
            let alpha_value = u32::from(alpha);
            let channel =
                |value: u8| u8::try_from((u32::from(value) * 255 + alpha_value / 2) / alpha_value).unwrap_or(u8::MAX);
            [channel(red), channel(green), channel(blue), alpha]
        }
    }
}

/// Encodes 8-bit RGBA pixels, or BGRA pixels when `is_bgra` is set, as an RGBA PNG.
///
/// # Safety
/// - `pixels` must be valid for `height` rows of `stride` bytes, the last of which holds at least `width * 4` bytes.
/// - `options` must be a valid pointer, and its ICC profile must be null or valid for `icc_profile_len` bytes.
/// - `write` is called with the encoded bytes in order, and returns false when it could not take them.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn png_rust_encode(
    pixels: *const u8,
    width: u32,
    height: u32,
    stride: usize,
    is_bgra: bool,
    options: *const PNGEncodeOptions,
    context: *mut c_void,
    write: PNGWriteCallback,
) -> bool {
    // SAFETY: The caller guarantees that `options` is valid.
    let options = unsafe { &*options };
    let row_bytes = width as usize * 4;
    if width == 0 || height == 0 || stride < row_bytes {
        return false;
    }
    let len = stride * (height as usize - 1) + row_bytes;
    // SAFETY: The caller guarantees that `pixels` is valid for `len` bytes.
    let pixels = unsafe { ak::slice_from_raw_parts(pixels, len) };

    let mut info = Info::with_size(width, height);
    info.color_type = ColorType::Rgba;
    info.bit_depth = BitDepth::Eight;
    if !options.icc_profile.is_null() {
        // SAFETY: The caller guarantees that the ICC profile is valid.
        let profile = unsafe { ak::slice_from_raw_parts(options.icc_profile, options.icc_profile_len) };
        info.icc_profile = Some(Cow::Borrowed(profile));
    }
    let Ok(mut encoder) = Encoder::with_info(CallbackWriter { context, write }, info) else {
        return false;
    };
    if options.fast {
        // NB: Trying every filter on each row and compressing with zlib dominate the encoding time.
        encoder.set_deflate_compression(DeflateCompression::FdeflateUltraFast);
        encoder.set_filter(Filter::Sub);
    } else {
        encoder.set_compression(Compression::Balanced);
    }

    let Ok(mut writer) = encoder.write_header() else {
        return false;
    };
    if options.has_cicp {
        let cicp = [
            options.cicp_color_primaries,
            options.cicp_transfer_function,
            options.cicp_matrix_coefficients,
            options.cicp_video_full_range_flag,
        ];
        if writer.write_chunk(chunk::cICP, &cicp).is_err() {
            return false;
        }
    }

    // The rows go to the encoder one at a time, so the image is never copied whole.
    let Ok(mut stream) = writer.stream_writer_with_size(STREAM_BUFFER_SIZE) else {
        return false;
    };
    let mut row = vec![0; row_bytes];
    for y in 0..height as usize {
        let source = &pixels[y * stride..y * stride + row_bytes];
        // NB: Most rows of most images are opaque, and unpremultiplying leaves an opaque pixel as it is, so such a row
        //     takes the same path as an unpremultiplied one.
        let row = if options.premultiplied && !is_opaque(source) {
            for (source, destination) in source.as_chunks::<4>().0.iter().zip(row.as_chunks_mut::<4>().0) {
                let pixel = if is_bgra {
                    [source[2], source[1], source[0], source[3]]
                } else {
                    *source
                };
                *destination = unpremultiplied(pixel);
            }
            &row[..]
        } else if is_bgra {
            for (source, destination) in source.as_chunks::<4>().0.iter().zip(row.as_chunks_mut::<4>().0) {
                *destination = [source[2], source[1], source[0], source[3]];
            }
            &row[..]
        } else {
            source
        };
        if stream.write_all(row).is_err() {
            return false;
        }
    }
    if stream.finish().is_err() {
        return false;
    }
    writer.finish().is_ok()
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn unpremultiplied_leaves_opaque_pixels_and_clears_transparent_ones() {
        assert_eq!(unpremultiplied([12, 34, 56, u8::MAX]), [12, 34, 56, u8::MAX]);
        assert_eq!(unpremultiplied([12, 34, 56, 0]), [0, 0, 0, 0]);
        assert_eq!(unpremultiplied([100, 50, 25, 128]), [199, 100, 50, 128]);
    }

    #[test]
    fn is_opaque_looks_at_every_pixel() {
        assert!(is_opaque(&[1, 2, 3, 255, 4, 5, 6, 255]));
        assert!(!is_opaque(&[1, 2, 3, 255, 4, 5, 6, 254]));
        assert!(!is_opaque(&[1, 2, 3, 0, 4, 5, 6, 255]));
    }
}
