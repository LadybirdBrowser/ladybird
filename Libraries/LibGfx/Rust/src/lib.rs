/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use yuv::{YuvPlanarImage, YuvRange, YuvStandardMatrix};

#[repr(u8)]
pub enum YUVRange {
    Limited = 0,
    Full = 1,
}

#[repr(u8)]
pub enum YUVMatrix {
    Bt709 = 0,
    Fcc = 1,
    Bt470BG = 2,
    Bt601 = 3,
    Smpte240 = 4,
    Bt2020 = 5,
}

impl From<YUVRange> for YuvRange {
    fn from(range: YUVRange) -> Self {
        match range {
            YUVRange::Limited => YuvRange::Limited,
            YUVRange::Full => YuvRange::Full,
        }
    }
}

impl From<YUVMatrix> for YuvStandardMatrix {
    fn from(matrix: YUVMatrix) -> Self {
        match matrix {
            YUVMatrix::Bt709 => YuvStandardMatrix::Bt709,
            YUVMatrix::Fcc => YuvStandardMatrix::Fcc,
            YUVMatrix::Bt470BG => YuvStandardMatrix::Bt470_6,
            YUVMatrix::Bt601 => YuvStandardMatrix::Bt601,
            YUVMatrix::Smpte240 => YuvStandardMatrix::Smpte240,
            YUVMatrix::Bt2020 => YuvStandardMatrix::Bt2020,
        }
    }
}

/// # Safety
/// All plane pointers must be valid for the specified dimensions and strides.
/// `dst` must point to a buffer of at least `dst_stride * height` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn yuv_u8_to_rgba(
    y_plane: *const u8,
    y_stride: u32,
    u_plane: *const u8,
    u_stride: u32,
    v_plane: *const u8,
    v_stride: u32,
    width: u32,
    height: u32,
    subsampling_x: bool,
    subsampling_y: bool,
    dst: *mut u8,
    dst_stride: u32,
    range: YUVRange,
    matrix: YUVMatrix,
) -> bool {
    let y_len = y_stride as usize * height as usize;
    let uv_height = if subsampling_y { height.div_ceil(2) } else { height } as usize;
    let uv_len = u_stride as usize * uv_height;

    let planar_image = YuvPlanarImage {
        y_plane: unsafe { core::slice::from_raw_parts(y_plane, y_len) },
        y_stride,
        u_plane: unsafe { core::slice::from_raw_parts(u_plane, uv_len) },
        u_stride,
        v_plane: unsafe { core::slice::from_raw_parts(v_plane, uv_len) },
        v_stride,
        width,
        height,
    };

    let dst_len = dst_stride as usize * height as usize;
    let dst_slice = unsafe { core::slice::from_raw_parts_mut(dst, dst_len) };

    let result = match (subsampling_x, subsampling_y) {
        (true, true) => yuv::yuv420_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
        (true, false) => yuv::yuv422_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
        (false, true) => return false,
        (false, false) => yuv::yuv444_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
    };

    result.is_ok()
}

/// # Safety
/// All plane pointers must be valid for the specified dimensions and strides.
/// `dst` must point to a buffer of at least `dst_stride * height` bytes.
/// Values in the u16 planes must be in 0-1023 range for 10-bit or 0-4095 for 12-bit.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn yuv_u16_to_rgba(
    y_plane: *const u16,
    y_stride: u32,
    u_plane: *const u16,
    u_stride: u32,
    v_plane: *const u16,
    v_stride: u32,
    width: u32,
    height: u32,
    bit_depth: u8,
    subsampling_x: bool,
    subsampling_y: bool,
    dst: *mut u8,
    dst_stride: u32,
    range: YUVRange,
    matrix: YUVMatrix,
) -> bool {
    let y_len = y_stride as usize * height as usize;
    let uv_height = if subsampling_y { height.div_ceil(2) } else { height } as usize;
    let uv_len = u_stride as usize * uv_height;

    let planar_image = YuvPlanarImage {
        y_plane: unsafe { core::slice::from_raw_parts(y_plane, y_len) },
        y_stride,
        u_plane: unsafe { core::slice::from_raw_parts(u_plane, uv_len) },
        u_stride,
        v_plane: unsafe { core::slice::from_raw_parts(v_plane, uv_len) },
        v_stride,
        width,
        height,
    };

    let dst_len = dst_stride as usize * height as usize;
    let dst_slice = unsafe { core::slice::from_raw_parts_mut(dst, dst_len) };

    let result = if bit_depth <= 10 {
        match (subsampling_x, subsampling_y) {
            (true, true) => yuv::i010_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
            (true, false) => yuv::i210_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
            (false, true) => return false,
            (false, false) => yuv::i410_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
        }
    } else {
        // 12-bit 4:4:4 has no 8-bit RGBA output; shift to 10-bit and use I410.
        if !subsampling_x && !subsampling_y {
            let y_10: Vec<u16> = unsafe { core::slice::from_raw_parts(y_plane, y_len) }
                .iter()
                .map(|&v| v >> 2)
                .collect();
            let u_10: Vec<u16> = unsafe { core::slice::from_raw_parts(u_plane, uv_len) }
                .iter()
                .map(|&v| v >> 2)
                .collect();
            let v_10: Vec<u16> = unsafe { core::slice::from_raw_parts(v_plane, uv_len) }
                .iter()
                .map(|&v| v >> 2)
                .collect();
            let planar_10 = YuvPlanarImage {
                y_plane: &y_10,
                y_stride,
                u_plane: &u_10,
                u_stride,
                v_plane: &v_10,
                v_stride,
                width,
                height,
            };
            return yuv::i410_to_rgba(&planar_10, dst_slice, dst_stride, range.into(), matrix.into()).is_ok();
        }
        match (subsampling_x, subsampling_y) {
            (true, true) => yuv::i012_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
            (true, false) => yuv::i212_to_rgba(&planar_image, dst_slice, dst_stride, range.into(), matrix.into()),
            (false, true) => return false,
            (false, false) => unreachable!(),
        }
    };

    result.is_ok()
}
