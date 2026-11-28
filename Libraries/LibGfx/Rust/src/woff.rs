/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Decoding of WOFF 1.0 files into the sfnt fonts they package.
//!
//! https://www.w3.org/TR/WOFF/

use std::ffi::c_void;
use std::num::NonZeroU16;
use std::ptr;

use libcompress_rust::{Decompressor, Format};

use crate::sfnt::{self, Reader, TABLE_DIRECTORY_HEADER_SIZE, TABLE_RECORD_SIZE};

const SIGNATURE: u32 = 0x774F4646;
const HEADER_SIZE: usize = 44;
const TABLE_DIRECTORY_ENTRY_SIZE: usize = 20;

/// We refuse to decode a WOFF file into an sfnt of more than this many bytes.
const MAX_SFNT_SIZE: u32 = 10 * 1024 * 1024;

/// The fields of a WOFF header that decoding uses, after the header has been checked.
///
/// https://www.w3.org/TR/WOFF/#WOFFHeader
struct Header {
    flavor: u32,
    length: usize,
    num_tables: NonZeroU16,
    total_sfnt_size: u32,
}

impl Header {
    fn parse(woff: &[u8]) -> Result<Self, &'static str> {
        let mut reader = Reader::new(woff, "Truncated WOFF header");
        let signature = reader.u32()?;
        let flavor = reader.u32()?;
        let length = reader.u32()? as usize;
        let num_tables = reader.u16()?;
        let reserved = reader.u16()?;
        let total_sfnt_size = reader.u32()?;
        let _major_version = reader.u16()?;
        let _minor_version = reader.u16()?;
        let meta_offset = reader.u32()?;
        let meta_length = reader.u32()?;
        let _meta_orig_length = reader.u32()?;
        let priv_offset = reader.u32()?;
        let priv_length = reader.u32()?;

        // The signature field in the WOFF header MUST contain the "magic number" 0x774F4646. If the field does not
        // contain this value, user agents MUST reject the file as invalid.
        if signature != SIGNATURE {
            return Err("Invalid WOFF signature");
        }
        // NB: The flavor is the sfnt version of the font. Any value is valid in a WOFF file.

        if length > woff.len() {
            return Err("Invalid WOFF length");
        }
        let Some(num_tables) = NonZeroU16::new(num_tables).filter(|count| count.get() <= u16::MAX / 16) else {
            return Err("Invalid WOFF numTables");
        };
        if reserved != 0 {
            return Err("Invalid WOFF reserved field");
        }
        if meta_length == 0 && meta_offset != 0 {
            return Err("Invalid WOFF meta block offset");
        }
        if priv_length == 0 && priv_offset != 0 {
            return Err("Invalid WOFF private block offset");
        }
        if HEADER_SIZE + usize::from(num_tables.get()) * TABLE_DIRECTORY_ENTRY_SIZE > length {
            return Err("Truncated WOFF table directory");
        }
        if (total_sfnt_size as usize) < sfnt_table_directory_size(num_tables) {
            return Err("Invalid WOFF total sfnt size");
        }
        if total_sfnt_size > MAX_SFNT_SIZE {
            return Err("Uncompressed font is more than 10 MiB");
        }

        Ok(Self {
            flavor,
            length,
            num_tables,
            total_sfnt_size,
        })
    }
}

fn sfnt_table_directory_size(num_tables: NonZeroU16) -> usize {
    TABLE_DIRECTORY_HEADER_SIZE + usize::from(num_tables.get()) * TABLE_RECORD_SIZE
}

/// Decodes a WOFF file into the sfnt it packages. The sfnt goes into the buffer that `allocate` gives for its size,
/// which is all written once decoding succeeds.
pub fn decode<'a>(woff: &[u8], allocate: impl FnOnce(usize) -> Option<&'a mut [u8]>) -> Result<(), &'static str> {
    let header = Header::parse(woff)?;
    let woff = woff.get(..header.length).ok_or("Invalid WOFF length")?;
    let sfnt = allocate(header.total_sfnt_size as usize).ok_or("Failed to allocate the decoded WOFF font")?;

    let directory_size = sfnt_table_directory_size(header.num_tables);
    write(sfnt, 0, &sfnt::table_directory_header(header.flavor, header.num_tables))?;

    // https://www.w3.org/TR/WOFF/#TableDirectory
    let mut entries = Reader::new(
        woff.get(HEADER_SIZE..).unwrap_or_default(),
        "Truncated WOFF table directory",
    );
    let mut expected_sfnt_size = directory_size as u64;
    let mut sfnt_offset = directory_size;
    for index in 0..usize::from(header.num_tables.get()) {
        let tag = entries.u32()?;
        let offset = entries.u32()? as usize;
        let comp_length = entries.u32()? as usize;
        let orig_length = entries.u32()?;
        let orig_checksum = entries.u32()?;

        // NB: Each table in the sfnt starts on a 4-byte boundary, so the total sfnt size counts the padding.
        let padded_length = (u64::from(orig_length) + 3) & !3;
        expected_sfnt_size += padded_length;
        if expected_sfnt_size > u64::from(header.total_sfnt_size) {
            return Err("Invalid WOFF total sfnt size");
        }
        let compressed = offset
            .checked_add(comp_length)
            .and_then(|end| woff.get(offset..end))
            .ok_or("Truncated WOFF table")?;
        // NB: The total sfnt size is checked above to hold the table and its padding, so these do not overflow.
        let table_end = sfnt_offset + orig_length as usize;
        let padded_end = sfnt_offset + padded_length as usize;
        let table = sfnt
            .get_mut(sfnt_offset..table_end)
            .ok_or("Uncompressed WOFF table too big")?;

        if comp_length < table.len() {
            decompress_table(compressed, table)?;
        } else if comp_length != table.len() {
            return Err("Invalid uncompressed WOFF table length");
        } else {
            table.copy_from_slice(compressed);
        }
        sfnt.get_mut(table_end..padded_end)
            .ok_or("Uncompressed WOFF table too big")?
            .fill(0);

        let record_offset = TABLE_DIRECTORY_HEADER_SIZE + index * TABLE_RECORD_SIZE;
        write(
            sfnt,
            record_offset,
            &sfnt::table_record(tag, orig_checksum, sfnt_offset as u32, orig_length),
        )?;

        sfnt_offset = padded_end;
    }

    if expected_sfnt_size != u64::from(header.total_sfnt_size) {
        return Err("Invalid WOFF total sfnt size");
    }
    Ok(())
}

fn write(sfnt: &mut [u8], offset: usize, data: &[u8]) -> Result<(), &'static str> {
    sfnt.get_mut(offset..offset + data.len())
        .ok_or("Invalid WOFF total sfnt size")?
        .copy_from_slice(data);
    Ok(())
}

/// Decompresses the zlib data of a table, which has to decompress to exactly the size of `table`.
///
/// NB: Data that holds more than one zlib stream is accepted, as long as the streams end with the data.
fn decompress_table(compressed: &[u8], table: &mut [u8]) -> Result<(), &'static str> {
    const INVALID_DATA: &str = "Invalid compressed WOFF table";
    const INVALID_LENGTH: &str = "Invalid decompressed WOFF table length";

    let mut decompressor = Decompressor::new(Format::Zlib);
    let mut input = compressed;
    let mut table_length = 0;
    // Once the table is full, the decoder writes here, and any byte it writes means the data is too long.
    let mut excess = [0; 1];
    loop {
        let is_full = table_length == table.len();
        let output = if is_full {
            &mut excess[..]
        } else {
            table.get_mut(table_length..).ok_or(INVALID_LENGTH)?
        };
        let decompressed = decompressor.decompress_into(input, output).map_err(|_| INVALID_DATA)?;
        if is_full && decompressed.output_length != 0 {
            return Err(INVALID_LENGTH);
        }
        table_length += decompressed.output_length;
        input = input.get(decompressed.consumed_input..).ok_or(INVALID_DATA)?;

        if decompressor.has_reached_end() {
            if input.is_empty() {
                break;
            }
            decompressor = Decompressor::new(Format::Zlib);
        } else if decompressed.consumed_input == 0 && decompressed.output_length == 0 {
            // NB: The data ends before its zlib stream does.
            return Err(INVALID_LENGTH);
        }
    }

    if table_length != table.len() {
        return Err(INVALID_LENGTH);
    }
    Ok(())
}

/// Gives the buffer that a decoded font goes into: `size` bytes of initialized memory to write, or null when it
/// cannot.
pub type SfntAllocator = unsafe extern "C" fn(context: *mut c_void, size: usize) -> *mut u8;

/// Asks the caller of a decoder for the buffer of the decoded font.
///
/// # Safety
/// `allocate` must keep the contract of `SfntAllocator`, and the buffer it gives must stay valid and unaliased for
/// `'a`.
pub(crate) unsafe fn allocate_sfnt<'a>(
    context: *mut c_void,
    allocate: SfntAllocator,
    size: usize,
) -> Option<&'a mut [u8]> {
    // SAFETY: Guaranteed by the caller.
    let data = unsafe { allocate(context, size) };
    if data.is_null() {
        return None;
    }
    // SAFETY: The allocator gives `size` bytes of initialized memory, which the caller guarantees for `'a`.
    Some(unsafe { ak::slice_from_raw_parts_mut(data, size) })
}

/// Returns null for a success, or else the description of the failure, which is `error_length` bytes long.
pub(crate) fn report(result: Result<(), &'static str>, error_length: &mut usize) -> *const u8 {
    match result {
        Ok(()) => ptr::null(),
        Err(error) => {
            *error_length = error.len();
            error.as_ptr()
        }
    }
}

/// Decodes a WOFF file into the sfnt font it packages, in the buffer that `allocate` gives. Returns null on success,
/// or else why the file is not valid, which is `error_length` bytes long.
///
/// # Safety
/// - `data` must be valid for `length` bytes.
/// - `allocate` is called once at most. It must return null, or `size` bytes of initialized memory that stay valid
///   and are not accessed otherwise until this function returns.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn woff_rust_decode(
    data: *const u8,
    length: usize,
    context: *mut c_void,
    allocate: SfntAllocator,
    error_length: &mut usize,
) -> *const u8 {
    // SAFETY: The caller guarantees that `data` is valid for `length` bytes.
    let woff = unsafe { ak::slice_from_raw_parts(data, length) };
    // SAFETY: The caller guarantees that `allocate` keeps the contract of `SfntAllocator`.
    let result = decode(woff, |size| unsafe { allocate_sfnt(context, allocate, size) });
    report(result, error_length)
}
