/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reading the big-endian data of font files, and writing the parts of an sfnt font that the font container decoders
//! make.

use std::num::NonZeroU16;

pub const TABLE_DIRECTORY_HEADER_SIZE: usize = 12;
pub const TABLE_RECORD_SIZE: usize = 16;

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
