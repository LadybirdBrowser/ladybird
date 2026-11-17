/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::ffi::{bytes_from_raw, catch_panic_or};

const ICON_TYPE_ICO: u16 = 1;
const ICON_TYPE_CUR: u16 = 2;
const ICONDIR_SIZE: usize = 6;
const ICONDIRENTRY_SIZE: usize = 16;

/// The image of an ICO or CUR file that should be displayed.
#[repr(C)]
pub struct ICOImageInfo {
    pub width: u32,
    pub height: u32,
    pub image_offset: usize,
    pub image_size: usize,
}

fn read_u16(data: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(*data.get(offset..)?.first_chunk()?))
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(*data.get(offset..)?.first_chunk()?))
}

// Returns the number of images in the file.
fn decode_icondir(data: &[u8]) -> Option<u16> {
    if read_u16(data, 0)? != 0 {
        return None;
    }
    let icon_type = read_u16(data, 2)?;
    if icon_type != ICON_TYPE_ICO && icon_type != ICON_TYPE_CUR {
        return None;
    }
    read_u16(data, 4)
}

fn decode_directory(data: &[u8]) -> Option<ICOImageInfo> {
    let image_count = decode_icondir(data)?;
    if image_count == 0 {
        return None;
    }

    let mut largest: Option<(ICOImageInfo, u64, u16)> = None;
    for index in 0..usize::from(image_count) {
        let entry = data.get(ICONDIR_SIZE + index * ICONDIRENTRY_SIZE..)?;
        let entry = entry.get(..ICONDIRENTRY_SIZE)?;

        // A width or height of 0 means 256.
        let width = if entry[0] == 0 { 256 } else { u32::from(entry[0]) };
        let height = if entry[1] == 0 { 256 } else { u32::from(entry[1]) };
        // NB: In CUR files, this field holds the vertical hotspot coordinate instead.
        let bits_per_pixel = read_u16(entry, 6)?;
        let image_size = read_u32(entry, 8)? as usize;
        let image_offset = read_u32(entry, 12)? as usize;
        if image_offset.checked_add(image_size)? > data.len() {
            return None;
        }

        // Pick the largest image, and among those the one with the most bits per pixel.
        let area = u64::from(width) * u64::from(height);
        let is_better = match &largest {
            None => true,
            Some((_, largest_area, largest_bits_per_pixel)) => {
                area > *largest_area || (area == *largest_area && bits_per_pixel > *largest_bits_per_pixel)
            }
        };
        if is_better {
            let info = ICOImageInfo {
                width,
                height,
                image_offset,
                image_size,
            };
            largest = Some((info, area, bits_per_pixel));
        }
    }
    largest.map(|(info, _, _)| info)
}

/// # Safety
/// `data` must be valid for `data_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ico_rust_sniff(data: *const u8, data_len: usize) -> bool {
    catch_panic_or(false, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return false;
        };
        decode_icondir(data).is_some()
    })
}

/// # Safety
/// - `data` must be valid for `data_len` bytes.
/// - `out_info` must be a valid writable pointer.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ico_rust_decode_directory(
    data: *const u8,
    data_len: usize,
    out_info: *mut ICOImageInfo,
) -> bool {
    catch_panic_or(false, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return false;
        };
        let Some(out_info) = (unsafe { out_info.as_mut() }) else {
            return false;
        };
        let Some(info) = decode_directory(data) else {
            return false;
        };
        *out_info = info;
        true
    })
}
