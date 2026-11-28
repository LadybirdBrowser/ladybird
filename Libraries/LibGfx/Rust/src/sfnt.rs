/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Reading the big-endian data of font files, finding the tables of an sfnt font, checking that a face in font data
//! loads, and writing the parts of an sfnt font that the font container decoders make.

use std::borrow::Cow;
use std::num::NonZeroU16;
use std::ops::Range;

pub const TABLE_DIRECTORY_HEADER_SIZE: usize = 12;
pub const TABLE_RECORD_SIZE: usize = 16;

const fn tag(name: &[u8; 4]) -> u32 {
    u32::from_be_bytes(*name)
}

const COLLECTION_TAG: u32 = tag(b"ttcf");
const TRUETYPE_VERSION: u32 = 0x0001_0000;
const CFF_VERSION: u32 = tag(b"OTTO");
const APPLE_TRUETYPE_VERSION: u32 = tag(b"true");

const BHED: u32 = tag(b"bhed");
const BLOC: u32 = tag(b"bloc");
const CBDT: u32 = tag(b"CBDT");
const CBLC: u32 = tag(b"CBLC");
const CFF: u32 = tag(b"CFF ");
const CFF2: u32 = tag(b"CFF2");
const EBLC: u32 = tag(b"EBLC");
const GLYF: u32 = tag(b"glyf");
const HEAD: u32 = tag(b"head");
const HHEA: u32 = tag(b"hhea");
const HMTX: u32 = tag(b"hmtx");
const LOCA: u32 = tag(b"loca");
const MAXP: u32 = tag(b"maxp");
const META: u32 = tag(b"META");
const SBIX: u32 = tag(b"sbix");
const SING: u32 = tag(b"SING");
const VMTX: u32 = tag(b"vmtx");

const HEAD_SIZE: usize = 54;
const HHEA_SIZE: usize = 36;

const NO_SFNT_FACE: &str = "Font data has no face in the SFNT format";
const NO_HEAD_TABLE: &str = "Font face has no head table";

/// Skia does not load font data of more than 1 GiB.
const MAX_FONT_DATA_SIZE: usize = 1 << 30;

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
    version: u32,
    table_count: u16,
    /// The data from the first table record to the end.
    records: &'a [u8],
}

impl<'a> TableDirectory<'a> {
    fn of_face(data: &'a [u8], face_index: u32) -> Option<Self> {
        Self::at(data, table_directory_offset(data, face_index)?)
    }

    fn at(data: &'a [u8], offset: usize) -> Option<Self> {
        let mut header = Reader::new(data.get(offset..)?, NO_SFNT_FACE);
        let version = header.u32().ok()?;
        let table_count = header.u16().ok()?;
        header.skip(6).ok()?;
        Some(Self {
            data,
            version,
            table_count,
            records: header.remaining(),
        })
    }

    /// The table records, up to the first one that the end of the data cuts off.
    fn records(&self) -> impl Iterator<Item = TableRecord> + 'a {
        let mut records = Reader::new(self.records, NO_SFNT_FACE);
        (0..self.table_count).map_while(move |_| TableRecord::read(&mut records).ok())
    }

    fn has_all_records(&self) -> bool {
        self.records.len() / TABLE_RECORD_SIZE >= usize::from(self.table_count)
    }

    /// The rules of FreeType for a table directory. A table that starts after the end of the data does not count, and
    /// neither does one that ends after it, except for the metrics tables, which FreeType cuts off.
    fn is_valid(&self) -> bool {
        if self.table_count == 0 {
            return false;
        }
        // FreeType checks the directory of a face with CFF outlines no further.
        if self.version == CFF_VERSION {
            return self.has_all_records();
        }

        let mut has_head = false;
        let mut has_sing = false;
        let mut has_meta = false;
        for record in self.records() {
            let Some(length) = record.length_in(self.data.len()) else {
                continue;
            };
            match record.tag {
                HEAD | BHED => {
                    // Each font header must have its full size, whichever of them FreeType reads.
                    if length < HEAD_SIZE {
                        return false;
                    }
                    has_head = true;
                }
                SING => has_sing = true,
                META => has_meta = true,
                _ => {}
            }
        }
        // A face with SING and META tables, which Adobe fonts of single glyphs have, does not need a font header.
        has_head || (has_sing && has_meta)
    }

    fn find(&self, tag: u32) -> Option<Range<usize>> {
        let (offset, length) = self
            .records()
            .filter(|record| record.tag == tag)
            .find_map(|record| Some((record.offset, record.length_in(self.data.len())?)))?;
        (length != 0).then(|| offset..offset + length)
    }

    fn has_table(&self, tag: u32) -> bool {
        self.find(tag).is_some()
    }

    /// A header of a table that FreeType reads with its fixed size, from the following data if the table is shorter.
    fn table_header(&self, tag: u32, size: usize) -> Option<&'a [u8]> {
        self.data.get(self.find(tag)?.start..)?.get(..size)
    }
}

/// The data from which HarfBuzz, and so LibGfx, reads the tables of a face. HarfBuzz reads a collection from a copy in
/// which it sets the offset of each face whose table directory runs past the end of the data to 0, one face after the
/// other.
///
/// https://learn.microsoft.com/en-us/typography/opentype/spec/otff#ttc-header
fn data_that_harfbuzz_reads(data: &[u8]) -> Cow<'_, [u8]> {
    let mut header = Reader::new(data, NO_SFNT_FACE);
    let (Ok(COLLECTION_TAG), Ok(1 | 2), Ok(_minor_version), Ok(face_count)) =
        (header.u32(), header.u16(), header.u16(), header.u32())
    else {
        return Cow::Borrowed(data);
    };
    let mut data = Cow::Borrowed(data);
    for slot in (12..data.len()).step_by(4).take(face_count as usize) {
        let Some(&offset) = data.get(slot..).and_then(|bytes| bytes.first_chunk::<4>()) else {
            break;
        };
        let offset = u32::from_be_bytes(offset) as usize;
        if offset == 0 || TableDirectory::at(&data, offset).is_some_and(|directory| directory.has_all_records()) {
            continue;
        }
        if let Some(offset) = data.to_mut().get_mut(slot..slot + 4) {
            offset.fill(0);
        }
    }
    data
}

/// Checks that font data has a face that FreeType loads as an sfnt font, by the rules by which FreeType loads the
/// tables of a face. `face_index` selects a face of a collection.
///
/// https://learn.microsoft.com/en-us/typography/opentype/spec/otff
pub fn validate_face(data: &[u8], face_index: u32) -> Result<(), &'static str> {
    if data.len() > MAX_FONT_DATA_SIZE {
        return Err("Font data is too large");
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#table-directory
    let directory = TableDirectory::of_face(data, face_index).ok_or(NO_SFNT_FACE)?;
    let version = directory.version;
    if !matches!(version, TRUETYPE_VERSION | CFF_VERSION | APPLE_TRUETYPE_VERSION) || !directory.is_valid() {
        return Err(NO_SFNT_FACE);
    }

    // FreeType reads the table directory from the data, but LibGfx reads the tables through HarfBuzz.
    let tables_data = data_that_harfbuzz_reads(data);
    let tables = TableDirectory::of_face(&tables_data, face_index);
    let has_table = |tag| tables.as_ref().is_some_and(|tables| tables.has_table(tag));
    let table_header = |tag, size| tables.as_ref().and_then(|tables| tables.table_header(tag, size));

    // https://learn.microsoft.com/en-us/typography/opentype/spec/head
    // A face without outlines can be an Apple bitmap font, which has a bhed table instead of a head table.
    let has_outlines = has_table(GLYF) || has_table(CFF) || has_table(CFF2);
    let bitmap_header = if has_outlines {
        None
    } else {
        table_header(BHED, HEAD_SIZE)
    };
    let is_apple_bitmap_font = bitmap_header.is_some();
    let header = match bitmap_header {
        Some(header) if !has_table(SBIX) => header,
        _ => table_header(HEAD, HEAD_SIZE).ok_or(NO_HEAD_TABLE)?,
    };
    let mut header = Reader::new(header, NO_HEAD_TABLE);
    header.skip(18)?;
    if !(16..=16384).contains(&header.u16()?) {
        return Err("Font face has an invalid number of units per em");
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/maxp
    if !has_table(MAXP) {
        return Err("Font face has no maxp table");
    }

    // https://learn.microsoft.com/en-us/typography/opentype/spec/hhea
    // A TrueType font for macOS does not need horizontal metrics.
    if !is_apple_bitmap_font {
        if table_header(HHEA, HHEA_SIZE).is_some() {
            if !has_table(HMTX) {
                return Err("Font face has no hmtx table");
            }
        } else if version != APPLE_TRUETYPE_VERSION {
            return Err("Font face has no hhea table");
        }
    }

    if version == CFF_VERSION {
        // https://learn.microsoft.com/en-us/typography/opentype/spec/otff#organization-of-an-opentype-font
        if !has_table(CFF) && !has_table(CFF2) {
            return Err("Font face with CFF outlines has no CFF table");
        }
    } else {
        // https://learn.microsoft.com/en-us/typography/opentype/spec/loca
        // FreeType needs a loca table for every TrueType face that it does not draw from bitmap strikes alone. It
        // draws a face without outlines or strikes as a face with empty outlines, and color bitmaps replace outlines.
        let has_scalable_outlines = has_outlines && !has_table(CBLC) && !has_table(CBDT);
        let has_bitmap_strikes = has_table(EBLC) || has_table(CBLC) || has_table(BLOC) || has_table(SBIX);
        if (has_scalable_outlines || !has_bitmap_strikes) && !has_table(LOCA) {
            return Err("Font face has no loca table");
        }
    }
    Ok(())
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

/// Checks that font data has a face that loads, as `validate_face` does. Returns null if it does, or else why it does
/// not, which is `error_length` bytes long.
///
/// # Safety
/// `data` must be valid for `length` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn sfnt_rust_validate_face(
    data: *const u8,
    length: usize,
    face_index: u32,
    error_length: &mut usize,
) -> *const u8 {
    // SAFETY: The caller guarantees that `data` is valid for `length` bytes.
    let data = unsafe { ak::slice_from_raw_parts(data, length) };
    crate::woff::report(validate_face(data, face_index), error_length)
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
