/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! C ABI bindings around the `jxl` (jxl-rs) crate, used by LibGfx's
//! JPEGXLLoader to decode JPEG XL images without depending on the C++ libjxl
//! library.
//!
//! The API is stateful: the C++ caller creates an opaque `JxlRsDecoder` from a
//! contiguous byte buffer with `jxl_rs_decoder_create`. The decoder caches
//! basic info, the embedded ICC profile and per-frame durations during
//! construction (using a fast scan-only pass), so cheap metadata queries don't
//! require a full pixel decode. Pixel decoding for individual frames happens
//! lazily through `jxl_rs_decoder_decode_frame`.
//!
//! Frames are decoded sequentially; if the caller asks for an earlier frame
//! again, the streaming decoder is rewound transparently.

use jxl::api::{
    JxlBasicInfo, JxlColorEncoding, JxlColorProfile, JxlColorType, JxlDataFormat, JxlDecoderInner, JxlDecoderOptions,
    JxlPixelFormat, JxlSignatureType, ProcessingResult, VisibleFrameSeekTarget, check_signature,
};
use jxl::headers::extra_channels::ExtraChannel;
use jxl::image::JxlOutputBuffer;

/// Status codes returned by FFI entry points.
#[repr(u8)]
pub enum JxlRsStatus {
    Success = 0,
    Error = 1,
    NeedMoreInput = 2,
}

/// Basic image information mirrored from `JxlBasicInfo`.
#[repr(C)]
pub struct JxlRsBasicInfo {
    pub width: u32,
    pub height: u32,
    pub bits_per_sample: u32,
    pub num_extra_channels: u32,
    pub has_alpha: bool,
    pub alpha_premultiplied: bool,
    pub have_animation: bool,
    pub animation_loop_count: u32,
    pub animation_tps_numerator: u32,
    pub animation_tps_denominator: u32,
    pub uses_original_profile: bool,
    pub orientation: u32,
    pub is_grayscale: bool,
}

impl Default for JxlRsBasicInfo {
    fn default() -> Self {
        Self {
            width: 0,
            height: 0,
            bits_per_sample: 0,
            num_extra_channels: 0,
            has_alpha: false,
            alpha_premultiplied: false,
            have_animation: false,
            animation_loop_count: 0,
            animation_tps_numerator: 1,
            animation_tps_denominator: 1000,
            uses_original_profile: false,
            orientation: 1,
            is_grayscale: false,
        }
    }
}

fn fill_basic_info(info: &JxlBasicInfo, profile: Option<&JxlColorProfile>) -> JxlRsBasicInfo {
    let alpha_channel = info
        .extra_channels
        .iter()
        .find(|ec| matches!(ec.ec_type, ExtraChannel::Alpha));
    let has_alpha = alpha_channel.is_some();
    let alpha_premultiplied = alpha_channel.map(|ec| ec.alpha_associated).unwrap_or(false);

    let (loop_count, tps_num, tps_den) = info
        .animation
        .as_ref()
        .map(|a| (a.num_loops, a.tps_numerator, a.tps_denominator))
        .unwrap_or((0, 1, 1000));

    let is_grayscale = matches!(
        profile,
        Some(JxlColorProfile::Simple(JxlColorEncoding::GrayscaleColorSpace { .. }))
    );

    JxlRsBasicInfo {
        width: info.size.0 as u32,
        height: info.size.1 as u32,
        bits_per_sample: info.bit_depth.bits_per_sample(),
        num_extra_channels: info.extra_channels.len() as u32,
        has_alpha,
        alpha_premultiplied,
        have_animation: info.animation.is_some(),
        animation_loop_count: loop_count,
        animation_tps_numerator: tps_num,
        animation_tps_denominator: tps_den,
        uses_original_profile: info.uses_original_profile,
        orientation: info.orientation as u32,
        is_grayscale,
    }
}

/// Cached per-frame metadata captured during the scan-only pass. Holds
/// everything the pixel decoder needs to seek directly to the frame.
struct ScannedFrame {
    duration_ms: f64,
    seek_target: VisibleFrameSeekTarget,
}

/// Opaque stateful decoder handle.
pub struct JxlRsDecoder {
    data: Vec<u8>,
    info: JxlRsBasicInfo,
    icc_profile: Vec<u8>,
    /// One entry per visible frame in the codestream (always >= 1).
    scanned: Vec<ScannedFrame>,
    /// Number of extra channels reported by the codestream; used when
    /// configuring the per-frame pixel format.
    extra_channels: usize,

    /// Pixel-decoding decoder, lazily initialized on first decode request and
    /// reused across frames via `start_new_frame` seeks.
    pixel: Option<PixelDecoder>,
}

struct PixelDecoder {
    decoder: JxlDecoderInner,
    /// `premultiply_output` value the decoder was constructed with. If a
    /// later decode request asks for a different value, we throw the decoder
    /// away and rebuild it.
    premultiply: bool,
}

fn drive_until_basic_info(decoder: &mut JxlDecoderInner, input: &mut &[u8]) -> Result<(), JxlRsStatus> {
    loop {
        match decoder.process(input, None) {
            Ok(ProcessingResult::Complete { .. }) => {
                if decoder.basic_info().is_some() {
                    return Ok(());
                }
                if input.is_empty() {
                    return Err(JxlRsStatus::NeedMoreInput);
                }
            }
            Ok(ProcessingResult::NeedsMoreInput { .. }) => {
                if input.is_empty() {
                    return Err(JxlRsStatus::NeedMoreInput);
                }
            }
            Err(_) => return Err(JxlRsStatus::Error),
        }
    }
}

impl JxlRsDecoder {
    fn build_pixel_format(&self) -> JxlPixelFormat {
        JxlPixelFormat {
            color_type: JxlColorType::Rgba,
            color_data_format: Some(JxlDataFormat::U8 { bit_depth: 8 }),
            extra_channel_format: vec![None; self.extra_channels],
        }
    }

    /// Build a fresh pixel decoder advanced past the file-level header so it
    /// can accept `start_new_frame` seeks.
    fn build_pixel_decoder(&self, premultiply: bool) -> Result<PixelDecoder, JxlRsStatus> {
        let mut options = JxlDecoderOptions::default();
        options.premultiply_output = premultiply;
        let mut decoder = JxlDecoderInner::new(options);
        let mut input: &[u8] = &self.data;
        drive_until_basic_info(&mut decoder, &mut input)?;
        decoder.set_pixel_format(self.build_pixel_format());
        Ok(PixelDecoder { decoder, premultiply })
    }

    fn ensure_pixel_decoder(&mut self, premultiply: bool) -> Result<(), JxlRsStatus> {
        let need_rebuild = match &self.pixel {
            None => true,
            Some(p) => p.premultiply != premultiply,
        };
        if need_rebuild {
            self.pixel = Some(self.build_pixel_decoder(premultiply)?);
        }
        Ok(())
    }
}

/// Check whether `data` starts with a valid JPEG XL signature.
///
/// # Safety
/// `data` must be either null (with `len == 0`) or point to at least `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_signature_check(data: *const u8, len: usize) -> bool {
    if data.is_null() || len < 2 {
        return false;
    }
    let bytes = unsafe { core::slice::from_raw_parts(data, len) };
    let probe = &bytes[..bytes.len().min(12)];
    matches!(
        check_signature(probe),
        ProcessingResult::Complete {
            result: Some(JxlSignatureType::Codestream | JxlSignatureType::Container)
        }
    )
}

/// Create a stateful decoder that owns a copy of `data`. Returns null on error.
///
/// On success, the returned handle has cached basic info and per-frame
/// durations (via a scan-only pass), so subsequent metadata queries are O(1).
///
/// # Safety
/// `data` must point to at least `len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_decoder_create(data: *const u8, len: usize) -> *mut JxlRsDecoder {
    if data.is_null() || len == 0 {
        return core::ptr::null_mut();
    }
    let bytes = unsafe { core::slice::from_raw_parts(data, len) }.to_vec();

    // Scan pass: collect basic info, embedded color profile (for ICC), and
    // visible-frame info (durations + animation flag).
    let mut scan_options = JxlDecoderOptions::default();
    scan_options.scan_frames_only = true;

    let mut scan_decoder = JxlDecoderInner::new(scan_options);
    let mut input: &[u8] = &bytes;
    if drive_until_basic_info(&mut scan_decoder, &mut input).is_err() {
        return core::ptr::null_mut();
    }

    // Drain the scan decoder so it walks every frame and populates
    // `scanned_frames`. We don't need pixel data here; pass `None` for
    // output buffers.
    loop {
        match scan_decoder.process(&mut input, None) {
            Ok(ProcessingResult::Complete { .. }) => {
                if !scan_decoder.has_more_frames() {
                    break;
                }
                if input.is_empty() {
                    break;
                }
            }
            Ok(ProcessingResult::NeedsMoreInput { .. }) => {
                if input.is_empty() {
                    break;
                }
            }
            Err(_) => return core::ptr::null_mut(),
        }
    }

    let info_ref = match scan_decoder.basic_info() {
        Some(info) => info,
        None => return core::ptr::null_mut(),
    };
    let info = fill_basic_info(info_ref, scan_decoder.embedded_color_profile());
    let extra_channels = info_ref.extra_channels.len();

    let icc_profile = scan_decoder
        .output_color_profile()
        .or_else(|| scan_decoder.embedded_color_profile())
        .and_then(|p| p.try_as_icc().map(|cow| cow.into_owned()))
        .unwrap_or_default();

    let mut scanned: Vec<ScannedFrame> = scan_decoder
        .scanned_frames()
        .iter()
        .map(|f| ScannedFrame {
            duration_ms: f.duration_ms,
            seek_target: f.seek_target,
        })
        .collect();
    if scanned.is_empty() {
        // Still images don't get reported via `scanned_frames` in some
        // configurations; synthesize a single seek target at offset 0 so the
        // pixel decoder can still produce frame 0.
        scanned.push(ScannedFrame {
            duration_ms: 0.0,
            seek_target: VisibleFrameSeekTarget {
                decode_start_file_offset: 0,
                remaining_in_box: 0,
                visible_frames_to_skip: 0,
            },
        });
    }

    let decoder = Box::new(JxlRsDecoder {
        data: bytes,
        info,
        icc_profile,
        scanned,
        extra_channels,
        pixel: None,
    });

    Box::into_raw(decoder)
}

/// Destroy a decoder created with `jxl_rs_decoder_create`.
///
/// # Safety
/// `decoder` must be a pointer returned by `jxl_rs_decoder_create` (or null).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_decoder_destroy(decoder: *mut JxlRsDecoder) {
    if decoder.is_null() {
        return;
    }
    unsafe {
        drop(Box::from_raw(decoder));
    }
}

/// Copy cached basic info into `out_info`.
///
/// # Safety
/// `decoder` must be a valid pointer from `jxl_rs_decoder_create`. `out_info`
/// must be a writable pointer to a `JxlRsBasicInfo`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_decoder_basic_info(
    decoder: *const JxlRsDecoder,
    out_info: *mut JxlRsBasicInfo,
) -> JxlRsStatus {
    if decoder.is_null() || out_info.is_null() {
        return JxlRsStatus::Error;
    }
    let decoder = unsafe { &*decoder };
    let info = JxlRsBasicInfo {
        width: decoder.info.width,
        height: decoder.info.height,
        bits_per_sample: decoder.info.bits_per_sample,
        num_extra_channels: decoder.info.num_extra_channels,
        has_alpha: decoder.info.has_alpha,
        alpha_premultiplied: decoder.info.alpha_premultiplied,
        have_animation: decoder.info.have_animation,
        animation_loop_count: decoder.info.animation_loop_count,
        animation_tps_numerator: decoder.info.animation_tps_numerator,
        animation_tps_denominator: decoder.info.animation_tps_denominator,
        uses_original_profile: decoder.info.uses_original_profile,
        orientation: decoder.info.orientation,
        is_grayscale: decoder.info.is_grayscale,
    };
    unsafe {
        core::ptr::write(out_info, info);
    }
    JxlRsStatus::Success
}

/// Number of visible frames in the codestream.
///
/// # Safety
/// `decoder` must be a valid pointer from `jxl_rs_decoder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_decoder_frame_count(decoder: *const JxlRsDecoder) -> usize {
    if decoder.is_null() {
        return 0;
    }
    let decoder = unsafe { &*decoder };
    decoder.scanned.len()
}

/// Duration of the visible frame at `index` in milliseconds.
///
/// Returns 0 for out-of-range indices.
///
/// # Safety
/// `decoder` must be a valid pointer from `jxl_rs_decoder_create`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_decoder_frame_duration_ms(decoder: *const JxlRsDecoder, index: usize) -> f64 {
    if decoder.is_null() {
        return 0.0;
    }
    let decoder = unsafe { &*decoder };
    decoder.scanned.get(index).map(|f| f.duration_ms).unwrap_or(0.0)
}

/// Borrow the cached ICC profile bytes from the decoder.
///
/// On return, `*out_data` and `*out_len` describe a buffer owned by the
/// decoder and valid for as long as the decoder lives. If the codestream
/// did not provide an ICC profile, `*out_data` is null and `*out_len` is 0.
///
/// # Safety
/// `decoder` must be a valid pointer from `jxl_rs_decoder_create`. `out_data`
/// and `out_len` must be writable pointers.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_decoder_icc_profile(
    decoder: *const JxlRsDecoder,
    out_data: *mut *const u8,
    out_len: *mut usize,
) -> JxlRsStatus {
    if decoder.is_null() || out_data.is_null() || out_len.is_null() {
        return JxlRsStatus::Error;
    }
    let decoder = unsafe { &*decoder };
    unsafe {
        if decoder.icc_profile.is_empty() {
            *out_data = core::ptr::null();
            *out_len = 0;
        } else {
            *out_data = decoder.icc_profile.as_ptr();
            *out_len = decoder.icc_profile.len();
        }
    }
    JxlRsStatus::Success
}

/// Decode visible frame `index` into `out_pixels` as RGBA8 with the given
/// row stride.
///
/// Frames are decoded sequentially. Requesting an earlier frame than the next
/// one to be produced rewinds the underlying streaming decoder transparently.
///
/// # Safety
/// `decoder` must be a valid pointer from `jxl_rs_decoder_create`. `out_pixels`
/// must be writable for at least `row_stride * height` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn jxl_rs_decoder_decode_frame(
    decoder: *mut JxlRsDecoder,
    index: usize,
    premultiply_alpha: bool,
    out_pixels: *mut u8,
    out_pixels_len: usize,
    width: u32,
    height: u32,
    row_stride: usize,
) -> JxlRsStatus {
    if decoder.is_null() || out_pixels.is_null() {
        return JxlRsStatus::Error;
    }
    let decoder = unsafe { &mut *decoder };

    if index >= decoder.scanned.len() {
        return JxlRsStatus::Error;
    }
    if width != decoder.info.width || height != decoder.info.height {
        return JxlRsStatus::Error;
    }

    let bytes_per_row = (width as usize).saturating_mul(4);
    if row_stride < bytes_per_row {
        return JxlRsStatus::Error;
    }
    let needed = match row_stride.checked_mul(height as usize) {
        Some(v) => v,
        None => return JxlRsStatus::Error,
    };
    if out_pixels_len < needed {
        return JxlRsStatus::Error;
    }

    if let Err(status) = decoder.ensure_pixel_decoder(premultiply_alpha) {
        return status;
    }

    let seek_target = decoder.scanned[index].seek_target;
    if seek_target.decode_start_file_offset > decoder.data.len() {
        return JxlRsStatus::Error;
    }

    let pixels = unsafe { core::slice::from_raw_parts_mut(out_pixels, out_pixels_len) };
    let output_buffer = JxlOutputBuffer::new_with_stride(pixels, height as usize, bytes_per_row, row_stride);
    let mut buffers = [output_buffer];

    let pixel = decoder.pixel.as_mut().expect("pixel decoder initialized");

    // Reset frame-level state and rewind the box parser to the seek point.
    pixel.decoder.start_new_frame(seek_target);

    let mut input: &[u8] = &decoder.data[seek_target.decode_start_file_offset..];

    // Each visible frame is produced by two `process()` stages:
    //   (a) parse the frame header (no output buffers), then
    //   (b) render the frame (with output buffers).
    // After (b) the decoder transitions back out of "with frame info", which
    // we observe via `frame_header()` going from Some to None.
    loop {
        let had_frame_header = pixel.decoder.frame_header().is_some();
        let process_result = if had_frame_header {
            pixel.decoder.process(&mut input, Some(&mut buffers))
        } else {
            pixel.decoder.process(&mut input, None)
        };
        match process_result {
            Ok(ProcessingResult::Complete { .. }) => {
                if had_frame_header && pixel.decoder.frame_header().is_none() {
                    return JxlRsStatus::Success;
                }
            }
            Ok(ProcessingResult::NeedsMoreInput { .. }) => {
                if input.is_empty() {
                    return JxlRsStatus::NeedMoreInput;
                }
            }
            Err(_) => return JxlRsStatus::Error,
        }
    }
}
