/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use yuv::YuvBiPlanarImage;
use yuv::YuvConversionMode;
use yuv::YuvPlanarImage;
use yuv::YuvRange;
use yuv::YuvStandardMatrix;

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
        y_plane: unsafe { ak::slice_from_raw_parts(y_plane, y_len) },
        y_stride,
        u_plane: unsafe { ak::slice_from_raw_parts(u_plane, uv_len) },
        u_stride,
        v_plane: unsafe { ak::slice_from_raw_parts(v_plane, uv_len) },
        v_stride,
        width,
        height,
    };

    let dst_len = dst_stride as usize * height as usize;
    let dst_slice = unsafe { ak::slice_from_raw_parts_mut(dst, dst_len) };

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
        y_plane: unsafe { ak::slice_from_raw_parts(y_plane, y_len) },
        y_stride,
        u_plane: unsafe { ak::slice_from_raw_parts(u_plane, uv_len) },
        u_stride,
        v_plane: unsafe { ak::slice_from_raw_parts(v_plane, uv_len) },
        v_stride,
        width,
        height,
    };

    let dst_len = dst_stride as usize * height as usize;
    let dst_slice = unsafe { ak::slice_from_raw_parts_mut(dst, dst_len) };

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
            let y_10: Vec<u16> = unsafe { ak::slice_from_raw_parts(y_plane, y_len) }
                .iter()
                .map(|&v| v >> 2)
                .collect();
            let u_10: Vec<u16> = unsafe { ak::slice_from_raw_parts(u_plane, uv_len) }
                .iter()
                .map(|&v| v >> 2)
                .collect();
            let v_10: Vec<u16> = unsafe { ak::slice_from_raw_parts(v_plane, uv_len) }
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

/// The number of elements a plane spans when its rows start `stride` elements apart, but its last row may end right
/// after its `row_length` elements, without padding up to the stride.
fn biplanar_plane_length(stride: u32, row_length: usize, rows: usize) -> Option<usize> {
    (stride as usize)
        .checked_mul(rows.checked_sub(1)?)?
        .checked_add(row_length)
}

/// Converts a bi-planar image whose planes' last rows may end without padding up to their strides. The converter walks
/// its planes a whole stride at a time, or two luma strides at a time where chroma is subsampled vertically, and leaves
/// out any group of rows that doesn't fill its strides. So every group but the last converts in place, and the last one
/// converts from a copy with its rows packed together.
#[allow(clippy::too_many_arguments)]
fn convert_biplanar<T: Copy + std::fmt::Debug>(
    y_plane: &[T],
    y_stride: u32,
    uv_plane: &[T],
    uv_stride: u32,
    width: u32,
    height: u32,
    chroma_row_length: usize,
    subsampling_y: bool,
    dst: &mut [u8],
    dst_stride: u32,
    convert: impl Fn(&YuvBiPlanarImage<T>, &mut [u8], u32) -> bool,
) -> bool {
    if height == 0 {
        return false;
    }
    let rows_per_chroma_row = if subsampling_y { 2 } else { 1 };
    let last_group_rows = (height - 1) % rows_per_chroma_row + 1;
    let leading_rows = height - last_group_rows;
    let leading_chroma_rows = (leading_rows / rows_per_chroma_row) as usize;

    if leading_rows > 0 {
        let leading = YuvBiPlanarImage {
            y_plane: &y_plane[..y_stride as usize * leading_rows as usize],
            y_stride,
            uv_plane: &uv_plane[..uv_stride as usize * leading_chroma_rows],
            uv_stride,
            width,
            height: leading_rows,
        };
        if !convert(
            &leading,
            &mut dst[..dst_stride as usize * leading_rows as usize],
            dst_stride,
        ) {
            return false;
        }
    }

    let row_length = width as usize;
    let mut last_y_rows = Vec::with_capacity(row_length * last_group_rows as usize);
    for row in leading_rows..height {
        let start = y_stride as usize * row as usize;
        last_y_rows.extend_from_slice(&y_plane[start..start + row_length]);
    }
    let chroma_start = uv_stride as usize * leading_chroma_rows;
    let last = YuvBiPlanarImage {
        y_plane: &last_y_rows,
        y_stride: width,
        uv_plane: &uv_plane[chroma_start..chroma_start + chroma_row_length],
        uv_stride: chroma_row_length as u32,
        width,
        height: last_group_rows,
    };
    convert(
        &last,
        &mut dst[dst_stride as usize * leading_rows as usize..],
        dst_stride,
    )
}

/// # Safety
/// All plane pointers must be valid for the specified dimensions and strides.
/// Each plane's last row may end without padding up to its stride.
/// `dst` must point to a buffer of at least `dst_stride * height` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn yuv_biplanar_u8_to_rgba(
    y_plane: *const u8,
    y_stride: u32,
    uv_plane: *const u8,
    uv_stride: u32,
    width: u32,
    height: u32,
    subsampling_x: bool,
    subsampling_y: bool,
    dst: *mut u8,
    dst_stride: u32,
    range: YUVRange,
    matrix: YUVMatrix,
) -> bool {
    let Some(y_len) = biplanar_plane_length(y_stride, width as usize, height as usize) else {
        return false;
    };
    let chroma_width = if subsampling_x { width.div_ceil(2) } else { width } as usize;
    let chroma_height = if subsampling_y { height.div_ceil(2) } else { height } as usize;
    let Some(uv_len) = biplanar_plane_length(uv_stride, chroma_width * 2, chroma_height) else {
        return false;
    };

    let y_plane = unsafe { ak::slice_from_raw_parts(y_plane, y_len) };
    let uv_plane = unsafe { ak::slice_from_raw_parts(uv_plane, uv_len) };

    let dst_len = dst_stride as usize * height as usize;
    let dst_slice = unsafe { ak::slice_from_raw_parts_mut(dst, dst_len) };

    let convert = match (subsampling_x, subsampling_y) {
        (true, true) => yuv::yuv_nv12_to_rgba,
        (true, false) => yuv::yuv_nv16_to_rgba,
        (false, true) => return false,
        (false, false) => yuv::yuv_nv24_to_rgba,
    };
    let range: YuvRange = range.into();
    let matrix: YuvStandardMatrix = matrix.into();

    convert_biplanar(
        y_plane,
        y_stride,
        uv_plane,
        uv_stride,
        width,
        height,
        chroma_width * 2,
        subsampling_y,
        dst_slice,
        dst_stride,
        |image, dst, dst_stride| convert(image, dst, dst_stride, range, matrix, YuvConversionMode::default()).is_ok(),
    )
}

/// # Safety
/// All plane pointers must be valid for the specified dimensions and strides.
/// Strides are in components per row, not bytes.
/// Each plane's last row may end without padding up to its stride.
/// `dst` must point to a buffer of at least `dst_stride * height` bytes.
/// Samples must sit in the high 10 bits of each u16.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn yuv_biplanar_u16_to_rgba(
    y_plane: *const u16,
    y_stride: u32,
    uv_plane: *const u16,
    uv_stride: u32,
    width: u32,
    height: u32,
    subsampling_x: bool,
    subsampling_y: bool,
    dst: *mut u8,
    dst_stride: u32,
    range: YUVRange,
    matrix: YUVMatrix,
) -> bool {
    let Some(y_len) = biplanar_plane_length(y_stride, width as usize, height as usize) else {
        return false;
    };
    let chroma_width = if subsampling_x { width.div_ceil(2) } else { width } as usize;
    let chroma_height = if subsampling_y { height.div_ceil(2) } else { height } as usize;
    let Some(uv_len) = biplanar_plane_length(uv_stride, chroma_width * 2, chroma_height) else {
        return false;
    };

    let y_plane = unsafe { ak::slice_from_raw_parts(y_plane, y_len) };
    let uv_plane = unsafe { ak::slice_from_raw_parts(uv_plane, uv_len) };

    let dst_len = dst_stride as usize * height as usize;
    let dst_slice = unsafe { ak::slice_from_raw_parts_mut(dst, dst_len) };

    let convert = match (subsampling_x, subsampling_y) {
        (true, true) => yuv::p010_to_rgba,
        (true, false) => yuv::p210_to_rgba,
        (false, true) => return false,
        (false, false) => yuv::p410_to_rgba,
    };
    let range: YuvRange = range.into();
    let matrix: YuvStandardMatrix = matrix.into();

    convert_biplanar(
        y_plane,
        y_stride,
        uv_plane,
        uv_stride,
        width,
        height,
        chroma_width * 2,
        subsampling_y,
        dst_slice,
        dst_stride,
        |image, dst, dst_stride| convert(image, dst, dst_stride, range, matrix, YuvConversionMode::default()).is_ok(),
    )
}
