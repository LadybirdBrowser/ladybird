/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use std::collections::{HashMap, HashSet};

const MAGIC: &[u8; 8] = b"LBFONTS\0";
const VERSION: u32 = 1;
const HEADER_SIZE: usize = 48;
const RECORD_SIZE: usize = 32;

#[repr(C)]
pub struct FfiFontCatalogFaceInput {
    pub family: *const u8,
    pub family_length: usize,
    pub face_id: u64,
    pub ttc_index: u32,
    pub weight: u16,
    pub width: u16,
    pub slope: u8,
    pub format: u8,
}

#[repr(C)]
#[derive(Clone, Copy, Default)]
pub struct FfiFontCatalogFace {
    pub family: *const u8,
    pub family_length: usize,
    pub face_id: u64,
    pub ttc_index: u32,
    pub weight: u16,
    pub width: u16,
    pub slope: u8,
    pub format: u8,
}

struct BuilderFace {
    family: Vec<u8>,
    face_id: u64,
    ttc_index: u32,
    weight: u16,
    width: u16,
    slope: u8,
    format: u8,
}

pub struct FontCatalogBuilder {
    generation: u64,
    faces: Vec<BuilderFace>,
    face_ids: HashSet<u64>,
}

#[derive(Clone, Copy)]
struct ParsedFace {
    family_offset: usize,
    family_length: usize,
    face_id: u64,
    ttc_index: u32,
    weight: u16,
    width: u16,
    slope: u8,
    format: u8,
}

pub struct FontCatalog {
    data: *const u8,
    data_length: usize,
    generation: u64,
    faces: Vec<ParsedFace>,
    face_by_id: HashMap<u64, usize>,
    faces_by_family: HashMap<u64, Vec<usize>>,
}

fn valid_face(face_id: u64, family: &[u8], weight: u16, width: u16, slope: u8, format: u8) -> bool {
    face_id != 0
        && !family.is_empty()
        && std::str::from_utf8(family).is_ok()
        && (1..=1000).contains(&weight)
        && (1..=9).contains(&width)
        && slope <= 2
        && format <= 1
}

fn family_hash(bytes: &[u8]) -> u64 {
    bytes.iter().fold(0xcbf29ce484222325, |hash, byte| {
        (hash ^ u64::from(byte.to_ascii_lowercase())).wrapping_mul(0x100000001b3)
    })
}

fn family_names_equal(left: &[u8], right: &[u8]) -> bool {
    left.len() == right.len()
        && left
            .iter()
            .zip(right)
            .all(|(left, right)| left.eq_ignore_ascii_case(right))
}

fn push_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn push_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn push_u64(output: &mut Vec<u8>, value: u64) {
    output.extend_from_slice(&value.to_le_bytes());
}

fn read_u16(data: &[u8], offset: usize) -> Option<u16> {
    Some(u16::from_le_bytes(
        data.get(offset..offset.checked_add(2)?)?.try_into().ok()?,
    ))
}

fn read_u32(data: &[u8], offset: usize) -> Option<u32> {
    Some(u32::from_le_bytes(
        data.get(offset..offset.checked_add(4)?)?.try_into().ok()?,
    ))
}

fn read_u64(data: &[u8], offset: usize) -> Option<u64> {
    Some(u64::from_le_bytes(
        data.get(offset..offset.checked_add(8)?)?.try_into().ok()?,
    ))
}

impl FontCatalogBuilder {
    fn serialize(&self) -> Option<Vec<u8>> {
        let records_size = self.faces.len().checked_mul(RECORD_SIZE)?;
        let strings_size = self
            .faces
            .iter()
            .try_fold(0usize, |size, face| size.checked_add(face.family.len()))?;
        let total_size = HEADER_SIZE.checked_add(records_size)?.checked_add(strings_size)?;
        let face_count = u64::try_from(self.faces.len()).ok()?;
        let total_size_u64 = u64::try_from(total_size).ok()?;

        let mut output = Vec::with_capacity(total_size);
        output.extend_from_slice(MAGIC);
        push_u32(&mut output, VERSION);
        push_u32(&mut output, HEADER_SIZE as u32);
        push_u64(&mut output, total_size_u64);
        push_u64(&mut output, self.generation);
        push_u64(&mut output, face_count);
        push_u64(&mut output, HEADER_SIZE as u64);

        let mut family_offset = HEADER_SIZE.checked_add(records_size)?;
        for face in &self.faces {
            push_u64(&mut output, face.face_id);
            push_u64(&mut output, u64::try_from(family_offset).ok()?);
            push_u32(&mut output, u32::try_from(face.family.len()).ok()?);
            push_u32(&mut output, face.ttc_index);
            push_u16(&mut output, face.weight);
            push_u16(&mut output, face.width);
            output.push(face.slope);
            output.push(face.format);
            push_u16(&mut output, 0);
            family_offset = family_offset.checked_add(face.family.len())?;
        }
        for face in &self.faces {
            output.extend_from_slice(&face.family);
        }

        debug_assert_eq!(output.len(), total_size);
        Some(output)
    }
}

impl FontCatalog {
    fn parse(data: &[u8], expected_generation: u64) -> Option<Self> {
        if data.len() < HEADER_SIZE || data.get(0..8)? != MAGIC {
            return None;
        }
        if read_u32(data, 8)? != VERSION || read_u32(data, 12)? as usize != HEADER_SIZE {
            return None;
        }
        if usize::try_from(read_u64(data, 16)?).ok()? != data.len() {
            return None;
        }
        let generation = read_u64(data, 24)?;
        if generation == 0 || generation != expected_generation {
            return None;
        }
        let face_count = usize::try_from(read_u64(data, 32)?).ok()?;
        let records_offset = usize::try_from(read_u64(data, 40)?).ok()?;
        if records_offset != HEADER_SIZE {
            return None;
        }
        let records_size = face_count.checked_mul(RECORD_SIZE)?;
        let strings_offset = records_offset.checked_add(records_size)?;
        if strings_offset > data.len() {
            return None;
        }

        let mut faces = Vec::with_capacity(face_count);
        let mut face_by_id = HashMap::with_capacity(face_count);
        let mut faces_by_family: HashMap<u64, Vec<usize>> = HashMap::new();

        for index in 0..face_count {
            let offset = records_offset.checked_add(index.checked_mul(RECORD_SIZE)?)?;
            let face_id = read_u64(data, offset)?;
            let family_offset = usize::try_from(read_u64(data, offset + 8)?).ok()?;
            let family_length = usize::try_from(read_u32(data, offset + 16)?).ok()?;
            let ttc_index = read_u32(data, offset + 20)?;
            let weight = read_u16(data, offset + 24)?;
            let width = read_u16(data, offset + 26)?;
            let slope = *data.get(offset + 28)?;
            let format = *data.get(offset + 29)?;
            if read_u16(data, offset + 30)? != 0 {
                return None;
            }

            let family_end = family_offset.checked_add(family_length)?;
            if family_offset < strings_offset || family_end > data.len() {
                return None;
            }
            let family = data.get(family_offset..family_end)?;
            if !valid_face(face_id, family, weight, width, slope, format) || face_by_id.contains_key(&face_id) {
                return None;
            }

            let face = ParsedFace {
                family_offset,
                family_length,
                face_id,
                ttc_index,
                weight,
                width,
                slope,
                format,
            };
            faces.push(face);
            face_by_id.insert(face_id, index);
            faces_by_family.entry(family_hash(family)).or_default().push(index);
        }

        Some(Self {
            data: data.as_ptr(),
            data_length: data.len(),
            generation,
            faces,
            face_by_id,
            faces_by_family,
        })
    }

    fn face_for_ffi(&self, index: usize) -> Option<FfiFontCatalogFace> {
        let face = *self.faces.get(index)?;
        let family_end = face.family_offset.checked_add(face.family_length)?;
        if family_end > self.data_length {
            return None;
        }
        Some(FfiFontCatalogFace {
            // SAFETY: parse() validated this offset and the caller keeps the snapshot alive.
            family: unsafe { self.data.add(face.family_offset) },
            family_length: face.family_length,
            face_id: face.face_id,
            ttc_index: face.ttc_index,
            weight: face.weight,
            width: face.width,
            slope: face.slope,
            format: face.format,
        })
    }

    fn family_bytes(&self, index: usize) -> Option<&[u8]> {
        let face = *self.faces.get(index)?;
        let family_end = face.family_offset.checked_add(face.family_length)?;
        if family_end > self.data_length {
            return None;
        }
        // SAFETY: parse() validated this range and the snapshot outlives the catalog.
        Some(unsafe { std::slice::from_raw_parts(self.data.add(face.family_offset), face.family_length) })
    }

    fn family_face_count(&self, family: &[u8]) -> usize {
        self.faces_by_family
            .get(&family_hash(family))
            .into_iter()
            .flatten()
            .filter(|index| {
                self.family_bytes(**index)
                    .is_some_and(|candidate| family_names_equal(candidate, family))
            })
            .count()
    }

    fn family_face_index_at(&self, family: &[u8], family_index: usize) -> Option<usize> {
        self.faces_by_family
            .get(&family_hash(family))?
            .iter()
            .copied()
            .filter(|index| {
                self.family_bytes(*index)
                    .is_some_and(|candidate| family_names_equal(candidate, family))
            })
            .nth(family_index)
    }

    fn match_style_index(&self, family: &[u8], weight: u16, width: u16, slope: u8) -> Option<usize> {
        self.faces_by_family
            .get(&family_hash(family))?
            .iter()
            .copied()
            .find(|index| {
                let face = self.faces[*index];
                self.family_bytes(*index)
                    .is_some_and(|candidate| family_names_equal(candidate, family))
                    && face.weight == weight
                    && face.width == width
                    && face.slope == slope
            })
    }
}

#[unsafe(no_mangle)]
pub extern "C" fn font_catalog_builder_create(generation: u64) -> *mut FontCatalogBuilder {
    if generation == 0 {
        return std::ptr::null_mut();
    }
    Box::into_raw(Box::new(FontCatalogBuilder {
        generation,
        faces: Vec::new(),
        face_ids: HashSet::new(),
    }))
}

/// # Safety
/// `builder` must be returned by `font_catalog_builder_create`, and `input.family`
/// must be readable for `input.family_length` bytes during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_builder_add_face(
    builder: *mut FontCatalogBuilder,
    input: FfiFontCatalogFaceInput,
) -> bool {
    let Some(builder) = (unsafe { builder.as_mut() }) else {
        return false;
    };
    if input.family.is_null() {
        return false;
    }
    let family = unsafe { std::slice::from_raw_parts(input.family, input.family_length) };
    if !valid_face(
        input.face_id,
        family,
        input.weight,
        input.width,
        input.slope,
        input.format,
    ) || !builder.face_ids.insert(input.face_id)
    {
        return false;
    }
    builder.faces.push(BuilderFace {
        family: family.to_vec(),
        face_id: input.face_id,
        ttc_index: input.ttc_index,
        weight: input.weight,
        width: input.width,
        slope: input.slope,
        format: input.format,
    });
    true
}

/// # Safety
/// `builder` must be returned by `font_catalog_builder_create`. If `output` is
/// non-null, it must be writable for `output_capacity` bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_builder_serialize(
    builder: *const FontCatalogBuilder,
    output: *mut u8,
    output_capacity: usize,
) -> usize {
    let Some(builder) = (unsafe { builder.as_ref() }) else {
        return 0;
    };
    let Some(serialized) = builder.serialize() else {
        return 0;
    };
    if output.is_null() {
        return serialized.len();
    }
    if output_capacity < serialized.len() {
        return 0;
    }
    unsafe { std::ptr::copy_nonoverlapping(serialized.as_ptr(), output, serialized.len()) };
    serialized.len()
}

/// # Safety
/// `builder` must be null or returned by `font_catalog_builder_create`, and it
/// must not be used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_builder_destroy(builder: *mut FontCatalogBuilder) {
    if !builder.is_null() {
        drop(unsafe { Box::from_raw(builder) });
    }
}

/// # Safety
/// `data` must remain readable and at the same address until the returned
/// catalog is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_parse(
    data: *const u8,
    data_length: usize,
    expected_generation: u64,
) -> *mut FontCatalog {
    if data.is_null() {
        return std::ptr::null_mut();
    }
    let data = unsafe { std::slice::from_raw_parts(data, data_length) };
    FontCatalog::parse(data, expected_generation)
        .map(|catalog| Box::into_raw(Box::new(catalog)))
        .unwrap_or(std::ptr::null_mut())
}

/// # Safety
/// `catalog` must be null or returned by `font_catalog_parse`, and it must not
/// be used after this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_destroy(catalog: *mut FontCatalog) {
    if !catalog.is_null() {
        drop(unsafe { Box::from_raw(catalog) });
    }
}

/// # Safety
/// `catalog` must point to a live parsed catalog.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_generation(catalog: *const FontCatalog) -> u64 {
    unsafe { catalog.as_ref() }.map_or(0, |catalog| catalog.generation)
}

/// # Safety
/// `catalog` must point to a live parsed catalog.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_face_count(catalog: *const FontCatalog) -> usize {
    unsafe { catalog.as_ref() }.map_or(0, |catalog| catalog.faces.len())
}

/// # Safety
/// `catalog` must point to a live parsed catalog and `output` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_face_at(
    catalog: *const FontCatalog,
    index: usize,
    output: *mut FfiFontCatalogFace,
) -> bool {
    let (Some(catalog), Some(output)) = (unsafe { catalog.as_ref() }, unsafe { output.as_mut() }) else {
        return false;
    };
    let Some(face) = catalog.face_for_ffi(index) else {
        return false;
    };
    *output = face;
    true
}

/// # Safety
/// `catalog` must point to a live parsed catalog. `family` must be readable for
/// `family_length` bytes during this call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_family_face_count(
    catalog: *const FontCatalog,
    family: *const u8,
    family_length: usize,
) -> usize {
    let Some(catalog) = (unsafe { catalog.as_ref() }) else {
        return 0;
    };
    if family.is_null() {
        return 0;
    }
    let family = unsafe { std::slice::from_raw_parts(family, family_length) };
    catalog.family_face_count(family)
}

/// # Safety
/// `catalog` must point to a live parsed catalog. `family` must be readable for
/// `family_length` bytes during this call and `output` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_family_face_at(
    catalog: *const FontCatalog,
    family: *const u8,
    family_length: usize,
    index: usize,
    output: *mut FfiFontCatalogFace,
) -> bool {
    let (Some(catalog), Some(output)) = (unsafe { catalog.as_ref() }, unsafe { output.as_mut() }) else {
        return false;
    };
    if family.is_null() {
        return false;
    }
    let family = unsafe { std::slice::from_raw_parts(family, family_length) };
    let Some(face_index) = catalog.family_face_index_at(family, index) else {
        return false;
    };
    let Some(face) = catalog.face_for_ffi(face_index) else {
        return false;
    };
    *output = face;
    true
}

/// # Safety
/// `catalog` must point to a live parsed catalog and `output` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_face_by_id(
    catalog: *const FontCatalog,
    face_id: u64,
    output: *mut FfiFontCatalogFace,
) -> bool {
    let (Some(catalog), Some(output)) = (unsafe { catalog.as_ref() }, unsafe { output.as_mut() }) else {
        return false;
    };
    let Some(index) = catalog.face_by_id.get(&face_id) else {
        return false;
    };
    let Some(face) = catalog.face_for_ffi(*index) else {
        return false;
    };
    *output = face;
    true
}

/// # Safety
/// `catalog` must point to a live parsed catalog. `family` must be readable for
/// `family_length` bytes during this call and `output` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn font_catalog_match_style(
    catalog: *const FontCatalog,
    family: *const u8,
    family_length: usize,
    weight: u16,
    width: u16,
    slope: u8,
    output: *mut FfiFontCatalogFace,
) -> bool {
    let (Some(catalog), Some(output)) = (unsafe { catalog.as_ref() }, unsafe { output.as_mut() }) else {
        return false;
    };
    if family.is_null() {
        return false;
    }
    let family = unsafe { std::slice::from_raw_parts(family, family_length) };
    let Some(index) = catalog.match_style_index(family, weight, width, slope) else {
        return false;
    };
    let Some(face) = catalog.face_for_ffi(index) else {
        return false;
    };
    *output = face;
    true
}

#[cfg(test)]
mod tests {
    use super::*;

    fn serialized_catalog() -> Vec<u8> {
        let mut builder = FontCatalogBuilder {
            generation: 7,
            faces: Vec::new(),
            face_ids: HashSet::new(),
        };
        builder.faces.push(BuilderFace {
            family: b"Example Sans".to_vec(),
            face_id: 11,
            ttc_index: 2,
            weight: 400,
            width: 5,
            slope: 0,
            format: 0,
        });
        builder.face_ids.insert(11);
        builder.serialize().unwrap()
    }

    #[test]
    fn round_trip_and_case_insensitive_style_match() {
        let bytes = serialized_catalog();
        let catalog = FontCatalog::parse(&bytes, 7).unwrap();
        assert_eq!(catalog.family_face_count(b"example sans"), 1);
        assert_eq!(catalog.match_style_index(b"EXAMPLE SANS", 400, 5, 0), Some(0));
        assert_eq!(catalog.faces[0].face_id, 11);
        assert_eq!(catalog.faces[0].ttc_index, 2);
    }

    #[test]
    fn rejects_truncation_and_wrong_generation() {
        let bytes = serialized_catalog();
        assert!(FontCatalog::parse(&bytes[..bytes.len() - 1], 7).is_none());
        assert!(FontCatalog::parse(&bytes, 8).is_none());
    }

    #[test]
    fn rejects_out_of_range_strings_and_duplicate_ids() {
        let mut bytes = serialized_catalog();
        let serialized_size = bytes.len() as u64;
        bytes[16..24].copy_from_slice(&serialized_size.to_le_bytes());
        bytes[56..64].copy_from_slice(&u64::MAX.to_le_bytes());
        assert!(FontCatalog::parse(&bytes, 7).is_none());

        let mut builder = FontCatalogBuilder {
            generation: 9,
            faces: Vec::new(),
            face_ids: HashSet::new(),
        };
        for family in [b"First" as &[u8], b"Second"] {
            builder.faces.push(BuilderFace {
                family: family.to_vec(),
                face_id: 3,
                ttc_index: 0,
                weight: 400,
                width: 5,
                slope: 0,
                format: 0,
            });
        }
        let bytes = builder.serialize().unwrap();
        assert!(FontCatalog::parse(&bytes, 9).is_none());
    }

    #[test]
    fn rejects_invalid_strings_and_face_fields() {
        let original = serialized_catalog();

        let mut invalid_utf8 = original.clone();
        invalid_utf8[HEADER_SIZE + RECORD_SIZE] = 0xff;
        assert!(FontCatalog::parse(&invalid_utf8, 7).is_none());

        let mut invalid_weight = original.clone();
        invalid_weight[72..74].copy_from_slice(&0_u16.to_le_bytes());
        assert!(FontCatalog::parse(&invalid_weight, 7).is_none());

        for (offset, value) in [(48, 0_u8), (76, 3), (77, 2), (78, 1)] {
            let mut bytes = original.clone();
            bytes[offset] = value;
            assert!(FontCatalog::parse(&bytes, 7).is_none());
        }
    }
}
