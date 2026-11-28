/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Decoding of WOFF 2.0 files into the sfnt fonts they package.
//!
//! https://www.w3.org/TR/WOFF2/

use std::collections::BTreeMap;
use std::ffi::c_void;
use std::num::NonZeroU16;

use libcompress_rust::{Decompressor, Format};

use crate::sfnt::{self, Reader};
use crate::woff::{SfntAllocator, allocate_sfnt, report};

const SIGNATURE: u32 = tag(b"wOF2");
const COLLECTION_FLAVOR: u32 = tag(b"ttcf");
const COLLECTION_VERSION_1: u32 = 0x0001_0000;
const COLLECTION_VERSION_2: u32 = 0x0002_0000;

/// We refuse to decode a WOFF2 file into more than this many bytes.
const MAX_OUTPUT_SIZE: usize = 30 * 1024 * 1024;

/// Fonts compress by a ratio of about 20 at most. A table directory that claims much more data than that is wrong.
const MAX_PLAUSIBLE_COMPRESSION_RATIO: f32 = 100.0;

const GLYF: u32 = tag(b"glyf");
const HEAD: u32 = tag(b"head");
const HHEA: u32 = tag(b"hhea");
const HMTX: u32 = tag(b"hmtx");
const LOCA: u32 = tag(b"loca");

/// The tags that a table directory entry names with an index instead of spelling them out.
///
/// https://www.w3.org/TR/WOFF2/#table_dir_format
const KNOWN_TAGS: [u32; 63] = [
    tag(b"cmap"),
    tag(b"head"),
    tag(b"hhea"),
    tag(b"hmtx"),
    tag(b"maxp"),
    tag(b"name"),
    tag(b"OS/2"),
    tag(b"post"),
    tag(b"cvt "),
    tag(b"fpgm"),
    tag(b"glyf"),
    tag(b"loca"),
    tag(b"prep"),
    tag(b"CFF "),
    tag(b"VORG"),
    tag(b"EBDT"),
    tag(b"EBLC"),
    tag(b"gasp"),
    tag(b"hdmx"),
    tag(b"kern"),
    tag(b"LTSH"),
    tag(b"PCLT"),
    tag(b"VDMX"),
    tag(b"vhea"),
    tag(b"vmtx"),
    tag(b"BASE"),
    tag(b"GDEF"),
    tag(b"GPOS"),
    tag(b"GSUB"),
    tag(b"EBSC"),
    tag(b"JSTF"),
    tag(b"MATH"),
    tag(b"CBDT"),
    tag(b"CBLC"),
    tag(b"COLR"),
    tag(b"CPAL"),
    tag(b"SVG "),
    tag(b"sbix"),
    tag(b"acnt"),
    tag(b"avar"),
    tag(b"bdat"),
    tag(b"bloc"),
    tag(b"bsln"),
    tag(b"cvar"),
    tag(b"fdsc"),
    tag(b"feat"),
    tag(b"fmtx"),
    tag(b"fvar"),
    tag(b"gvar"),
    tag(b"hsty"),
    tag(b"just"),
    tag(b"lcar"),
    tag(b"mort"),
    tag(b"morx"),
    tag(b"opbd"),
    tag(b"prop"),
    tag(b"trak"),
    tag(b"Zapf"),
    tag(b"Silf"),
    tag(b"Glat"),
    tag(b"Gloc"),
    tag(b"Feat"),
    tag(b"Sill"),
];

const INVALID_HEADER: &str = "Invalid WOFF2 header";
const INVALID_TABLE_DIRECTORY: &str = "Invalid WOFF2 table directory";
const INVALID_COLLECTION_DIRECTORY: &str = "Invalid WOFF2 collection directory";
const TABLE_DATA_TOO_BIG: &str = "Decompressed WOFF2 table data is too big";
const IMPLAUSIBLE_COMPRESSION_RATIO: &str = "Implausible WOFF2 compression ratio";
const INVALID_COMPRESSED_DATA: &str = "Invalid compressed WOFF2 data";
const INVALID_FONT_DATA: &str = "Invalid WOFF2 font data";
const INVALID_GLYF: &str = "Invalid transformed WOFF2 glyf table";
const INVALID_HMTX: &str = "Invalid transformed WOFF2 hmtx table";
const OUTPUT_TOO_BIG: &str = "Decoded WOFF2 font is too big";
const OUT_OF_MEMORY: &str = "Not enough memory to decode the WOFF2 font";

const fn tag(name: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*name)
}

fn round_up_to_4(value: u64) -> u64 {
    (value + 3) & !3
}

/// https://www.w3.org/TR/WOFF2/#DataTypes
fn read_uint_base_128(reader: &mut Reader) -> Result<u32, &'static str> {
    let mut accumulator: u32 = 0;
    for index in 0..5 {
        let byte = reader.u8()?;
        // No leading zeros.
        if index == 0 && byte == 0x80 {
            return Err(INVALID_TABLE_DIRECTORY);
        }
        // The value has to fit in 32 bits.
        if accumulator & 0xFE00_0000 != 0 {
            return Err(INVALID_TABLE_DIRECTORY);
        }
        accumulator = (accumulator << 7) | u32::from(byte & 0x7F);
        if byte & 0x80 == 0 {
            return Ok(accumulator);
        }
    }
    Err(INVALID_TABLE_DIRECTORY)
}

/// https://www.w3.org/TR/WOFF2/#DataTypes
fn read_255_uint16(reader: &mut Reader) -> Result<u16, &'static str> {
    const WORD_CODE: u8 = 253;
    const ONE_MORE_BYTE_CODE_2: u8 = 254;
    const ONE_MORE_BYTE_CODE_1: u8 = 255;
    const LOWEST_U_CODE: u16 = 253;

    Ok(match reader.u8()? {
        WORD_CODE => reader.u16()?,
        ONE_MORE_BYTE_CODE_1 => u16::from(reader.u8()?) + LOWEST_U_CODE,
        ONE_MORE_BYTE_CODE_2 => u16::from(reader.u8()?) + LOWEST_U_CODE * 2,
        code => u16::from(code),
    })
}

/// The sum of the 32-bit big-endian numbers in some data, which is taken as padded with zeros to a multiple of 4 bytes.
///
/// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#calculating-checksums
fn checksum(data: &[u8]) -> u32 {
    let (words, rest) = data.as_chunks::<4>();
    let mut sum = words
        .iter()
        .fold(0u32, |sum, word| sum.wrapping_add(u32::from_be_bytes(*word)));
    if !rest.is_empty() {
        let mut last = [0; 4];
        last[..rest.len()].copy_from_slice(rest);
        sum = sum.wrapping_add(u32::from_be_bytes(last));
    }
    sum
}

/// A table of the table directory.
struct Table {
    tag: u32,
    is_transformed: bool,
    /// Where the table is in the decompressed data, and its length there.
    src_offset: u32,
    src_length: u32,
    /// Where the table is in the decoded font, and its length there. Until the table is written, `dst_length` is the
    /// origLength of the table directory.
    dst_offset: u32,
    dst_length: u32,
}

/// A font of a collection: the indices of its tables in the table directory.
struct CollectionFont {
    flavor: u32,
    table_indices: Vec<u16>,
    /// The checksum of the table directory of the font in the decoded font.
    header_checksum: u32,
}

/// The header, table directory and collection directory of a WOFF2 file, after they have been checked.
struct Header {
    flavor: u32,
    /// The version of the collection header, for a collection of fonts.
    collection_version: Option<u32>,
    compressed_offset: usize,
    compressed_length: usize,
    /// The size of the data that the compressed data decompresses to, which is the sum of the lengths of the tables.
    uncompressed_size: usize,
    tables: Vec<Table>,
    fonts: Vec<CollectionFont>,
}

impl Header {
    /// https://www.w3.org/TR/WOFF2/#woff20Header
    fn read(woff2: &[u8]) -> Result<Self, &'static str> {
        let length = woff2.len();
        let mut reader = Reader::new(woff2, INVALID_HEADER);
        if reader.u32()? != SIGNATURE {
            return Err(INVALID_HEADER);
        }
        let flavor = reader.u32()?;
        if reader.u32()? as usize != length {
            return Err(INVALID_HEADER);
        }
        let num_tables = reader.u16()?;
        if num_tables == 0 {
            return Err(INVALID_HEADER);
        }
        // NB: The reserved field has no meaning for a decoder, and totalSfntSize is only for reference.
        reader.skip(6)?;
        let compressed_length = reader.u32()? as usize;
        // NB: The version of the file has no meaning for a decoder.
        reader.skip(4)?;
        let meta_offset = reader.u32()? as usize;
        let meta_length = reader.u32()? as usize;
        let _meta_orig_length = reader.u32()?;
        if meta_offset != 0 && (meta_offset >= length || length - meta_offset < meta_length) {
            return Err(INVALID_HEADER);
        }
        let priv_offset = reader.u32()? as usize;
        let priv_length = reader.u32()? as usize;
        if priv_offset != 0 && (priv_offset >= length || length - priv_offset < priv_length) {
            return Err(INVALID_HEADER);
        }

        // https://www.w3.org/TR/WOFF2/#table_dir_format
        let mut reader = Reader::new(reader.remaining(), INVALID_TABLE_DIRECTORY);
        let mut tables = Vec::with_capacity(usize::from(num_tables));
        let mut uncompressed_size: usize = 0;
        for _ in 0..num_tables {
            let flags = reader.u8()?;
            let tag = match KNOWN_TAGS.get(usize::from(flags & 0x3F)) {
                Some(&tag) => tag,
                None => reader.u32()?,
            };
            // NB: Transformation version 0 of glyf and loca is their transform, and the other versions of them are the
            //     null transform. For all other tables, version 0 is the null transform.
            let transform_version = flags >> 6;
            let is_transformed = if tag == GLYF || tag == LOCA {
                transform_version == 0
            } else {
                transform_version != 0
            };
            let orig_length = read_uint_base_128(&mut reader)?;
            let mut transform_length = orig_length;
            if is_transformed {
                transform_length = read_uint_base_128(&mut reader)?;
                if tag == LOCA && transform_length != 0 {
                    return Err(INVALID_TABLE_DIRECTORY);
                }
            }
            tables.push(Table {
                tag,
                is_transformed,
                src_offset: uncompressed_size as u32,
                src_length: transform_length,
                dst_offset: 0,
                dst_length: orig_length,
            });
            // NB: All of the table data is decompressed into one buffer, so a font with too much of it is refused
            //     before anything is allocated for it.
            uncompressed_size = uncompressed_size
                .checked_add(transform_length as usize)
                .filter(|&size| size <= MAX_OUTPUT_SIZE)
                .ok_or(TABLE_DATA_TOO_BIG)?;
        }

        // https://www.w3.org/TR/WOFF2/#collection_dir_format
        let mut collection_version = None;
        let mut fonts = Vec::new();
        if flavor == COLLECTION_FLAVOR {
            reader = Reader::new(reader.remaining(), INVALID_COLLECTION_DIRECTORY);
            let version = reader.u32()?;
            if version != COLLECTION_VERSION_1 && version != COLLECTION_VERSION_2 {
                return Err(INVALID_COLLECTION_DIRECTORY);
            }
            collection_version = Some(version);
            let num_fonts = read_255_uint16(&mut reader)?;
            if num_fonts == 0 {
                return Err(INVALID_COLLECTION_DIRECTORY);
            }
            for _ in 0..num_fonts {
                let num_tables = read_255_uint16(&mut reader)?;
                if num_tables == 0 {
                    return Err(INVALID_COLLECTION_DIRECTORY);
                }
                let flavor = reader.u32()?;
                let mut table_indices = Vec::new();
                let mut glyf_index = None;
                let mut loca_index = None;
                for _ in 0..num_tables {
                    let index = read_255_uint16(&mut reader)?;
                    let Some(table) = tables.get(usize::from(index)) else {
                        return Err(INVALID_COLLECTION_DIRECTORY);
                    };
                    if table.tag == GLYF {
                        glyf_index = Some(index);
                    } else if table.tag == LOCA {
                        loca_index = Some(index);
                    }
                    table_indices.push(index);
                }
                // NB: A font that has only one of glyf and loca is refused once its tables are decoded.
                if let (Some(glyf_index), Some(loca_index)) = (glyf_index, loca_index)
                    && glyf_index.checked_add(1) != Some(loca_index)
                {
                    return Err(INVALID_COLLECTION_DIRECTORY);
                }
                fonts.push(CollectionFont {
                    flavor,
                    table_indices,
                    header_checksum: 0,
                });
            }
        }

        // The data blocks follow each other with only the padding to 4 bytes between them, up to the end of the file.
        let compressed_offset = length - reader.remaining().len();
        if compressed_offset > u32::MAX as usize {
            return Err(INVALID_HEADER);
        }
        let mut block_end = round_up_to_4(compressed_offset as u64 + compressed_length as u64);
        if block_end > length as u64 {
            return Err(INVALID_HEADER);
        }
        for (block_offset, block_length) in [(meta_offset, meta_length), (priv_offset, priv_length)] {
            if block_offset == 0 {
                continue;
            }
            if block_end != block_offset as u64 {
                return Err(INVALID_HEADER);
            }
            block_end = round_up_to_4(block_offset as u64 + block_length as u64);
            if block_end > u64::from(u32::MAX) {
                return Err(INVALID_HEADER);
            }
        }
        if block_end != round_up_to_4(length as u64) {
            return Err(INVALID_HEADER);
        }

        Ok(Self {
            flavor,
            collection_version,
            compressed_offset,
            compressed_length,
            uncompressed_size,
            tables,
            fonts,
        })
    }

    /// The indices of the tables of a font in the table directory, in the order the font is decoded in.
    fn font_table_indices(&self, font_index: usize) -> Vec<usize> {
        match self.fonts.get(font_index) {
            Some(font) => font.table_indices.iter().copied().map(usize::from).collect(),
            None => (0..self.tables.len()).collect(),
        }
    }
}

/// The decoded font as it is written, which refuses to grow past `MAX_OUTPUT_SIZE`.
#[derive(Default)]
struct Output {
    bytes: Vec<u8>,
}

impl Output {
    fn len(&self) -> usize {
        self.bytes.len()
    }

    fn reserve(&mut self, additional: usize) -> Result<(), &'static str> {
        if additional > MAX_OUTPUT_SIZE - self.bytes.len() {
            return Err(OUTPUT_TOO_BIG);
        }
        self.bytes.try_reserve(additional).map_err(|_| OUT_OF_MEMORY)
    }

    fn write(&mut self, data: &[u8]) -> Result<(), &'static str> {
        self.reserve(data.len())?;
        self.bytes.extend_from_slice(data);
        Ok(())
    }

    fn write_zeros(&mut self, count: usize) -> Result<(), &'static str> {
        self.reserve(count)?;
        self.bytes.resize(self.bytes.len() + count, 0);
        Ok(())
    }

    /// Pads the font with zeros to a multiple of 4 bytes, where the next table starts.
    fn pad_to_4(&mut self) -> Result<(), &'static str> {
        self.write_zeros(self.bytes.len().next_multiple_of(4) - self.bytes.len())
    }

    /// Writes over bytes that are written already.
    fn write_at(&mut self, offset: usize, data: &[u8]) -> Result<(), &'static str> {
        self.bytes
            .get_mut(offset..offset + data.len())
            .ok_or(INVALID_FONT_DATA)?
            .copy_from_slice(data);
        Ok(())
    }
}

/// What reconstructing a font learns that its later tables need.
#[derive(Default)]
struct FontInfo {
    num_glyphs: u16,
    num_hmetrics: u16,
    x_mins: Vec<i16>,
    /// Where the table record of each table is in the decoded font.
    record_offsets: BTreeMap<u32, usize>,
}

/// Writes the collection header and the table directories, with records that are filled in as the tables are written.
/// Returns the checksum of what it wrote and what the reconstruction of each font needs.
fn write_headers(header: &mut Header, output: &mut Output) -> Result<(u32, Vec<FontInfo>), &'static str> {
    let records: u64 = match header.collection_version {
        Some(_) => header.fonts.iter().map(|font| font.table_indices.len() as u64).sum(),
        None => header.tables.len() as u64,
    };
    let num_fonts = header.fonts.len() as u64;
    let collection_header_size = match header.collection_version {
        Some(COLLECTION_VERSION_2) => 24 + 4 * num_fonts,
        Some(_) => 12 + 4 * num_fonts,
        None => 0,
    };
    let headers_size = collection_header_size
        + num_fonts.max(1) * sfnt::TABLE_DIRECTORY_HEADER_SIZE as u64
        + records * sfnt::TABLE_RECORD_SIZE as u64;
    if headers_size > MAX_OUTPUT_SIZE as u64 {
        return Err(OUTPUT_TOO_BIG);
    }
    output.reserve(headers_size as usize)?;

    let tables = &header.tables;
    let mut infos = Vec::new();
    match header.collection_version {
        Some(version) => {
            // The table records of each font are in the order of their tags, as in an sfnt.
            // NB: The first records take the tables in that order, one for each tag. If a font names a tag more than
            //     once, its other records keep the tables they had.
            for font in &mut header.fonts {
                let sorted_index_by_tag: BTreeMap<u32, u16> = font
                    .table_indices
                    .iter()
                    .map(|&index| (tables[usize::from(index)].tag, index))
                    .collect();
                for (slot, index) in font.table_indices.iter_mut().zip(sorted_index_by_tag.into_values()) {
                    *slot = index;
                }
            }

            // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
            output.write(&header.flavor.to_be_bytes())?;
            output.write(&version.to_be_bytes())?;
            output.write(&(header.fonts.len() as u32).to_be_bytes())?;
            let offset_table_start = output.len();
            output.write_zeros(4 * header.fonts.len())?;
            if version == COLLECTION_VERSION_2 {
                // NB: The DSIG fields stay zero, as the font has no signature.
                output.write_zeros(12)?;
            }

            for (font_index, font) in header.fonts.iter_mut().enumerate() {
                let directory_offset = output.len();
                output.write_at(
                    offset_table_start + 4 * font_index,
                    &(directory_offset as u32).to_be_bytes(),
                )?;
                let num_tables =
                    NonZeroU16::new(font.table_indices.len() as u16).ok_or(INVALID_COLLECTION_DIRECTORY)?;
                output.write(&sfnt::table_directory_header(font.flavor, num_tables))?;
                let mut info = FontInfo::default();
                for &index in &font.table_indices {
                    let tag = tables[usize::from(index)].tag;
                    info.record_offsets.insert(tag, output.len());
                    output.write(&sfnt::table_record(tag, 0, 0, 0))?;
                }
                font.header_checksum = checksum(&output.bytes[directory_offset..]);
                infos.push(info);
            }
        }
        None => {
            let mut tags: Vec<u32> = tables.iter().map(|table| table.tag).collect();
            tags.sort_unstable();
            let num_tables = NonZeroU16::new(tags.len() as u16).ok_or(INVALID_HEADER)?;
            output.write(&sfnt::table_directory_header(header.flavor, num_tables))?;
            let mut info = FontInfo::default();
            for tag in tags {
                info.record_offsets.insert(tag, output.len());
                output.write(&sfnt::table_record(tag, 0, 0, 0))?;
            }
            infos.push(info);
        }
    }

    Ok((checksum(&output.bytes), infos))
}

/// Decompresses the table data, which has to decompress to exactly the sum of the lengths of the tables.
fn decompress(woff2: &[u8], header: &Header) -> Result<Vec<u8>, &'static str> {
    let compressed = woff2
        .get(header.compressed_offset..header.compressed_offset + header.compressed_length)
        .ok_or(INVALID_HEADER)?;
    let mut data = Vec::new();
    data.try_reserve_exact(header.uncompressed_size)
        .map_err(|_| OUT_OF_MEMORY)?;
    data.resize(header.uncompressed_size, 0);

    let mut decompressor = Decompressor::new(Format::Brotli);
    let decompressed = decompressor
        .decompress_into(compressed, &mut data)
        .map_err(|_| INVALID_COMPRESSED_DATA)?;
    // NB: The padding after the compressed data is not part of it, so the decoder leaves it unconsumed.
    if !decompressor.has_reached_end() || decompressed.output_length != data.len() {
        return Err(INVALID_COMPRESSED_DATA);
    }
    Ok(data)
}

/// Writes the tables of a font, and fills in their records in its table directory.
fn reconstruct_font(
    data: &mut [u8],
    header: &mut Header,
    font_index: usize,
    info: &mut FontInfo,
    headers_checksum: u32,
    table_checksums: &mut BTreeMap<(u32, u32), u32>,
    output: &mut Output,
) -> Result<(), &'static str> {
    let table_indices = header.font_table_indices(font_index);
    let find_table = |tables: &[Table], tag: u32| table_indices.iter().copied().find(|&index| tables[index].tag == tag);

    let glyf_index = find_table(&header.tables, GLYF);
    let loca_index = find_table(&header.tables, LOCA);
    match (glyf_index, loca_index) {
        (Some(glyf_index), Some(loca_index)) => {
            // NB: The glyf table makes the loca table, so either both are transformed or neither is.
            if header.tables[glyf_index].is_transformed != header.tables[loca_index].is_transformed {
                return Err(INVALID_FONT_DATA);
            }
        }
        (None, None) => {}
        _ => return Err(INVALID_FONT_DATA),
    }

    let mut font_checksum = match header.fonts.get(font_index) {
        Some(font) => font.header_checksum,
        None => headers_checksum,
    };
    let mut loca_checksum = 0;
    let mut dst_offset = output.len();
    for &index in &table_indices {
        let table = &header.tables[index];
        let (tag, is_transformed) = (table.tag, table.is_transformed);
        let key = (tag, table.src_offset);
        // NB: Only a later font of a collection can share a table that an earlier font wrote.
        let reused_checksum = table_checksums.get(&key).copied();
        if font_index == 0 && reused_checksum.is_some() {
            return Err(INVALID_FONT_DATA);
        }
        let src_start = table.src_offset as usize;
        let src_end = src_start + table.src_length as usize;
        if src_end > data.len() {
            return Err(INVALID_FONT_DATA);
        }

        if tag == HHEA {
            // https://learn.microsoft.com/en-us/typography/opentype/spec/hhea
            let num_hmetrics = data.get(src_start..src_end).and_then(|hhea| hhea.get(34..36));
            let Some(&[high, low]) = num_hmetrics else {
                return Err(INVALID_FONT_DATA);
            };
            info.num_hmetrics = u16::from_be_bytes([high, low]);
        }

        let table_checksum = match reused_checksum {
            Some(table_checksum) => table_checksum,
            None => {
                let table_checksum = if !is_transformed {
                    if tag == HEAD {
                        // NB: The checkSumAdjustment field is zero while the checksum of the font is computed.
                        if src_end - src_start < 12 {
                            return Err(INVALID_FONT_DATA);
                        }
                        data[src_start + 8..src_start + 12].fill(0);
                    }
                    header.tables[index].dst_offset = dst_offset as u32;
                    let table_data = &data[src_start..src_end];
                    output.write(table_data)?;
                    checksum(table_data)
                } else if tag == GLYF {
                    header.tables[index].dst_offset = dst_offset as u32;
                    let loca_index = loca_index.ok_or(INVALID_FONT_DATA)?;
                    let (glyf_checksum, new_loca_checksum) = reconstruct_glyf(
                        &data[src_start..src_end],
                        &mut header.tables,
                        index,
                        loca_index,
                        info,
                        output,
                    )?;
                    loca_checksum = new_loca_checksum;
                    glyf_checksum
                } else if tag == LOCA {
                    // NB: Reconstructing the glyf table wrote the loca table.
                    loca_checksum
                } else if tag == HMTX {
                    header.tables[index].dst_offset = dst_offset as u32;
                    reconstruct_hmtx(&data[src_start..src_end], info, output)?
                } else {
                    // An unknown transform.
                    return Err(INVALID_FONT_DATA);
                };
                table_checksums.insert(key, table_checksum);
                table_checksum
            }
        };
        font_checksum = font_checksum.wrapping_add(table_checksum);

        let table = &header.tables[index];
        let mut entry = [0; 12];
        entry[0..4].copy_from_slice(&table_checksum.to_be_bytes());
        entry[4..8].copy_from_slice(&table.dst_offset.to_be_bytes());
        entry[8..12].copy_from_slice(&table.dst_length.to_be_bytes());
        let record_offset = info.record_offsets.get(&tag).copied().ok_or(INVALID_FONT_DATA)?;
        output.write_at(record_offset + 4, &entry)?;
        // NB: The record was all zeros when the checksum of the table directory was computed.
        font_checksum = font_checksum.wrapping_add(checksum(&entry));

        output.pad_to_4()?;
        if u64::from(table.dst_offset) + u64::from(table.dst_length) > output.len() as u64 {
            return Err(INVALID_FONT_DATA);
        }
        dst_offset = output.len();
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#calculating-checksums
    if let Some(head_index) = find_table(&header.tables, HEAD) {
        let head = &header.tables[head_index];
        if head.dst_length < 12 {
            return Err(INVALID_FONT_DATA);
        }
        let checksum_adjustment = 0xB1B0_AFBAu32.wrapping_sub(font_checksum);
        output.write_at(head.dst_offset as usize + 8, &checksum_adjustment.to_be_bytes())?;
    }
    Ok(())
}

// Flags of the points of a simple glyph.
// https://learn.microsoft.com/en-us/typography/opentype/spec/glyf#simple-glyph-description
const ON_CURVE_POINT: u8 = 1 << 0;
const X_SHORT_VECTOR: u8 = 1 << 1;
const Y_SHORT_VECTOR: u8 = 1 << 2;
const REPEAT_FLAG: u8 = 1 << 3;
const X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR: u8 = 1 << 4;
const Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR: u8 = 1 << 5;
const OVERLAP_SIMPLE: u8 = 1 << 6;

// Flags of the components of a composite glyph.
// https://learn.microsoft.com/en-us/typography/opentype/spec/glyf#composite-glyph-description
const ARG_1_AND_2_ARE_WORDS: u16 = 1 << 0;
const WE_HAVE_A_SCALE: u16 = 1 << 3;
const MORE_COMPONENTS: u16 = 1 << 5;
const WE_HAVE_AN_X_AND_Y_SCALE: u16 = 1 << 6;
const WE_HAVE_A_TWO_BY_TWO: u16 = 1 << 7;
const WE_HAVE_INSTRUCTIONS: u16 = 1 << 8;

/// The optionFlags bit that says the transformed glyf table has an overlapSimpleBitmap.
const OVERLAP_SIMPLE_BITMAP: u16 = 1 << 0;

/// A point of a glyph outline. The coordinates wrap like those of the reference decoder.
#[derive(Clone, Copy)]
struct Point {
    x: i32,
    y: i32,
    is_on_curve: bool,
}

/// Rebuilds the glyf and loca tables from a transformed glyf table. Returns the checksums of the two tables.
///
/// https://www.w3.org/TR/WOFF2/#glyf_table_format
fn reconstruct_glyf(
    data: &[u8],
    tables: &mut [Table],
    glyf_index: usize,
    loca_index: usize,
    info: &mut FontInfo,
    output: &mut Output,
) -> Result<(u32, u32), &'static str> {
    const NUM_SUBSTREAMS: usize = 7;

    let glyf_start = output.len();
    let mut reader = Reader::new(data, INVALID_GLYF);
    let _reserved = reader.u16()?;
    let option_flags = reader.u16()?;
    let num_glyphs = reader.u16()?;
    let index_format = reader.u16()?;
    info.num_glyphs = num_glyphs;

    // The origLength of the loca table has to be the length of the loca table this makes.
    let loca_offset_size: u32 = if index_format != 0 { 4 } else { 2 };
    if tables[loca_index].dst_length != loca_offset_size * (u32::from(num_glyphs) + 1) {
        return Err(INVALID_GLYF);
    }

    let mut substreams = [&[][..]; NUM_SUBSTREAMS];
    let mut stream_data = Reader::new(data.get((2 + NUM_SUBSTREAMS) * 4..).ok_or(INVALID_GLYF)?, INVALID_GLYF);
    for substream in &mut substreams {
        *substream = stream_data.bytes(reader.u32()? as usize)?;
    }
    let [
        n_contour_stream,
        n_points_stream,
        flag_stream,
        glyph_stream,
        composite_stream,
        bbox_stream,
        instruction_stream,
    ] = substreams;
    let mut n_contour_stream = Reader::new(n_contour_stream, INVALID_GLYF);
    let mut n_points_stream = Reader::new(n_points_stream, INVALID_GLYF);
    let mut flag_stream = Reader::new(flag_stream, INVALID_GLYF);
    let mut glyph_stream = Reader::new(glyph_stream, INVALID_GLYF);
    let mut composite_stream = Reader::new(composite_stream, INVALID_GLYF);
    let mut instruction_stream = Reader::new(instruction_stream, INVALID_GLYF);

    let bbox_bitmap_length = usize::from(num_glyphs).div_ceil(32) * 4;
    let (bbox_bitmap, bbox_stream) = bbox_stream.split_at_checked(bbox_bitmap_length).ok_or(INVALID_GLYF)?;
    let mut bbox_stream = Reader::new(bbox_stream, INVALID_GLYF);
    let overlap_simple_bitmap = if option_flags & OVERLAP_SIMPLE_BITMAP != 0 {
        Some(stream_data.bytes(usize::from(num_glyphs).div_ceil(8))?)
    } else {
        None
    };
    let has_bit = |bitmap: &[u8], glyph: usize| {
        bitmap
            .get(glyph / 8)
            .is_some_and(|byte| byte & (0x80 >> (glyph % 8)) != 0)
    };

    let mut loca_values = Vec::with_capacity(usize::from(num_glyphs) + 1);
    info.x_mins.resize(usize::from(num_glyphs), 0);
    let mut glyf_checksum = 0u32;
    let mut glyph = Vec::new();
    let mut points = Vec::new();
    let mut contour_point_counts = Vec::new();
    for glyph_index in 0..usize::from(num_glyphs) {
        glyph.clear();
        let has_bbox = has_bit(bbox_bitmap, glyph_index);
        let n_contours = n_contour_stream.u16()?;

        if n_contours == 0xFFFF {
            // A composite glyph, which has to have its bounding box in the bbox stream.
            if !has_bbox {
                return Err(INVALID_GLYF);
            }
            let (composite_size, have_instructions) = size_of_composite(composite_stream.remaining())?;
            let instruction_size = if have_instructions {
                read_255_uint16(&mut glyph_stream)?
            } else {
                0
            };
            glyph.extend_from_slice(&n_contours.to_be_bytes());
            glyph.extend_from_slice(bbox_stream.bytes(8)?);
            glyph.extend_from_slice(composite_stream.bytes(composite_size)?);
            if have_instructions {
                glyph.extend_from_slice(&instruction_size.to_be_bytes());
                glyph.extend_from_slice(instruction_stream.bytes(usize::from(instruction_size))?);
            }
        } else if n_contours > 0 {
            // A simple glyph.
            contour_point_counts.clear();
            let mut total_points = 0usize;
            for _ in 0..n_contours {
                let point_count = read_255_uint16(&mut n_points_stream)?;
                contour_point_counts.push(point_count);
                total_points += usize::from(point_count);
            }
            let flags = flag_stream.bytes(total_points)?;
            let triplet_length = decode_triplets(flags, glyph_stream.remaining(), &mut points)?;
            glyph_stream.skip(triplet_length)?;
            let instruction_size = read_255_uint16(&mut glyph_stream)?;

            glyph.extend_from_slice(&n_contours.to_be_bytes());
            if has_bbox {
                glyph.extend_from_slice(bbox_stream.bytes(8)?);
            } else {
                glyph.extend_from_slice(&bounding_box(&points));
            }
            let mut end_point: i32 = -1;
            for &point_count in &contour_point_counts {
                end_point += i32::from(point_count);
                if end_point >= 65536 {
                    return Err(INVALID_GLYF);
                }
                glyph.extend_from_slice(&(end_point as u16).to_be_bytes());
            }
            glyph.extend_from_slice(&instruction_size.to_be_bytes());
            glyph.extend_from_slice(instruction_stream.bytes(usize::from(instruction_size))?);
            let overlaps = overlap_simple_bitmap.is_some_and(|bitmap| has_bit(bitmap, glyph_index));
            store_points(&points, overlaps, &mut glyph);
        } else if has_bbox {
            // An empty glyph, which must not have a bounding box.
            return Err(INVALID_GLYF);
        }

        loca_values.push((output.len() - glyf_start) as u32);
        output.write(&glyph)?;
        output.pad_to_4()?;
        glyf_checksum = glyf_checksum.wrapping_add(checksum(&glyph));

        // NB: Reconstructing the hmtx table can need the xMin of each glyph that has a bounding box.
        if n_contours > 0
            && let Some(&[high, low]) = glyph.get(2..4)
        {
            info.x_mins[glyph_index] = i16::from_be_bytes([high, low]);
        }
    }

    let glyf_length = (output.len() - glyf_start) as u32;
    tables[glyf_index].dst_length = glyf_length;
    loca_values.push(glyf_length);

    // https://learn.microsoft.com/en-us/typography/opentype/spec/loca
    let mut loca = Vec::with_capacity(loca_values.len() * loca_offset_size as usize);
    for value in loca_values {
        if index_format != 0 {
            loca.extend_from_slice(&value.to_be_bytes());
        } else {
            loca.extend_from_slice(&((value >> 1) as u16).to_be_bytes());
        }
    }
    let loca_offset = output.len();
    output.write(&loca)?;
    tables[loca_index].dst_offset = loca_offset as u32;
    tables[loca_index].dst_length = loca.len() as u32;

    Ok((glyf_checksum, checksum(&loca)))
}

/// The size of the components of a composite glyph at the start of the composite stream, and whether the glyph has
/// instructions.
fn size_of_composite(composite_stream: &[u8]) -> Result<(usize, bool), &'static str> {
    let mut reader = Reader::new(composite_stream, INVALID_GLYF);
    let mut have_instructions = false;
    let mut flags = MORE_COMPONENTS;
    while flags & MORE_COMPONENTS != 0 {
        flags = reader.u16()?;
        have_instructions |= flags & WE_HAVE_INSTRUCTIONS != 0;
        // The glyph index, then the arguments.
        let mut argument_size = 2;
        argument_size += if flags & ARG_1_AND_2_ARE_WORDS != 0 { 4 } else { 2 };
        if flags & WE_HAVE_A_SCALE != 0 {
            argument_size += 2;
        } else if flags & WE_HAVE_AN_X_AND_Y_SCALE != 0 {
            argument_size += 4;
        } else if flags & WE_HAVE_A_TWO_BY_TWO != 0 {
            argument_size += 8;
        }
        reader.skip(argument_size)?;
    }
    Ok((composite_stream.len() - reader.remaining().len(), have_instructions))
}

/// Decodes the points of a simple glyph, one for each flag, from the glyph stream. Returns how many bytes of the glyph
/// stream the points take.
///
/// https://www.w3.org/TR/WOFF2/#triplet_decoding
fn decode_triplets(flags: &[u8], glyph_stream: &[u8], points: &mut Vec<Point>) -> Result<usize, &'static str> {
    fn with_sign(flag: i32, value: i32) -> i32 {
        if flag & 1 != 0 { value } else { -value }
    }

    // NB: Each point takes one byte of the glyph stream at least.
    if flags.len() > glyph_stream.len() {
        return Err(INVALID_GLYF);
    }
    points.clear();
    points.try_reserve(flags.len()).map_err(|_| OUT_OF_MEMORY)?;
    let mut x: i32 = 0;
    let mut y: i32 = 0;
    let mut offset = 0;
    for &flag in flags {
        let is_on_curve = flag >> 7 == 0;
        let flag = i32::from(flag & 0x7F);
        let data_size = match flag {
            0..84 => 1,
            84..120 => 2,
            120..124 => 3,
            _ => 4,
        };
        let Some(data) = glyph_stream.get(offset..offset + data_size) else {
            return Err(INVALID_GLYF);
        };
        let byte = |index: usize| data.get(index).copied().map_or(0, i32::from);
        let (b0, b1, b2, b3) = (byte(0), byte(1), byte(2), byte(3));
        let (dx, dy) = match flag {
            0..10 => (0, with_sign(flag, ((flag & 14) << 7) + b0)),
            10..20 => (with_sign(flag, (((flag - 10) & 14) << 7) + b0), 0),
            20..84 => {
                let high = flag - 20;
                (
                    with_sign(flag, 1 + (high & 0x30) + (b0 >> 4)),
                    with_sign(flag >> 1, 1 + ((high & 0x0C) << 2) + (b0 & 0x0F)),
                )
            }
            84..120 => {
                let high = flag - 84;
                (
                    with_sign(flag, 1 + ((high / 12) << 8) + b0),
                    with_sign(flag >> 1, 1 + (((high % 12) >> 2) << 8) + b1),
                )
            }
            120..124 => (
                with_sign(flag, (b0 << 4) + (b1 >> 4)),
                with_sign(flag >> 1, ((b1 & 0x0F) << 8) + b2),
            ),
            _ => (with_sign(flag, (b0 << 8) + b1), with_sign(flag >> 1, (b2 << 8) + b3)),
        };
        offset += data_size;
        x = x.wrapping_add(dx);
        y = y.wrapping_add(dy);
        points.push(Point { x, y, is_on_curve });
    }
    Ok(offset)
}

/// The bounding box of a simple glyph, as the xMin, yMin, xMax and yMax fields of its header.
fn bounding_box(points: &[Point]) -> [u8; 8] {
    let (mut x_min, mut y_min, mut x_max, mut y_max) = (0, 0, 0, 0);
    if let Some(first) = points.first() {
        (x_min, y_min, x_max, y_max) = (first.x, first.y, first.x, first.y);
    }
    for point in points {
        x_min = x_min.min(point.x);
        y_min = y_min.min(point.y);
        x_max = x_max.max(point.x);
        y_max = y_max.max(point.y);
    }
    let mut bounding_box = [0; 8];
    for (field, value) in bounding_box
        .as_chunks_mut::<2>()
        .0
        .iter_mut()
        .zip([x_min, y_min, x_max, y_max])
    {
        *field = (value as u16).to_be_bytes();
    }
    bounding_box
}

/// Writes the flags and coordinates of the points of a simple glyph.
fn store_points(points: &[Point], overlaps: bool, glyph: &mut Vec<u8>) {
    let mut last_flag = None;
    let mut repeat_count: u8 = 0;
    let mut last = Point {
        x: 0,
        y: 0,
        is_on_curve: false,
    };
    for (index, point) in points.iter().enumerate() {
        let mut flag = if point.is_on_curve { ON_CURVE_POINT } else { 0 };
        if overlaps && index == 0 {
            flag |= OVERLAP_SIMPLE;
        }
        let dx = point.x.wrapping_sub(last.x);
        let dy = point.y.wrapping_sub(last.y);
        if dx == 0 {
            flag |= X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR;
        } else if dx > -256 && dx < 256 {
            flag |= X_SHORT_VECTOR
                | if dx > 0 {
                    X_IS_SAME_OR_POSITIVE_X_SHORT_VECTOR
                } else {
                    0
                };
        }
        if dy == 0 {
            flag |= Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR;
        } else if dy > -256 && dy < 256 {
            flag |= Y_SHORT_VECTOR
                | if dy > 0 {
                    Y_IS_SAME_OR_POSITIVE_Y_SHORT_VECTOR
                } else {
                    0
                };
        }

        if last_flag == Some(flag) && repeat_count != u8::MAX {
            // NB: The flag of a run is the last byte written until the run ends with its repeat count.
            if let Some(run_flag) = glyph.last_mut() {
                *run_flag |= REPEAT_FLAG;
            }
            repeat_count += 1;
        } else {
            if repeat_count != 0 {
                glyph.push(repeat_count);
            }
            glyph.push(flag);
            repeat_count = 0;
        }
        last = *point;
        last_flag = Some(flag);
    }
    if repeat_count != 0 {
        glyph.push(repeat_count);
    }

    store_coordinates(points.iter().map(|point| point.x), glyph);
    store_coordinates(points.iter().map(|point| point.y), glyph);
}

/// Writes the coordinates of the points of a simple glyph, each as its difference from the one before.
fn store_coordinates(coordinates: impl Iterator<Item = i32>, glyph: &mut Vec<u8>) {
    let mut last = 0i32;
    for coordinate in coordinates {
        let delta = coordinate.wrapping_sub(last);
        if delta != 0 {
            if delta > -256 && delta < 256 {
                glyph.push(delta.unsigned_abs() as u8);
            } else {
                glyph.extend_from_slice(&(delta as u16).to_be_bytes());
            }
        }
        last = coordinate;
    }
}

/// Rebuilds the hmtx table from a transformed hmtx table, with the xMin values of the glyphs for the left side
/// bearings it leaves out. Returns the checksum of the table.
///
/// https://www.w3.org/TR/WOFF2/#hmtx_table_format
fn reconstruct_hmtx(data: &[u8], info: &FontInfo, output: &mut Output) -> Result<u32, &'static str> {
    let mut reader = Reader::new(data, INVALID_HMTX);
    let flags = reader.u8()?;
    let has_proportional_lsbs = flags & 1 == 0;
    let has_monospace_lsbs = flags & 2 == 0;
    // Bits 2-7 are reserved and must be zero, and the table has to leave out at least one of the arrays.
    if flags & 0xFC != 0 || (has_proportional_lsbs && has_monospace_lsbs) {
        return Err(INVALID_HMTX);
    }
    // The hmtx table has one horizontal metric at least, and no more than one for each glyph.
    let num_glyphs = usize::from(info.num_glyphs);
    let num_hmetrics = usize::from(info.num_hmetrics);
    if num_hmetrics > num_glyphs || num_hmetrics < 1 {
        return Err(INVALID_HMTX);
    }

    let mut advance_widths = Vec::with_capacity(num_hmetrics);
    for _ in 0..num_hmetrics {
        advance_widths.push(reader.u16()?);
    }
    let mut left_side_bearings = Vec::with_capacity(num_glyphs);
    for glyph in 0..num_glyphs {
        let is_stored = if glyph < num_hmetrics {
            has_proportional_lsbs
        } else {
            has_monospace_lsbs
        };
        let left_side_bearing = if is_stored {
            reader.u16()?
        } else {
            *info.x_mins.get(glyph).ok_or(INVALID_HMTX)? as u16
        };
        left_side_bearings.push(left_side_bearing);
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/hmtx
    let mut hmtx = Vec::with_capacity(2 * (num_glyphs + num_hmetrics));
    for (glyph, left_side_bearing) in left_side_bearings.into_iter().enumerate() {
        if let Some(advance_width) = advance_widths.get(glyph) {
            hmtx.extend_from_slice(&advance_width.to_be_bytes());
        }
        hmtx.extend_from_slice(&left_side_bearing.to_be_bytes());
    }
    output.write(&hmtx)?;
    Ok(checksum(&hmtx))
}

/// Decodes a WOFF2 file into the sfnt it packages. The sfnt goes into the buffer that `allocate` gives for its size,
/// which is all written once decoding succeeds.
pub fn decode<'a>(woff2: &[u8], allocate: impl FnOnce(usize) -> Option<&'a mut [u8]>) -> Result<(), &'static str> {
    let mut header = Header::read(woff2)?;
    let mut output = Output::default();
    let (headers_checksum, mut infos) = write_headers(&mut header, &mut output)?;

    // NB: The ratio is computed in single precision, as the reference decoder does.
    if header.uncompressed_size as f32 / woff2.len() as f32 > MAX_PLAUSIBLE_COMPRESSION_RATIO {
        return Err(IMPLAUSIBLE_COMPRESSION_RATIO);
    }
    if header.uncompressed_size == 0 {
        return Err(INVALID_TABLE_DIRECTORY);
    }
    let mut data = decompress(woff2, &header)?;

    let mut table_checksums = BTreeMap::new();
    for (font_index, info) in infos.iter_mut().enumerate() {
        reconstruct_font(
            &mut data,
            &mut header,
            font_index,
            info,
            headers_checksum,
            &mut table_checksums,
            &mut output,
        )?;
    }

    let sfnt = allocate(output.len()).ok_or("Failed to allocate the decoded WOFF2 font")?;
    sfnt.copy_from_slice(&output.bytes);
    Ok(())
}

/// Decodes a WOFF2 file into the sfnt font it packages, in the buffer that `allocate` gives. Returns null on success,
/// or else why the file is not valid, which is `error_length` bytes long.
///
/// # Safety
/// - `data` must be valid for `length` bytes.
/// - `allocate` is called once at most. It must return null, or `size` bytes of initialized memory that stay valid
///   and are not accessed otherwise until this function returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn woff2_rust_decode(
    data: *const u8,
    length: usize,
    context: *mut c_void,
    allocate: SfntAllocator,
    error_length: &mut usize,
) -> *const u8 {
    // SAFETY: The caller guarantees that `data` is valid for `length` bytes.
    let woff2 = unsafe { ak::slice_from_raw_parts(data, length) };
    // SAFETY: The caller guarantees that `allocate` keeps the contract of `SfntAllocator`.
    let result = decode(woff2, |size| unsafe { allocate_sfnt(context, allocate, size) });
    report(result, error_length)
}
