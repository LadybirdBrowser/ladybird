/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! How the window server is told the colors of a frame of a video it shows: as values of the frame's IOSurface where a
//! layer shows the surface as its contents, and as attachments of a sample buffer where AVFoundation shows the frame.

use super::frameworks::*;
use super::quartz_core::SampleBufferDisplayLayer;
use super::size_of_io_surface;
use crate::raster_host::{ContentLightLevelInformation, MasteringDisplayColorVolume};
use crate::window_server::{TRANSFER_CHARACTERISTICS_HLG, TRANSFER_CHARACTERISTICS_SMPTE_ST_2084, VideoColorMetadata};
use libgfx_rust::IntSize;
use std::ffi::CStr;

// The color primaries of Display P3, and of BT.2020, among the coding-independent code points.
const COLOR_PRIMARIES_SMPTE_EG_432: u8 = 12;
const COLOR_PRIMARIES_BT_2020: u8 = 9;
// The matrix coefficients of RGB, among the coding-independent code points.
pub(super) const MATRIX_COEFFICIENTS_IDENTITY: u8 = 0;

/// A string that keys a value, made once per thread.
fn with_key<R>(cache: &'static std::thread::LocalKey<CfRetained>, use_key: impl FnOnce(CFStringRef) -> R) -> R {
    cache.with(|key| use_key(key.as_ptr()))
}

thread_local! {
    static Y_CB_CR_MATRIX: CfRetained = CfRetained::string(c"IOSurfaceYCbCrMatrix");
    static COLOR_PRIMARIES: CfRetained = CfRetained::string(c"IOSurfaceColorPrimaries");
    static TRANSFER_FUNCTION: CfRetained = CfRetained::string(c"IOSurfaceTransferFunction");
    static COLOR_SPACE: CfRetained = CfRetained::string(c"IOSurfaceColorSpace");
}

/// The window server converts a YCbCr surface to RGB by the matrix it is tagged with. It decodes the transfer of SDR
/// video by a curve of its own whatever the transfer is tagged as, but the colour space of the surface overrides that,
/// and naming it after sRGB draws the video as the frame draws it. PQ and HLG video is tagged as what it is.
pub(super) fn describe_colours_of_video_surface(surface: IOSurfaceRef, color: &VideoColorMetadata) {
    // SAFETY: The surface is live, and every value is a CFString constant of the frameworks or one they answer.
    unsafe {
        let matrix = CVYCbCrMatrixGetStringForIntegerCodePoint(i32::from(color.matrix_coefficients));
        let primaries = CVColorPrimariesGetStringForIntegerCodePoint(i32::from(color.color_primaries));
        let matrix = if matrix.is_null() {
            kCVImageBufferYCbCrMatrix_ITU_R_709_2
        } else {
            matrix
        };
        let primaries = if primaries.is_null() {
            kCVImageBufferColorPrimaries_ITU_R_709_2
        } else {
            primaries
        };
        with_key(&Y_CB_CR_MATRIX, |key| IOSurfaceSetValue(surface, key, matrix));
        with_key(&COLOR_PRIMARIES, |key| IOSurfaceSetValue(surface, key, primaries));

        let mut transfer_function = kCVImageBufferTransferFunction_sRGB;
        let mut color_space_name = kCGColorSpaceSRGB;
        // The colour spaces named after ITU-R BT.2100 have BT.2020 primaries, so Display P3 video has names of its own.
        let has_display_p3_primaries = color.color_primaries == COLOR_PRIMARIES_SMPTE_EG_432;
        if color.transfer_characteristics == TRANSFER_CHARACTERISTICS_SMPTE_ST_2084 {
            transfer_function = kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ;
            color_space_name = if has_display_p3_primaries {
                kCGColorSpaceDisplayP3_PQ
            } else {
                kCGColorSpaceITUR_2100_PQ
            };
        } else if color.transfer_characteristics == TRANSFER_CHARACTERISTICS_HLG {
            transfer_function = kCVImageBufferTransferFunction_ITU_R_2100_HLG;
            color_space_name = if has_display_p3_primaries {
                kCGColorSpaceDisplayP3_HLG
            } else {
                kCGColorSpaceITUR_2100_HLG
            };
        } else if has_display_p3_primaries {
            color_space_name = kCGColorSpaceDisplayP3;
        } else if color.color_primaries == COLOR_PRIMARIES_BT_2020 {
            color_space_name = kCGColorSpaceITUR_2020_sRGBGamma;
        }
        with_key(&TRANSFER_FUNCTION, |key| {
            IOSurfaceSetValue(surface, key, transfer_function);
        });
        with_key(&COLOR_SPACE, |key| IOSurfaceSetValue(surface, key, color_space_name));
    }
}

fn set_pixel_buffer_attachment(pixel_buffer: CVPixelBufferRef, key: CFStringRef, value: CFTypeRef) {
    if value.is_null() {
        return;
    }
    // SAFETY: The pixel buffer, the key and the value are live.
    unsafe { CVBufferSetAttachment(pixel_buffer, key, value, CV_ATTACHMENT_MODE_SHOULD_PROPAGATE) };
}

// An HLG video is given an ambient viewing environment, whose values were found not to change how AVFoundation shows
// the video. This is the one that iPhone HDR video carries.
// https://developer.apple.com/documentation/technotes/tn3145-hdr-video-metadata
fn ambient_viewing_environment() -> CfRetained {
    const ILLUMINANCE_IN_TEN_THOUSANDTHS_OF_LUX: u32 = 3_140_000;
    const WHITE_POINT_X: u16 = 15635;
    const WHITE_POINT_Y: u16 = 16450;
    let mut bytes = Vec::with_capacity(8);
    bytes.extend_from_slice(&ILLUMINANCE_IN_TEN_THOUSANDTHS_OF_LUX.to_be_bytes());
    bytes.extend_from_slice(&WHITE_POINT_X.to_be_bytes());
    bytes.extend_from_slice(&WHITE_POINT_Y.to_be_bytes());
    CfRetained::data(&bytes)
}

/// The payload of an HEVC mastering display colour volume SEI message, which lists the primaries as green, blue, red.
fn mastering_display_color_volume(volume: &MasteringDisplayColorVolume) -> CfRetained {
    const CHROMATICITY_UNITS_PER_UNIT: f32 = 50_000.0;
    const LUMINANCE_UNITS_PER_CANDELA_PER_SQUARE_METRE: f32 = 10_000.0;
    let mut bytes = Vec::with_capacity(24);
    for chromaticity in [volume.green, volume.blue, volume.red, volume.white_point] {
        for coordinate in chromaticity {
            bytes.extend_from_slice(&((coordinate * CHROMATICITY_UNITS_PER_UNIT).round() as u16).to_be_bytes());
        }
    }
    for luminance in [volume.maximum_luminance, volume.minimum_luminance] {
        bytes.extend_from_slice(
            &((luminance * LUMINANCE_UNITS_PER_CANDELA_PER_SQUARE_METRE).round() as u32).to_be_bytes(),
        );
    }
    CfRetained::data(&bytes)
}

fn content_light_level_information(information: &ContentLightLevelInformation) -> CfRetained {
    let mut bytes = Vec::with_capacity(4);
    bytes.extend_from_slice(&information.maximum_content_light_level.to_be_bytes());
    bytes.extend_from_slice(&information.maximum_frame_average_light_level.to_be_bytes());
    CfRetained::data(&bytes)
}

fn log_failure(what: &CStr, status: i32) {
    eprintln!("Compositor: Failed to {}: {status}", what.to_string_lossy());
}

/// Hands the surface of a frame to AVFoundation, which shows it at `shown_size` of its top left as soon as it is
/// enqueued.
pub(super) fn enqueue_video_surface(
    layer: &SampleBufferDisplayLayer,
    surface: IOSurfaceRef,
    color: &VideoColorMetadata,
    shown_size: IntSize,
) {
    let mut pixel_buffer: CVPixelBufferRef = std::ptr::null_mut();
    // SAFETY: The surface is live, and the pixel buffer is written on success.
    let result =
        unsafe { CVPixelBufferCreateWithIOSurface(std::ptr::null(), surface, std::ptr::null(), &raw mut pixel_buffer) };
    if result != CV_RETURN_SUCCESS {
        log_failure(c"wrap a video surface in a pixel buffer", result);
        return;
    }
    // SAFETY: The pixel buffer is new, and this owns it.
    let Some(pixel_buffer) = (unsafe { CfRetained::adopt(pixel_buffer.cast_const()) }) else {
        return;
    };

    // SAFETY: The keys are CFString constants of CoreVideo, and the values are CFStrings it answers or live objects.
    unsafe {
        set_pixel_buffer_attachment(
            pixel_buffer.as_mut_ptr(),
            kCVImageBufferColorPrimariesKey,
            CVColorPrimariesGetStringForIntegerCodePoint(i32::from(color.color_primaries)),
        );
        set_pixel_buffer_attachment(
            pixel_buffer.as_mut_ptr(),
            kCVImageBufferTransferFunctionKey,
            CVTransferFunctionGetStringForIntegerCodePoint(i32::from(color.transfer_characteristics)),
        );
        set_pixel_buffer_attachment(
            pixel_buffer.as_mut_ptr(),
            kCVImageBufferYCbCrMatrixKey,
            CVYCbCrMatrixGetStringForIntegerCodePoint(i32::from(color.matrix_coefficients)),
        );
        if color.transfer_characteristics == TRANSFER_CHARACTERISTICS_HLG {
            let environment = ambient_viewing_environment();
            set_pixel_buffer_attachment(
                pixel_buffer.as_mut_ptr(),
                kCVImageBufferAmbientViewingEnvironmentKey,
                environment.as_ptr(),
            );
        }
        if let Some(volume) = &color.mastering_display_color_volume {
            let volume = mastering_display_color_volume(volume);
            set_pixel_buffer_attachment(
                pixel_buffer.as_mut_ptr(),
                kCVImageBufferMasteringDisplayColorVolumeKey,
                volume.as_ptr(),
            );
        }
        if let Some(information) = &color.content_light_level_information
            && information.maximum_content_light_level != 0
            && information.maximum_frame_average_light_level != 0
        {
            let information = content_light_level_information(information);
            set_pixel_buffer_attachment(
                pixel_buffer.as_mut_ptr(),
                kCVImageBufferContentLightLevelInfoKey,
                information.as_ptr(),
            );
        }

        // A decoder's surface can be larger than its picture, and the clean aperture is centred in the surface.
        let surface_size = size_of_io_surface(surface);
        if shown_size != surface_size {
            let width = CfRetained::number(f64::from(shown_size.width));
            let height = CfRetained::number(f64::from(shown_size.height));
            let horizontal_offset = CfRetained::number(f64::from(shown_size.width - surface_size.width) / 2.0);
            let vertical_offset = CfRetained::number(f64::from(shown_size.height - surface_size.height) / 2.0);
            let clean_aperture = CfRetained::dictionary(&[
                (kCVImageBufferCleanApertureWidthKey, width.as_ptr()),
                (kCVImageBufferCleanApertureHeightKey, height.as_ptr()),
                (
                    kCVImageBufferCleanApertureHorizontalOffsetKey,
                    horizontal_offset.as_ptr(),
                ),
                (kCVImageBufferCleanApertureVerticalOffsetKey, vertical_offset.as_ptr()),
            ]);
            set_pixel_buffer_attachment(
                pixel_buffer.as_mut_ptr(),
                kCVImageBufferCleanApertureKey,
                clean_aperture.as_ptr(),
            );
        }
    }

    let mut format_description: CMVideoFormatDescriptionRef = std::ptr::null();
    // SAFETY: The pixel buffer is live, and the description is written on success.
    let result = unsafe {
        CMVideoFormatDescriptionCreateForImageBuffer(
            std::ptr::null(),
            pixel_buffer.as_mut_ptr(),
            &raw mut format_description,
        )
    };
    if result != 0 {
        log_failure(c"describe the format of a video surface", result);
        return;
    }
    // SAFETY: The description is new, and this owns it.
    let Some(format_description) = (unsafe { CfRetained::adopt(format_description) }) else {
        return;
    };

    // The time is unused, as the sample is shown as soon as it is enqueued.
    // SAFETY: The times are constants of CoreMedia.
    let timing = unsafe {
        CMSampleTimingInfo {
            duration: kCMTimeInvalid,
            presentation_time_stamp: kCMTimeZero,
            decode_time_stamp: kCMTimeInvalid,
        }
    };
    let mut sample_buffer: CMSampleBufferRef = std::ptr::null();
    // SAFETY: The pixel buffer and its description are live, and the sample buffer is written on success.
    let result = unsafe {
        CMSampleBufferCreateReadyWithImageBuffer(
            std::ptr::null(),
            pixel_buffer.as_mut_ptr(),
            format_description.as_ptr(),
            &raw const timing,
            &raw mut sample_buffer,
        )
    };
    if result != 0 {
        log_failure(c"make a sample buffer of a video surface", result);
        return;
    }
    // SAFETY: The sample buffer is new, and this owns it.
    let Some(sample_buffer) = (unsafe { CfRetained::adopt(sample_buffer) }) else {
        return;
    };
    // SAFETY: The sample buffer is live, and its attachments array holds a mutable dictionary per sample, of which it
    // has one.
    unsafe {
        let attachments = CMSampleBufferGetSampleAttachmentsArray(sample_buffer.as_ptr(), 1);
        if attachments.is_null() || CFArrayGetCount(attachments) == 0 {
            return;
        }
        let attachments_of_sample = CFArrayGetValueAtIndex(attachments, 0).cast_mut();
        CFDictionarySetValue(
            attachments_of_sample,
            kCMSampleAttachmentKey_DisplayImmediately,
            kCFBooleanTrue,
        );
    }
    layer.enqueue(sample_buffer.as_ptr());
}
