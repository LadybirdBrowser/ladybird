/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::ffi::{bytes_from_raw, catch_panic_or};
use ::png::{BlendOp, ColorType, Decoder, DecodingError, DisposeOp, InterlaceInfo, Limits, Reader, Transformations};
use std::ffi::c_void;
use std::io::Cursor;

const PNG_SIGNATURE: [u8; 8] = [0x89, b'P', b'N', b'G', 0x0D, 0x0A, 0x1A, 0x0A];

#[repr(C)]
pub struct PNGImageInfo {
    pub width: u32,
    pub height: u32,
    pub loop_count: u32,
    pub has_cicp: bool,
    pub cicp_color_primaries: u8,
    pub cicp_transfer_function: u8,
    pub cicp_matrix_coefficients: u8,
    pub cicp_video_full_range_flag: u8,
    /// The ICC profile, valid only for the duration of the info callback.
    pub icc_profile: *const u8,
    pub icc_profile_len: usize,
    /// The eXIf chunk, valid only for the duration of the info callback.
    pub exif: *const u8,
    pub exif_len: usize,
}

#[repr(C)]
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum PNGDecodeResult {
    /// The header or the metadata before the image data could not be read.
    InvalidHeader,
    /// The header was read, but decoding the frames failed.
    DecodingFailed,
    Success,
}

type PNGImageInfoCallback = unsafe extern "C" fn(context: *mut c_void, info: *const PNGImageInfo) -> bool;
type PNGGetFrameBuffer =
    unsafe extern "C" fn(context: *mut c_void, duration_ms: u32, buffer: *mut *mut u8, stride: *mut usize) -> bool;
type PNGFrameDecoded = unsafe extern "C" fn(context: *mut c_void) -> bool;

struct Callbacks {
    context: *mut c_void,
    get_frame_buffer: PNGGetFrameBuffer,
    frame_decoded: PNGFrameDecoded,
}

impl Callbacks {
    /// Hands out a BGRA buffer of the image's size, as `height` rows of `stride` bytes.
    fn frame_buffer(&mut self, duration_ms: u32, width: usize, height: usize) -> Option<(&mut [u8], usize)> {
        let mut buffer = std::ptr::null_mut();
        let mut stride = 0;
        // SAFETY: The callback contract is documented on `png_rust_decode`.
        if !unsafe { (self.get_frame_buffer)(self.context, duration_ms, &raw mut buffer, &raw mut stride) }
            || buffer.is_null()
            || stride < width.checked_mul(4)?
        {
            return None;
        }
        let len = if height == 0 {
            0
        } else {
            stride.checked_mul(height - 1)?.checked_add(width * 4)?
        };
        // SAFETY: The callback guarantees a writable allocation with `height` rows of `stride` bytes, the last of
        // which holds at least `width * 4` bytes.
        let buffer = unsafe { ak::slice_from_raw_parts_mut(buffer, len) };
        Some((buffer, stride))
    }

    fn frame_decoded(&self) -> bool {
        // SAFETY: The callback contract is documented on `png_rust_decode`.
        unsafe { (self.frame_decoded)(self.context) }
    }
}

fn is_truncation(error: &DecodingError) -> bool {
    matches!(error, DecodingError::IoError(error) if error.kind() == std::io::ErrorKind::UnexpectedEof)
}

/// Converts one row of 8-bit RGBA or gray+alpha samples into unpremultiplied BGRA.
fn convert_row(color_type: ColorType, source: &[u8], destination: &mut [u8]) {
    match color_type {
        ColorType::Rgba => {
            for (source, destination) in source.as_chunks::<4>().0.iter().zip(destination.as_chunks_mut::<4>().0) {
                *destination = [source[2], source[1], source[0], source[3]];
            }
        }
        ColorType::GrayscaleAlpha => {
            for (source, destination) in source.as_chunks::<2>().0.iter().zip(destination.as_chunks_mut::<4>().0) {
                *destination = [source[0], source[0], source[0], source[1]];
            }
        }
        _ => unreachable!("the ALPHA transformation only produces RGBA and gray+alpha output"),
    }
}

/// How many rows the seven passes of an interlaced frame of `width` by `height` pixels have between them.
fn interlaced_row_count(width: usize, height: usize) -> usize {
    // The first column and the first row of each pass, and the step from one of its rows to the next.
    const PASSES: [(usize, usize, usize); 7] = [
        (0, 0, 8),
        (4, 0, 8),
        (0, 4, 8),
        (2, 0, 4),
        (0, 2, 4),
        (1, 0, 2),
        (0, 1, 2),
    ];
    PASSES
        .iter()
        .filter(|(first_column, ..)| width > *first_column)
        .map(|(_, first_row, row_step)| height.saturating_sub(*first_row).div_ceil(*row_step))
        .sum()
}

/// Decodes the next frame, of `width` by `height` pixels, into `destination` as BGRA rows of `stride` bytes. The pixels
/// decoded before an error are kept, and the others are left as they were.
fn decode_frame(
    reader: &mut Reader<Cursor<&[u8]>>,
    (width, height): (usize, usize),
    row_buffer: &mut Vec<u8>,
    destination: &mut [u8],
    stride: usize,
) -> Result<(), DecodingError> {
    let (color_type, _) = reader.output_color_type();
    let samples = color_type.samples();

    if reader.info().interlaced {
        // Each row of an interlaced pass holds some of the pixels of one row of the frame. It is turned into BGRA, and
        // then spread out over those pixels.
        for _ in 0..interlaced_row_count(width, height) {
            let Some(row) = reader.next_interlaced_row()? else {
                break;
            };
            let InterlaceInfo::Adam7(pass) = *row.interlace() else {
                unreachable!("an interlaced frame only has Adam7 rows");
            };
            row_buffer.resize(row.data().len() / samples * 4, 0);
            convert_row(color_type, row.data(), row_buffer);
            ::png::expand_interlaced_row(destination, stride, row_buffer, &pass, 32);
        }
    } else {
        for destination in destination.chunks_mut(stride).take(height) {
            let Some(row) = reader.next_row()? else {
                break;
            };
            convert_row(
                color_type,
                &row.data()[..width * samples],
                &mut destination[..width * 4],
            );
        }
    }

    // Reading on past the last row checks the rest of the frame's data. A file that ends there still has the whole
    // frame.
    match reader.next_row() {
        Err(error) if is_truncation(&error) => Ok(()),
        result => result.map(|_| ()),
    }
}

fn decode_still_image(reader: &mut Reader<Cursor<&[u8]>>, callbacks: &mut Callbacks) -> PNGDecodeResult {
    let (width, height) = reader.info().size();
    let Some((buffer, stride)) = callbacks.frame_buffer(0, width as usize, height as usize) else {
        return PNGDecodeResult::DecodingFailed;
    };

    // A truncated image keeps the rows that arrived, and the rest stays transparent.
    buffer.fill(0);
    let mut row_buffer = Vec::new();
    match decode_frame(
        reader,
        (width as usize, height as usize),
        &mut row_buffer,
        buffer,
        stride,
    ) {
        Err(error) if !is_truncation(&error) => return PNGDecodeResult::DecodingFailed,
        _ => {}
    }

    if !callbacks.frame_decoded() {
        return PNGDecodeResult::DecodingFailed;
    }
    PNGDecodeResult::Success
}

struct Rect {
    x: usize,
    y: usize,
    width: usize,
    height: usize,
}

/// An unpremultiplied BGRA image, whose rows are packed.
struct Canvas {
    pixels: Vec<u8>,
    width: usize,
    height: usize,
}

impl Canvas {
    fn new(width: usize, height: usize) -> Option<Self> {
        Some(Self {
            pixels: vec![0; width.checked_mul(height)?.checked_mul(4)?],
            width,
            height,
        })
    }

    fn row(&self, y: usize) -> &[u8] {
        &self.pixels[y * self.width * 4..(y + 1) * self.width * 4]
    }

    /// Calls `callback` with each pixel of `source_rect` in `source` and the canvas pixel it lands on at `location`.
    fn for_each_frame_pixel(
        &mut self,
        location: (usize, usize),
        source: &Canvas,
        source_rect: &Rect,
        mut callback: impl FnMut(&mut [u8; 4], &[u8; 4]),
    ) {
        let width = source_rect.width.min(self.width.saturating_sub(location.0));
        let height = source_rect.height.min(self.height.saturating_sub(location.1));
        for y in 0..height {
            let destination_start = ((location.1 + y) * self.width + location.0) * 4;
            let source_start = ((source_rect.y + y) * source.width + source_rect.x) * 4;
            let destination = &mut self.pixels[destination_start..destination_start + width * 4];
            let source = &source.pixels[source_start..source_start + width * 4];
            for (destination, source) in destination
                .as_chunks_mut::<4>()
                .0
                .iter_mut()
                .zip(source.as_chunks::<4>().0)
            {
                callback(destination, source);
            }
        }
    }

    /// The source pixels replace the canvas pixels. A pixel with no alpha also loses its color.
    fn copy_pixels(&mut self, location: (usize, usize), source: &Canvas, source_rect: &Rect) {
        self.for_each_frame_pixel(location, source, source_rect, |destination, source| {
            *destination = *source;
            if source[3] == 0 {
                destination[..3].fill(0);
            }
        });
    }

    /// The source pixels go over the canvas pixels with a simple OVER operation. This takes the same steps as Skia takes
    /// when it draws an unpremultiplied bitmap onto an unpremultiplied bitmap: premultiply both, blend, and
    /// unpremultiply.
    fn blend_pixels(&mut self, location: (usize, usize), source: &Canvas) {
        let source_rect = Rect {
            x: 0,
            y: 0,
            width: source.width,
            height: source.height,
        };
        self.for_each_frame_pixel(location, source, &source_rect, |destination, source| {
            let source_alpha = source[3];
            if source_alpha == 255 {
                *destination = *source;
                return;
            }
            if source_alpha == 0 {
                if destination[3] == 0 {
                    destination[..3].fill(0);
                }
                return;
            }
            const K: f32 = 1.0 / 255.0;
            let sa = source_alpha as f32 * K;
            let da = destination[3] as f32 * K;
            let result_alpha = da * (1.0 - sa) + sa;
            let scale = 1.0 / result_alpha;
            for channel in 0..3 {
                let sc = (source[channel] as f32 * K) * sa;
                let dc = (destination[channel] as f32 * K) * da;
                let result = (dc * (1.0 - sa) + sc) * scale;
                destination[channel] = (result * 255.0).clamp(0.0, 255.0).round_ties_even() as u8;
            }
            destination[3] = (result_alpha * 255.0).clamp(0.0, 255.0).round_ties_even() as u8;
        });
    }

    fn clear(&mut self, rect: &Rect) {
        let x_end = (rect.x + rect.width).min(self.width);
        let y_end = (rect.y + rect.height).min(self.height);
        if rect.x >= x_end {
            return;
        }
        for y in rect.y..y_end {
            self.pixels[(y * self.width + rect.x) * 4..(y * self.width + x_end) * 4].fill(0);
        }
    }
}

fn duration_in_milliseconds(delay_num: u16, delay_den: u16) -> u32 {
    if delay_num == 0 {
        return 1;
    }
    let denominator = if delay_den != 0 { delay_den as u32 } else { 100 };
    delay_num as u32 * 1000 / denominator
}

/// Decodes the frames of an animation, and returns how many were handed out. Decoding stops at the first error, and the
/// frames before it are kept.
fn decode_animation(reader: &mut Reader<Cursor<&[u8]>>, frame_count: u32, callbacks: &mut Callbacks) -> u32 {
    let mut decoded_frame_count = 0;
    let (width, height) = reader.info().size();
    let Some(mut canvas) = Canvas::new(width as usize, height as usize) else {
        return decoded_frame_count;
    };

    // A default image without an fcTL chunk is not part of the animation.
    if reader.info().frame_control.is_none() && reader.next_frame_info().is_err() {
        return decoded_frame_count;
    }

    let mut frame = Canvas {
        pixels: Vec::new(),
        width: 0,
        height: 0,
    };
    let mut previous_canvas = Canvas {
        pixels: Vec::new(),
        width: 0,
        height: 0,
    };
    let mut row_buffer = Vec::new();

    for frame_index in 0..frame_count {
        // The reader starts out at the first frame's image data, and has to be moved on to each later one.
        if frame_index != 0 && reader.next_frame_info().is_err() {
            return decoded_frame_count;
        }
        let Some(frame_control) = reader.info().frame_control else {
            return decoded_frame_count;
        };
        if frame_control.width == 0 || frame_control.height == 0 {
            return decoded_frame_count;
        }
        let frame_rect = Rect {
            x: frame_control.x_offset as usize,
            y: frame_control.y_offset as usize,
            width: frame_control.width as usize,
            height: frame_control.height as usize,
        };

        frame.width = frame_rect.width;
        frame.height = frame_rect.height;
        frame.pixels.clear();
        frame.pixels.resize(frame_rect.width * frame_rect.height * 4, 0);
        let frame_size = (frame_rect.width, frame_rect.height);
        if decode_frame(
            reader,
            frame_size,
            &mut row_buffer,
            &mut frame.pixels,
            frame_rect.width * 4,
        )
        .is_err()
        {
            return decoded_frame_count;
        }

        // Only keep a copy of the canvas when the frame asks for it to be restored.
        if frame_control.dispose_op == DisposeOp::Previous {
            previous_canvas.width = canvas.width;
            previous_canvas.height = canvas.height;
            previous_canvas.pixels.clone_from(&canvas.pixels);
        }

        let location = (frame_rect.x, frame_rect.y);
        match frame_control.blend_op {
            // All color components of the frame, including alpha, overwrite the current contents of the frame's output
            // buffer region.
            BlendOp::Source => {
                let source_rect = Rect {
                    x: 0,
                    y: 0,
                    width: frame.width,
                    height: frame.height,
                };
                canvas.copy_pixels(location, &frame, &source_rect);
            }
            // The frame should be composited onto the output buffer based on its alpha, using a simple OVER operation.
            BlendOp::Over => canvas.blend_pixels(location, &frame),
        }

        let duration_ms = duration_in_milliseconds(frame_control.delay_num, frame_control.delay_den);
        let Some((buffer, stride)) = callbacks.frame_buffer(duration_ms, canvas.width, canvas.height) else {
            return decoded_frame_count;
        };
        for y in 0..canvas.height {
            buffer[y * stride..y * stride + canvas.width * 4].copy_from_slice(canvas.row(y));
        }
        if !callbacks.frame_decoded() {
            return decoded_frame_count;
        }
        decoded_frame_count += 1;

        match frame_control.dispose_op {
            // No disposal is done on this frame before rendering the next; the contents of the output buffer are left
            // as is.
            DisposeOp::None => {}
            // The frame's region of the output buffer is to be cleared to fully transparent black before rendering the
            // next frame.
            DisposeOp::Background => canvas.clear(&frame_rect),
            // The frame's region of the output buffer is to be reverted to the previous contents before rendering the
            // next frame.
            DisposeOp::Previous => canvas.copy_pixels(location, &previous_canvas, &frame_rect),
        }
    }

    decoded_frame_count
}

fn read_info(data: &[u8]) -> Result<Reader<Cursor<&[u8]>>, DecodingError> {
    read_until_image_data(Decoder::new(Cursor::new(data)))
}

/// Reads up to the image data again, for decoding the frames of an animation.
fn read_animation_info(data: &[u8]) -> Result<Reader<Cursor<&[u8]>>, DecodingError> {
    // The decoder counts a row of output against its memory limit for every frame it starts, and never gives it back,
    // so the default limit cuts a long animation short. The limit also bounds the inflated ICC profile and text, which
    // this reader skips, so all it allocates is bounded by the input, and it can go without one.
    let mut decoder = Decoder::new_with_limits(Cursor::new(data), Limits { bytes: usize::MAX });
    decoder.set_ignore_iccp_chunk(true);
    decoder.set_ignore_text_chunk(true);
    read_until_image_data(decoder)
}

fn read_until_image_data(mut decoder: Decoder<Cursor<&[u8]>>) -> Result<Reader<Cursor<&[u8]>>, DecodingError> {
    // Palette and low bit depth images are expanded, 16-bit samples are cut to 8 bits, and every image gets an alpha
    // channel. The output is then always 8-bit RGBA or gray+alpha.
    decoder.set_transformations(Transformations::EXPAND | Transformations::STRIP_16 | Transformations::ALPHA);
    decoder.read_info()
}

/// An ICC profile only applies to an image whose kind of color it describes.
fn icc_profile_matches_image(profile: &[u8], color_type: ColorType) -> bool {
    let Some(color_space) = profile.get(16..20) else {
        return false;
    };
    match color_type {
        ColorType::Grayscale | ColorType::GrayscaleAlpha => color_space == b"GRAY",
        ColorType::Rgb | ColorType::Rgba | ColorType::Indexed => color_space == b"RGB ",
    }
}

/// # Safety
/// `data` must be valid for `data_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn png_rust_sniff(data: *const u8, data_len: usize) -> bool {
    catch_panic_or(false, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return false;
        };
        data.starts_with(&PNG_SIGNATURE)
    })
}

/// Decodes a PNG or APNG image. Every frame is handed out as unpremultiplied BGRA pixels at the full image size, with
/// the animation already composed.
///
/// # Safety
/// - `data` must be valid for `data_len` bytes.
/// - `image_info` is called once, before any frame. The pointers in the info are only valid during the call.
/// - `get_frame_buffer` must initialize `buffer` and `stride` with writable storage for a BGRA8 image of the size
///   passed to `image_info`. The storage must remain valid until `frame_decoded` is called or this function returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn png_rust_decode(
    data: *const u8,
    data_len: usize,
    context: *mut c_void,
    image_info: PNGImageInfoCallback,
    get_frame_buffer: PNGGetFrameBuffer,
    frame_decoded: PNGFrameDecoded,
) -> PNGDecodeResult {
    catch_panic_or(PNGDecodeResult::DecodingFailed, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return PNGDecodeResult::InvalidHeader;
        };

        let Ok(mut reader) = read_info(data) else {
            return PNGDecodeResult::InvalidHeader;
        };

        let info = reader.info();
        let animation_frame_count = info.animation_control.map_or(0, |animation| animation.num_frames);
        let cicp = info.coding_independent_code_points;
        let (icc_profile, icc_profile_len) = info
            .icc_profile
            .as_deref()
            .filter(|profile| icc_profile_matches_image(profile, info.color_type))
            .map_or((std::ptr::null(), 0), |profile| (profile.as_ptr(), profile.len()));
        let (exif, exif_len) = info
            .exif_metadata
            .as_deref()
            .map_or((std::ptr::null(), 0), |exif| (exif.as_ptr(), exif.len()));
        let image_info_value = PNGImageInfo {
            width: info.width,
            height: info.height,
            loop_count: info.animation_control.map_or(0, |animation| animation.num_plays),
            has_cicp: cicp.is_some(),
            cicp_color_primaries: cicp.map_or(0, |cicp| cicp.color_primaries),
            cicp_transfer_function: cicp.map_or(0, |cicp| cicp.transfer_function),
            cicp_matrix_coefficients: cicp.map_or(0, |cicp| cicp.matrix_coefficients),
            cicp_video_full_range_flag: cicp.map_or(0, |cicp| cicp.is_video_full_range_image as u8),
            icc_profile,
            icc_profile_len,
            exif,
            exif_len,
        };
        // SAFETY: The callback contract is documented on this function.
        if !unsafe { image_info(context, &raw const image_info_value) } {
            return PNGDecodeResult::InvalidHeader;
        }

        let mut callbacks = Callbacks {
            context,
            get_frame_buffer,
            frame_decoded,
        };
        if animation_frame_count == 0 {
            return decode_still_image(&mut reader, &mut callbacks);
        }
        if let Ok(mut animation_reader) = read_animation_info(data)
            && decode_animation(&mut animation_reader, animation_frame_count, &mut callbacks) > 0
        {
            return PNGDecodeResult::Success;
        }

        // An animation that is broken before its first frame shows the default image instead.
        decode_still_image(&mut reader, &mut callbacks)
    })
}
