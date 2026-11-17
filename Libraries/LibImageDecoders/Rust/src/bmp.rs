/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use crate::ffi::{bytes_from_raw, catch_panic_or};

const BMP_FILE_HEADER_SIZE: usize = 14;
const COLOR_PALETTE_LIMIT: u32 = 1024;
const MAX_RLE_BUFFER_SIZE: usize = 300 * 1024 * 1024;

// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/4e588f70-bd92-4a6f-b77f-35d0feaf7a57
mod compression {
    pub const RGB: u32 = 0;
    pub const RLE8: u32 = 1;
    pub const RLE4: u32 = 2;
    pub const BITFIELDS: u32 = 3;
    // NB: This doubles as JPEG for V4+ headers, which we do not support.
    pub const RLE24: u32 = 4;
    pub const ALPHABITFIELDS: u32 = 6;
}

// https://learn.microsoft.com/en-us/openspecs/windows_protocols/ms-wmf/3c289fe1-c42e-42f6-b125-4b5fc49a2b20
const COLOR_SPACE_PROFILE_EMBEDDED: u32 = 0x4D42_4544; // 'MBED'

#[derive(Clone, Copy, PartialEq, Eq, PartialOrd, Ord)]
enum DIBType {
    Core,
    OSV2Short,
    OSV2,
    Info,
    V2,
    V3,
    V4,
    V5,
}

impl DIBType {
    fn from_size(size: usize) -> Option<Self> {
        Some(match size {
            12 => Self::Core,
            16 => Self::OSV2Short,
            64 => Self::OSV2,
            40 => Self::Info,
            52 => Self::V2,
            56 => Self::V3,
            108 => Self::V4,
            124 => Self::V5,
            _ => return None,
        })
    }
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum BMPBitmapFormat {
    BGRx8888,
    BGRA8888,
    RGBx8888,
}

#[repr(u8)]
#[derive(Clone, Copy, PartialEq, Eq)]
pub enum BMPICCProfile {
    Absent,
    Embedded,
    OutOfBounds,
}

#[repr(C)]
pub struct BMPImageInfo {
    pub width: u32,
    pub height: u32,
    pub format: BMPBitmapFormat,
    pub icc_profile: BMPICCProfile,
    pub icc_profile_offset: usize,
    pub icc_profile_size: usize,
}

struct Reader<'a> {
    data: &'a [u8],
}

impl<'a> Reader<'a> {
    fn new(data: &'a [u8]) -> Self {
        Self { data }
    }

    fn remaining(&self) -> usize {
        self.data.len()
    }

    fn is_at_end(&self) -> bool {
        self.data.is_empty()
    }

    fn read_array<const N: usize>(&mut self) -> Option<[u8; N]> {
        let (head, tail) = self.data.split_first_chunk::<N>()?;
        self.data = tail;
        Some(*head)
    }

    fn read_u8(&mut self) -> Option<u8> {
        Some(self.read_array::<1>()?[0])
    }

    fn read_u16(&mut self) -> Option<u16> {
        Some(u16::from_le_bytes(self.read_array()?))
    }

    fn read_u24(&mut self) -> Option<u32> {
        let [b0, b1, b2] = self.read_array()?;
        Some(u32::from_le_bytes([b0, b1, b2, 0]))
    }

    fn read_u32(&mut self) -> Option<u32> {
        Some(u32::from_le_bytes(self.read_array()?))
    }

    fn read_i32(&mut self) -> Option<i32> {
        Some(i32::from_le_bytes(self.read_array()?))
    }

    fn skip(&mut self, count: usize) -> Option<()> {
        self.data = self.data.get(count..)?;
        Some(())
    }
}

#[derive(Clone, Copy)]
struct ColorMask {
    mask: u32,
    // The number of right shifts that align the most significant bit of the mask with bit 7. This can be negative.
    shift: i8,
    // The number of set bits in the mask, capped at 8.
    size: u8,
}

impl ColorMask {
    fn new(mask: u32) -> Self {
        if mask == 0 {
            return Self {
                mask,
                shift: 0,
                size: 0,
            };
        }
        let mut trailing_zeros = mask.trailing_zeros() as i32;
        let mut size = (!(mask >> trailing_zeros)).trailing_zeros() as i32;
        if size > 8 {
            // Drop the lowest bits if the mask is longer than 8 bits.
            trailing_zeros += size - 8;
            size = 8;
        }
        Self {
            mask,
            shift: (size + trailing_zeros - 8) as i8,
            size: size as u8,
        }
    }

    // Scales the masked bits of `data` over the entire 8-bit range, so that for example the 5-bit value 0x1f becomes
    // 0xff instead of 0xf8.
    fn scaled_value(&self, data: u32) -> u8 {
        const SCALING_FACTORS_2BIT: [u8; 4] = [0x00, 0x55, 0xaa, 0xff];
        const SCALING_FACTORS_3BIT: [u8; 8] = [0x00, 0x24, 0x48, 0x6d, 0x91, 0xb6, 0xdb, 0xff];

        let data = data & self.mask;
        let number = if self.shift < 0 {
            (data << -self.shift) as u8
        } else {
            (data >> self.shift) as u8
        };
        match self.size {
            0 => 0,
            1 => {
                if number != 0 {
                    0xff
                } else {
                    0
                }
            }
            2 => SCALING_FACTORS_2BIT[(number >> 6) as usize],
            3 => SCALING_FACTORS_3BIT[(number >> 5) as usize],
            // With 4 or more bits, copying the most significant bits into the least significant bits scales evenly.
            size => (u32::from(number) | (u32::from(number) >> size)) as u8,
        }
    }
}

struct DIBHeader {
    width: i32,
    height: i32,
    bpp: u16,
    compression: u32,
    number_of_palette_colors: u32,
    masks: Vec<u32>,
    color_space: u32,
    profile_data: u32,
    profile_size: u32,
}

pub struct BMPDecoder {
    included_in_ico: bool,
    dib_type: DIBType,
    dib_size: usize,
    data_offset: usize,
    width: usize,
    height: usize,
    top_down: bool,
    bpp: u16,
    compression: u32,
    number_of_palette_colors: u32,
    masks: Vec<ColorMask>,
}

fn decode_file_header(data: &[u8]) -> Option<usize> {
    if data.get(0..2)? != b"BM" {
        return None;
    }
    // NB: The file size field is ignored, since files disagree on whether it holds the size of the file or of the
    //     header. The four bytes after it are reserved.
    let data_offset = u32::from_le_bytes(*data.get(10..14)?.first_chunk()?) as usize;
    if data_offset >= data.len() {
        return None;
    }
    Some(data_offset)
}

fn is_supported_compression(dib_type: DIBType, compression: u32) -> bool {
    matches!(
        compression,
        compression::RGB | compression::BITFIELDS | compression::ALPHABITFIELDS | compression::RLE8 | compression::RLE4
    ) || (compression == compression::RLE24 && dib_type <= DIBType::OSV2)
}

fn decode_core_dib(reader: &mut Reader, dib: &mut DIBHeader, dib_type: DIBType) -> Option<()> {
    // The width and height are u16 fields in BITMAPCOREHEADER.
    if dib_type == DIBType::Core {
        dib.width = i32::from(reader.read_u16()?);
        dib.height = i32::from(reader.read_u16()?);
    } else {
        dib.width = reader.read_i32()?;
        dib.height = reader.read_i32()?;
    }
    if dib.width < 0 || dib.height == i32::MIN {
        return None;
    }
    // INTEROP: The plane count should be 1, but is ignored.
    reader.skip(2)?;
    dib.bpp = reader.read_u16()?;
    match dib.bpp {
        1 | 2 | 4 | 8 | 16 | 24 | 32 => Some(()),
        _ => None,
    }
}

fn decode_osv2_dib(reader: &mut Reader, dib: &mut DIBHeader, dib_type: DIBType) -> Option<()> {
    dib.width = reader.read_i32()?;
    dib.height = reader.read_i32()?;
    if dib.width < 0 || dib.height == i32::MIN {
        return None;
    }
    // INTEROP: The plane count should be 1, but is ignored.
    reader.skip(2)?;
    dib.bpp = reader.read_u16()?;
    // OS/2 did not expect 16 or 32 bpp to be popular.
    if !matches!(dib.bpp, 1 | 2 | 4 | 8 | 24) {
        return None;
    }
    if dib_type == DIBType::OSV2Short {
        return Some(());
    }

    dib.compression = reader.read_u32()?;
    if !is_supported_compression(dib_type, dib.compression) {
        return None;
    }
    // Image size, horizontal resolution, vertical resolution.
    reader.skip(12)?;
    dib.number_of_palette_colors = reader.read_u32()?.min(COLOR_PALETTE_LIMIT);
    // Important colors, units, reserved, recording, halftoning, size1, size2, color encoding, identifier.
    reader.skip(4 + 4 + 4 + 4 + 4 + 8)
}

fn decode_info_dib(reader: &mut Reader, dib: &mut DIBHeader, dib_type: DIBType) -> Option<()> {
    decode_core_dib(reader, dib, dib_type)?;
    dib.compression = reader.read_u32()?;
    if !is_supported_compression(dib_type, dib.compression) {
        return None;
    }
    // Image size, horizontal resolution, vertical resolution.
    reader.skip(12)?;
    dib.number_of_palette_colors = reader.read_u32()?.min(COLOR_PALETTE_LIMIT);
    // Important colors.
    reader.skip(4)
}

fn decode_v2_dib(reader: &mut Reader, dib: &mut DIBHeader) -> Option<()> {
    if dib.compression != compression::BITFIELDS && dib.compression != compression::ALPHABITFIELDS {
        return reader.skip(12);
    }
    for _ in 0..3 {
        dib.masks.push(reader.read_u32()?);
    }
    Some(())
}

fn decode_v3_dib(reader: &mut Reader, dib: &mut DIBHeader) -> Option<()> {
    // There is no comprehensive documentation of when alpha masks apply, so this follows the results of the BMP test
    // suite.
    if dib.compression == compression::ALPHABITFIELDS {
        dib.masks.push(reader.read_u32()?);
        return Some(());
    }
    if dib.bpp < 16 {
        return reader.skip(4);
    }
    let mask = reader.read_u32()?;
    if dib.compression == compression::RGB && dib.bpp == 32 && mask != 0 {
        dib.masks.extend([0xff0000, 0x00ff00, 0x0000ff]);
    }
    if (dib.bpp == 32 && mask != 0) || (dib.bpp == 16 && dib.compression != compression::RGB) {
        dib.masks.push(mask);
    }
    Some(())
}

fn decode_v4_dib(reader: &mut Reader, dib: &mut DIBHeader) -> Option<()> {
    dib.color_space = reader.read_u32()?;
    // Red, green and blue endpoints, then gamma values.
    reader.skip(36 + 12)
}

fn decode_v5_dib(reader: &mut Reader, dib: &mut DIBHeader) -> Option<()> {
    // Intent.
    reader.skip(4)?;
    dib.profile_data = reader.read_u32()?;
    dib.profile_size = reader.read_u32()?;
    // Reserved.
    reader.skip(4)
}

fn is_valid_bitmask_combination(dib_type: DIBType, dib: &DIBHeader) -> bool {
    let bpp = dib.bpp;
    let compression = dib.compression;

    if compression == compression::ALPHABITFIELDS && dib_type != DIBType::Info {
        return false;
    }

    match dib_type {
        DIBType::Core => !matches!(bpp, 2 | 16 | 32),
        DIBType::Info => match compression {
            compression::BITFIELDS | compression::ALPHABITFIELDS => bpp == 16 || bpp == 32,
            compression::RGB => true,
            compression::RLE8 => bpp <= 8,
            compression::RLE4 => bpp <= 4,
            // Other compressions are not officially supported.
            _ => false,
        },
        _ => {
            !(compression == compression::BITFIELDS && bpp != 16 && bpp != 32)
                && !(compression == compression::RLE8 && bpp > 8)
                && !(compression == compression::RLE4 && bpp > 4)
                && !(compression == compression::RLE24 && bpp != 24)
        }
    }
}

fn decode_dib(data: &[u8], included_in_ico: bool) -> Option<(BMPDecoder, BMPImageInfo)> {
    let header_size = if included_in_ico { 0 } else { BMP_FILE_HEADER_SIZE };
    let mut data_offset = if included_in_ico { 0 } else { decode_file_header(data)? };

    let dib_size = u32::from_le_bytes(*data.get(header_size..header_size + 4)?.first_chunk()?) as usize;
    if data.len() < header_size.checked_add(dib_size)? {
        return None;
    }
    if !included_in_ico && data_offset < header_size + dib_size {
        return None;
    }

    // NB: A BMP included in an ICO has no file header, so its data offset is only known after the color table size.
    //     This also means that it cannot contain extra bit masks.
    let dib_offset = if included_in_ico {
        dib_size
    } else {
        data_offset.checked_sub(header_size + 4)?
    };
    let dib_start = header_size + 4;
    if dib_offset.checked_add(dib_start)? >= data.len() {
        return None;
    }
    let mut reader = Reader::new(&data[dib_start..dib_start + dib_offset]);

    let dib_type = DIBType::from_size(dib_size)?;
    let mut dib = DIBHeader {
        width: 0,
        height: 0,
        bpp: 0,
        compression: compression::RGB,
        number_of_palette_colors: 0,
        masks: Vec::new(),
        color_space: 0,
        profile_data: 0,
        profile_size: 0,
    };
    match dib_type {
        DIBType::Core => decode_core_dib(&mut reader, &mut dib, dib_type)?,
        DIBType::OSV2Short | DIBType::OSV2 => decode_osv2_dib(&mut reader, &mut dib, dib_type)?,
        _ => {
            decode_info_dib(&mut reader, &mut dib, dib_type)?;
            if dib_type >= DIBType::V2 {
                decode_v2_dib(&mut reader, &mut dib)?;
            }
            if dib_type >= DIBType::V3 {
                decode_v3_dib(&mut reader, &mut dib)?;
            }
            if dib_type >= DIBType::V4 {
                decode_v4_dib(&mut reader, &mut dib)?;
            }
            if dib_type >= DIBType::V5 {
                decode_v5_dib(&mut reader, &mut dib)?;
            }
        }
    }

    if !is_valid_bitmask_combination(dib_type, &dib) {
        return None;
    }
    if dib.bpp == 16 || dib.bpp == 32 {
        if dib_type > DIBType::OSV2 && dib.bpp == 16 && dib.compression == compression::RGB {
            // RGB555.
            dib.masks.extend([0x7c00, 0x03e0, 0x001f]);
        } else if dib_type == DIBType::Info
            && (dib.compression == compression::BITFIELDS || dib.compression == compression::ALPHABITFIELDS)
        {
            // The masks follow the BITMAPINFOHEADER.
            let mask_count = if dib.compression == compression::ALPHABITFIELDS {
                4
            } else {
                3
            };
            for _ in 0..mask_count {
                dib.masks.push(reader.read_u32()?);
            }
        }
    }

    if included_in_ico {
        data_offset = dib_size;
        if dib.bpp <= 8 {
            let bytes_per_color = if dib_type == DIBType::Core { 3 } else { 4 };
            data_offset += palette_color_count(&dib) as usize * bytes_per_color;
        }
    }
    if data_offset >= data.len() {
        return None;
    }

    let mut icc_profile = BMPICCProfile::Absent;
    let mut icc_profile_offset = 0;
    let mut icc_profile_size = 0;
    // FIXME: Handle linked profiles, the sRGB color spaces, and calibrated RGB endpoints.
    if dib_type == DIBType::V5
        && dib.color_space == COLOR_SPACE_PROFILE_EMBEDDED
        && dib.profile_data != 0
        && dib.profile_size != 0
    {
        let offset = header_size as u64 + u64::from(dib.profile_data);
        if offset + u64::from(dib.profile_size) > data.len() as u64 {
            icc_profile = BMPICCProfile::OutOfBounds;
        } else {
            icc_profile = BMPICCProfile::Embedded;
            icc_profile_offset = offset as usize;
            icc_profile_size = dib.profile_size as usize;
        }
    }

    // INTEROP: An empty alpha mask means that the image has no alpha channel.
    if dib.masks.get(3) == Some(&0) {
        dib.masks.truncate(3);
    }
    let masks: Vec<ColorMask> = dib.masks.iter().map(|&mask| ColorMask::new(mask)).collect();

    // NB: A BMP included in an ICO is always given an alpha channel, since its AND mask makes pixels transparent. RLE
    //     images leave the pixels that they skip transparent.
    let is_rle = matches!(
        dib.compression,
        compression::RLE4 | compression::RLE8 | compression::RLE24
    );
    let format = if included_in_ico || is_rle {
        BMPBitmapFormat::BGRA8888
    } else {
        match dib.bpp {
            16 | 32 if masks.len() == 4 => BMPBitmapFormat::BGRA8888,
            24 => BMPBitmapFormat::RGBx8888,
            _ => BMPBitmapFormat::BGRx8888,
        }
    };

    let width = dib.width as u32;
    let absolute_height = dib.height.unsigned_abs();
    // The height of a BMP included in an ICO covers both the image and its AND mask.
    let height = if included_in_ico {
        absolute_height / 2
    } else {
        absolute_height
    };

    let decoder = BMPDecoder {
        included_in_ico,
        dib_type,
        dib_size,
        data_offset,
        width: width as usize,
        height: height as usize,
        top_down: dib.height < 0,
        bpp: dib.bpp,
        compression: dib.compression,
        number_of_palette_colors: dib.number_of_palette_colors,
        masks,
    };
    let info = BMPImageInfo {
        width,
        height,
        format,
        icc_profile,
        icc_profile_offset,
        icc_profile_size,
    };
    Some((decoder, info))
}

fn palette_color_count(dib: &DIBHeader) -> u32 {
    if dib.number_of_palette_colors > 0 {
        dib.number_of_palette_colors
    } else {
        1 << dib.bpp
    }
}

impl BMPDecoder {
    fn header_size(&self) -> usize {
        if self.included_in_ico { 0 } else { BMP_FILE_HEADER_SIZE }
    }

    fn decode_color_table(&self, data: &[u8]) -> Option<Vec<u32>> {
        if self.bpp > 8 {
            return Some(Vec::new());
        }

        let max_colors = 1usize << self.bpp;
        let header_size = self.header_size();
        let bytes_per_color = match self.dib_type {
            // OS/2 1.x uses 3 bytes per color.
            DIBType::Core => 3,
            // OS/2 2.x uses 3 or 4 bytes per color, which is only known from the size of the color table.
            DIBType::OSV2 | DIBType::OSV2Short if self.data_offset - header_size - self.dib_size == 3 * max_colors => 3,
            _ => 4,
        };

        let size_of_color_table = if self.included_in_ico {
            let count = if self.number_of_palette_colors > 0 {
                self.number_of_palette_colors as usize
            } else {
                max_colors
            };
            count * bytes_per_color
        } else {
            self.data_offset - header_size - self.dib_size
        };

        // NB: Partial color tables are against the spec, but are accepted since most viewers process them anyway.
        let start = header_size + self.dib_size;
        let mut reader = Reader::new(data.get(start..start.checked_add(size_of_color_table)?)?);
        let mut color_table = Vec::with_capacity(max_colors);
        while !reader.is_at_end() && color_table.len() < max_colors {
            let color = if bytes_per_color == 4 {
                reader.read_u32()?
            } else {
                reader.read_u24()?
            };
            color_table.push(color | 0xff00_0000);
        }
        Some(color_table)
    }

    // Returns the number of bits per pixel of the data that `decode_rle_data()` produces.
    fn rle_bits_per_pixel(&self) -> Option<u16> {
        match self.compression {
            // INTEROP: RLE8 and RLE4 data with a lower bit count has an equally small color table, but its pixel
            //          data is still RLE8 or RLE4.
            compression::RLE8 => Some(8),
            compression::RLE4 => Some(4),
            compression::RLE24 => Some(24),
            _ => None,
        }
    }

    // Returns the decoded pixel data, which of its pixels are set, and the offset in `data` where the RLE data ends.
    // The pixels that are not set were skipped and are transparent.
    fn decode_rle_data(&self, data: &[u8], bits_per_pixel: u16) -> Option<(Vec<u8>, Vec<bool>, usize)> {
        let rle_data = data.get(self.data_offset..)?;
        let mut reader = Reader::new(rle_data);
        let compression = self.compression;
        let width = self.width;
        let total_rows = self.height;
        let row_size = row_size(width, bits_per_pixel)?;
        // NB: Runs and deltas wrap at the 4-pixel boundary after the end of a row. Pixels past the end of the row are
        //     dropped.
        let total_columns = width.checked_next_multiple_of(4)?;

        let buffer_size = row_size.checked_mul(total_rows)?;
        if buffer_size > MAX_RLE_BUFFER_SIZE {
            return None;
        }
        let mut buffer = Vec::new();
        buffer.try_reserve_exact(buffer_size).ok()?;
        buffer.resize(buffer_size, 0);
        let mut pixel_is_set = Vec::new();
        let pixel_count = width.checked_mul(total_rows)?;
        pixel_is_set.try_reserve_exact(pixel_count).ok()?;
        pixel_is_set.resize(pixel_count, false);

        let mut column = 0usize;
        let mut row = 0usize;

        let mut set_pixel = |column: &mut usize, row: &mut usize, value: u32| -> Option<()> {
            if *column >= total_columns {
                *column = 0;
                *row += 1;
            }
            if *row >= total_rows {
                return None;
            }
            if *column < width {
                pixel_is_set[*row * width + *column] = true;
                let row_start = *row * row_size;
                match compression {
                    compression::RLE8 => buffer[row_start + *column] = value as u8,
                    compression::RLE4 => {
                        let byte = &mut buffer[row_start + *column / 2];
                        if (*column).is_multiple_of(2) {
                            *byte = (*byte & 0x0f) | ((value as u8) << 4);
                        } else {
                            *byte = (*byte & 0xf0) | (value as u8 & 0x0f);
                        }
                    }
                    _ => {
                        let index = row_start + *column * 3;
                        buffer[index..index + 3].copy_from_slice(&value.to_le_bytes()[..3]);
                    }
                }
            }
            *column += 1;
            Some(())
        };

        let read_value = |reader: &mut Reader| -> Option<u32> {
            if compression == compression::RLE24 {
                reader.read_u24()
            } else {
                reader.read_u8().map(u32::from)
            }
        };

        loop {
            let count = reader.read_u8()?;
            if count != 0 {
                // Encoded mode: repeat one value. In RLE4, the value holds two pixels that alternate.
                let value = read_value(&mut reader)?;
                for i in 0..count {
                    let pixel = if compression == compression::RLE4 {
                        if i % 2 == 0 { value >> 4 } else { value & 0x0f }
                    } else {
                        value
                    };
                    set_pixel(&mut column, &mut row, pixel)?;
                }
                continue;
            }

            match reader.read_u8()? {
                // End of line.
                0 => {
                    column = 0;
                    row += 1;
                }
                // End of bitmap.
                1 => break,
                // Delta.
                2 => {
                    let offset_x = reader.read_u8()?;
                    let offset_y = reader.read_u8()?;
                    column += usize::from(offset_x);
                    if column >= total_columns {
                        column -= total_columns;
                        row += 1;
                    }
                    row += usize::from(offset_y);
                }
                // Absolute mode: a run of literal values, padded to a 16-bit boundary.
                count => {
                    let count = usize::from(count);
                    if compression == compression::RLE4 {
                        let byte_count = count.div_ceil(2);
                        for i in 0..count {
                            if i % 2 == 0 {
                                let byte = reader.read_u8()?;
                                set_pixel(&mut column, &mut row, u32::from(byte >> 4))?;
                                if i + 1 < count {
                                    set_pixel(&mut column, &mut row, u32::from(byte & 0x0f))?;
                                }
                            }
                        }
                        if byte_count % 2 != 0 {
                            reader.read_u8()?;
                        }
                    } else {
                        for _ in 0..count {
                            let value = read_value(&mut reader)?;
                            set_pixel(&mut column, &mut row, value)?;
                        }
                        if count % 2 != 0 {
                            reader.read_u8()?;
                        }
                    }
                }
            }
        }

        let end_offset = self.data_offset + (rle_data.len() - reader.remaining());
        Some((buffer, pixel_is_set, end_offset))
    }

    fn masked_color(&self, data: u32) -> u32 {
        let red = u32::from(self.masks[0].scaled_value(data));
        let green = u32::from(self.masks[1].scaled_value(data));
        let blue = u32::from(self.masks[2].scaled_value(data));
        let alpha = match self.masks.get(3) {
            Some(mask) => u32::from(mask.scaled_value(data)),
            None => 0xff,
        };
        (alpha << 24) | (red << 16) | (green << 8) | blue
    }

    fn decode(&self, data: &[u8], output: &mut [u8], stride: usize) -> Option<()> {
        let color_table = self.decode_color_table(data)?;

        let rle_data;
        // NB: The AND mask of an RLE image follows its compressed data, not the decoded pixel data.
        let (pixel_data, bits_per_pixel, rle_pixel_is_set, rle_end_offset) = match self.rle_bits_per_pixel() {
            Some(bits_per_pixel) => {
                rle_data = self.decode_rle_data(data, bits_per_pixel)?;
                (
                    rle_data.0.as_slice(),
                    bits_per_pixel,
                    rle_data.1.as_slice(),
                    Some(rle_data.2),
                )
            }
            None => (data.get(self.data_offset..)?, self.bpp, &[][..], None),
        };
        let mut reader = Reader::new(pixel_data);
        let has_alpha_mask = self.masks.len() == 4;
        let mut has_alpha_values = false;

        let width = self.width;
        let height = self.height;
        let row_bytes = width * 4;
        let output_row = |row: usize| -> usize {
            if self.top_down {
                row * stride
            } else {
                (height - 1 - row) * stride
            }
        };
        // INTEROP: Indices past the end of the color table are opaque black.
        let palette_color = |index: u8| -> u32 { color_table.get(usize::from(index)).copied().unwrap_or(0xff00_0000) };

        let skip_row_padding = |reader: &mut Reader, consumed: usize| -> Option<()> {
            let remainder = consumed % 4;
            if remainder != 0 {
                reader.skip(4 - remainder)?;
            }
            Some(())
        };

        for row in 0..height {
            let row_start = output_row(row);
            let pixels = &mut output[row_start..row_start + row_bytes];
            let remaining_before_row = reader.remaining();
            let mut set_pixel = |column: usize, color: u32| {
                pixels[column * 4..column * 4 + 4].copy_from_slice(&color.to_ne_bytes());
            };

            let mut column = 0;
            while column < width {
                match bits_per_pixel {
                    1 | 2 | 4 => {
                        let byte = reader.read_u8()?;
                        let pixels_per_byte = 8 / usize::from(bits_per_pixel);
                        let index_mask = (1u8 << bits_per_pixel) - 1;
                        for i in 0..pixels_per_byte {
                            if column >= width {
                                break;
                            }
                            let shift = 8 - bits_per_pixel as usize * (i + 1);
                            set_pixel(column, palette_color((byte >> shift) & index_mask));
                            column += 1;
                        }
                    }
                    8 => {
                        let byte = reader.read_u8()?;
                        set_pixel(column, palette_color(byte));
                        column += 1;
                    }
                    16 => {
                        let color = self.masked_color(u32::from(reader.read_u16()?));
                        has_alpha_values |= has_alpha_mask && color >> 24 != 0;
                        set_pixel(column, color);
                        column += 1;
                    }
                    24 => {
                        let pixel = reader.read_u24()?;
                        if self.included_in_ico {
                            // 24-bit BMPs included in ICOs are already in BGRA8888 order.
                            set_pixel(column, pixel);
                        } else if self.compression == compression::RLE24 {
                            set_pixel(column, pixel | 0xff00_0000);
                        } else {
                            set_pixel(
                                column,
                                ((pixel & 0xff) << 16) | (pixel & 0xff00) | ((pixel >> 16) & 0xff),
                            );
                        }
                        column += 1;
                    }
                    32 => {
                        let value = reader.read_u32()?;
                        if self.masks.is_empty() && self.included_in_ico {
                            has_alpha_values |= value >> 24 != 0;
                            set_pixel(column, value);
                        } else if self.masks.is_empty() {
                            // INTEROP: Without an alpha mask, the fourth byte of a standalone BMP is not alpha.
                            set_pixel(column, value | 0xff00_0000);
                        } else {
                            let color = self.masked_color(value);
                            has_alpha_values |= has_alpha_mask && color >> 24 != 0;
                            set_pixel(column, color);
                        }
                        column += 1;
                    }
                    _ => return None,
                }
            }

            if !rle_pixel_is_set.is_empty() {
                for (column, &is_set) in rle_pixel_is_set[row * width..(row + 1) * width].iter().enumerate() {
                    if !is_set {
                        set_pixel(column, 0);
                    }
                }
            }

            let consumed = remaining_before_row - reader.remaining();

            skip_row_padding(&mut reader, consumed)?;
        }

        // INTEROP: The fourth byte of a 32-bit BMP in an ICO is alpha, unless it is zero for every pixel. Only an image
        //          without alpha values uses its AND mask.
        let may_have_alpha_values = self.included_in_ico && self.bpp == 32 && self.masks.is_empty();
        if may_have_alpha_values && !has_alpha_values {
            for row in 0..height {
                let row_start = row * stride;
                for pixel in output[row_start..row_start + row_bytes].as_chunks_mut::<4>().0 {
                    *pixel = (u32::from_ne_bytes(*pixel) | 0xff00_0000).to_ne_bytes();
                }
            }
        }

        // An ICO image is followed by an AND mask with one bit per pixel, which makes pixels transparent.
        // INTEROP: An image with alpha values ignores its AND mask.
        let mut mask_reader = match rle_end_offset {
            Some(offset) => Reader::new(data.get(offset..)?),
            None => reader,
        };
        if self.included_in_ico && !has_alpha_values && !mask_reader.is_at_end() {
            for row in 0..height {
                let row_start = output_row(row);
                let pixels = &mut output[row_start..row_start + row_bytes];
                let remaining_before_row = mask_reader.remaining();

                let mut column = 0;
                while column < width {
                    let byte = mask_reader.read_u8()?;
                    for bit in (0..8).rev() {
                        if column >= width {
                            break;
                        }
                        // Pixels skipped by RLE data stay transparent.
                        if !rle_pixel_is_set.is_empty() && !rle_pixel_is_set[row * width + column] {
                            column += 1;
                            continue;
                        }
                        let pixel_bytes = &mut pixels[column * 4..column * 4 + 4];
                        let mut pixel = u32::from_ne_bytes(pixel_bytes.try_into().ok()?);
                        if (byte >> bit) & 1 != 0 {
                            pixel &= 0x00ff_ffff;
                        } else if self.bpp < 32 {
                            pixel |= 0xff00_0000;
                        }
                        pixel_bytes.copy_from_slice(&pixel.to_ne_bytes());
                        column += 1;
                    }
                }

                let consumed = remaining_before_row - mask_reader.remaining();

                skip_row_padding(&mut mask_reader, consumed)?;
            }
        }

        Some(())
    }
}

fn row_size(width: usize, bits_per_pixel: u16) -> Option<usize> {
    width
        .checked_mul(usize::from(bits_per_pixel))?
        .div_ceil(8)
        .checked_next_multiple_of(4)
}

/// # Safety
/// `data` must be valid for `data_len` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bmp_rust_sniff(data: *const u8, data_len: usize) -> bool {
    catch_panic_or(false, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return false;
        };
        decode_file_header(data).is_some()
    })
}

/// # Safety
/// - `data` must be valid for `data_len` bytes.
/// - `out_info` must be a valid writable pointer.
/// - The returned pointer must be freed with `bmp_rust_decoder_free`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bmp_rust_decoder_new(
    data: *const u8,
    data_len: usize,
    included_in_ico: bool,
    out_info: *mut BMPImageInfo,
) -> *mut BMPDecoder {
    catch_panic_or(std::ptr::null_mut(), || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return std::ptr::null_mut();
        };
        let Some(out_info) = (unsafe { out_info.as_mut() }) else {
            return std::ptr::null_mut();
        };
        let Some((decoder, info)) = decode_dib(data, included_in_ico) else {
            return std::ptr::null_mut();
        };
        *out_info = info;
        Box::into_raw(Box::new(decoder))
    })
}

/// # Safety
/// `decoder` must be null or a pointer returned by `bmp_rust_decoder_new` that has not already been freed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bmp_rust_decoder_free(decoder: *mut BMPDecoder) {
    catch_panic_or((), || {
        if !decoder.is_null() {
            // SAFETY: The caller transfers ownership of a decoder allocated by `bmp_rust_decoder_new`.
            drop(unsafe { Box::from_raw(decoder) });
        }
    });
}

/// # Safety
/// - `decoder` must be a valid pointer returned by `bmp_rust_decoder_new`.
/// - `data` must be the same image passed to `bmp_rust_decoder_new` and be valid for `data_len` bytes.
/// - `buffer` must be valid for writes of `stride * height` bytes, where `height` is the height returned by
///   `bmp_rust_decoder_new`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bmp_rust_decode(
    decoder: *const BMPDecoder,
    data: *const u8,
    data_len: usize,
    buffer: *mut u8,
    stride: usize,
) -> bool {
    catch_panic_or(false, || {
        let Some(data) = (unsafe { bytes_from_raw(data, data_len) }) else {
            return false;
        };
        let Some(decoder) = (unsafe { decoder.as_ref() }) else {
            return false;
        };
        if stride < decoder.width.saturating_mul(4) {
            return false;
        }
        let Some(buffer_size) = stride.checked_mul(decoder.height) else {
            return false;
        };
        // NB: An empty bitmap may have no buffer at all.
        if buffer.is_null() && buffer_size != 0 {
            return false;
        }
        // SAFETY: The caller guarantees that `buffer` is valid for writes of `stride * height` bytes.
        let output = unsafe { ak::slice_from_raw_parts_mut(buffer, buffer_size) };
        decoder.decode(data, output, stride).is_some()
    })
}
