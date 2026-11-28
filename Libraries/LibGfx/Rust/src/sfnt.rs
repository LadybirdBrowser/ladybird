/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reading the big-endian data of font files, finding the tables of an sfnt font, and writing the parts of an sfnt font
//! that the font container decoders make.

use std::num::NonZeroU16;
use std::ops::Range;

pub const TABLE_DIRECTORY_HEADER_SIZE: usize = 12;
pub const TABLE_RECORD_SIZE: usize = 16;

const fn tag(name: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*name)
}

const COLLECTION_TAG: u32 = tag(b"ttcf");
const HMTX: u32 = tag(b"hmtx");
const VMTX: u32 = tag(b"vmtx");

const NO_SFNT_FACE: &str = "Font data has no face in the SFNT format";

/// Reads big-endian numbers and byte runs from the start of some data. A read past the end fails with the error the
/// reader was made with.
pub struct Reader<'a> {
    bytes: &'a [u8],
    error: &'static str,
}

impl<'a> Reader<'a> {
    pub fn new(bytes: &'a [u8], error: &'static str) -> Self {
        Self { bytes, error }
    }

    /// The bytes that are not read yet.
    pub fn remaining(&self) -> &'a [u8] {
        self.bytes
    }

    pub fn bytes(&mut self, length: usize) -> Result<&'a [u8], &'static str> {
        let Some((bytes, rest)) = self.bytes.split_at_checked(length) else {
            return Err(self.error);
        };
        self.bytes = rest;
        Ok(bytes)
    }

    pub fn skip(&mut self, length: usize) -> Result<(), &'static str> {
        self.bytes(length).map(|_| ())
    }

    fn array<const N: usize>(&mut self) -> Result<[u8; N], &'static str> {
        let Some((bytes, rest)) = self.bytes.split_first_chunk::<N>() else {
            return Err(self.error);
        };
        self.bytes = rest;
        Ok(*bytes)
    }

    pub fn u8(&mut self) -> Result<u8, &'static str> {
        self.array().map(u8::from_be_bytes)
    }

    pub fn u16(&mut self) -> Result<u16, &'static str> {
        self.array().map(u16::from_be_bytes)
    }

    pub fn u32(&mut self) -> Result<u32, &'static str> {
        self.array().map(u32::from_be_bytes)
    }
}

/// The offset of the table directory of a face, if the data is an sfnt font or a collection of them.
///
/// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
pub fn table_directory_offset(data: &[u8], face_index: u32) -> Option<usize> {
    let mut header = Reader::new(data, NO_SFNT_FACE);
    if header.u32().ok()? != COLLECTION_TAG {
        return (face_index == 0).then_some(0);
    }
    let _version = header.u32().ok()?;
    if face_index >= header.u32().ok()? {
        return None;
    }
    header.skip((face_index as usize).checked_mul(4)?).ok()?;
    header.u32().ok().map(|offset| offset as usize)
}

/// Finds a table of a face in font data, by the rules by which FreeType finds it. The first record of the table
/// directory with the tag counts. A table of length 0 is missing, and so is one that ends after the end of the data,
/// except for the metrics tables, which are cut off at the end of the data instead.
///
/// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#table-directory
pub fn find_table(data: &[u8], face_index: u32, tag: u32) -> Option<Range<usize>> {
    TableDirectory::of_face(data, face_index)?.find(tag)
}

/// Where the table directory of a face says a table is.
struct TableRecord {
    tag: u32,
    offset: usize,
    length: usize,
}

impl TableRecord {
    fn read(reader: &mut Reader) -> Result<Self, &'static str> {
        let mut record = Reader::new(reader.bytes(TABLE_RECORD_SIZE)?, NO_SFNT_FACE);
        let tag = record.u32()?;
        let _checksum = record.u32()?;
        let offset = record.u32()? as usize;
        let length = record.u32()? as usize;
        Ok(Self { tag, offset, length })
    }

    /// The length of the table in data of `data_length` bytes, or `None` if the table is not in the data.
    fn length_in(&self, data_length: usize) -> Option<usize> {
        let available = data_length.checked_sub(self.offset)?;
        if self.length <= available {
            return Some(self.length);
        }
        // Metrics tables are simple enough to cut off at the end of the data.
        (self.tag == HMTX || self.tag == VMTX).then_some(available & !3)
    }
}

/// The table directory of a face.
struct TableDirectory<'a> {
    data: &'a [u8],
    table_count: u16,
    /// The data from the first table record to the end.
    records: &'a [u8],
}

impl<'a> TableDirectory<'a> {
    fn of_face(data: &'a [u8], face_index: u32) -> Option<Self> {
        let mut header = Reader::new(data.get(table_directory_offset(data, face_index)?..)?, NO_SFNT_FACE);
        let _version = header.u32().ok()?;
        let table_count = header.u16().ok()?;
        header.skip(6).ok()?;
        Some(Self {
            data,
            table_count,
            records: header.remaining(),
        })
    }

    /// The table records, up to the first one that the end of the data cuts off.
    fn records(&self) -> impl Iterator<Item = TableRecord> + 'a {
        let mut records = Reader::new(self.records, NO_SFNT_FACE);
        (0..self.table_count).map_while(move |_| TableRecord::read(&mut records).ok())
    }

    fn find(&self, tag: u32) -> Option<Range<usize>> {
        let (offset, length) = self
            .records()
            .filter(|record| record.tag == tag)
            .find_map(|record| Some((record.offset, record.length_in(self.data.len())?)))?;
        (length != 0).then(|| offset..offset + length)
    }
}

/// The header of a table directory: the sfnt version, the number of tables, and the search fields derived from it.
///
/// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#table-directory
pub fn table_directory_header(sfnt_version: u32, num_tables: NonZeroU16) -> [u8; TABLE_DIRECTORY_HEADER_SIZE] {
    let entry_selector = num_tables.ilog2();
    // NB: The search fields are 16 bits wide, so they wrap for 4096 tables and more.
    let search_range = (1u32 << entry_selector) * 16;
    let range_shift = (u32::from(num_tables.get()) * 16).wrapping_sub(search_range);

    let mut header = [0; TABLE_DIRECTORY_HEADER_SIZE];
    header[0..4].copy_from_slice(&sfnt_version.to_be_bytes());
    header[4..6].copy_from_slice(&num_tables.get().to_be_bytes());
    header[6..8].copy_from_slice(&(search_range as u16).to_be_bytes());
    header[8..10].copy_from_slice(&(entry_selector as u16).to_be_bytes());
    header[10..12].copy_from_slice(&(range_shift as u16).to_be_bytes());
    header
}

/// A record of a table directory, which says where a table is in the file.
pub fn table_record(tag: u32, checksum: u32, offset: u32, length: u32) -> [u8; TABLE_RECORD_SIZE] {
    let mut record = [0; TABLE_RECORD_SIZE];
    record[0..4].copy_from_slice(&tag.to_be_bytes());
    record[4..8].copy_from_slice(&checksum.to_be_bytes());
    record[8..12].copy_from_slice(&offset.to_be_bytes());
    record[12..16].copy_from_slice(&length.to_be_bytes());
    record
}

/// Writes the offset of the table directory of a face to `directory_offset`. Returns false if the data has no such
/// face.
///
/// # Safety
/// `data` must be valid for `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sfnt_rust_table_directory_offset(
    data: *const u8,
    length: usize,
    face_index: u32,
    directory_offset: &mut usize,
) -> bool {
    // SAFETY: The caller guarantees that `data` is valid for `length` bytes.
    let data = unsafe { ak::slice_from_raw_parts(data, length) };
    let Some(offset) = table_directory_offset(data, face_index) else {
        return false;
    };
    *directory_offset = offset;
    true
}

/// Finds a table of a face in font data, and writes where it is to `table_offset` and `table_length`. Returns false if
/// the face has no such table.
///
/// # Safety
/// `data` must be valid for `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sfnt_rust_find_table(
    data: *const u8,
    length: usize,
    face_index: u32,
    tag: u32,
    table_offset: &mut usize,
    table_length: &mut usize,
) -> bool {
    // SAFETY: The caller guarantees that `data` is valid for `length` bytes.
    let data = unsafe { ak::slice_from_raw_parts(data, length) };
    let Some(table) = find_table(data, face_index, tag) else {
        return false;
    };
    *table_offset = table.start;
    *table_length = table.len();
    true
}
