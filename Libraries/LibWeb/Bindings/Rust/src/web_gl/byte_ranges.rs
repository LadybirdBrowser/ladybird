/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Which bytes of script memory a WebGL operation means and which sizes it tells GL, as arithmetic on plain numbers
//! that the tests of the crate cover without the JS runtime. buffer_source.rs reads the numbers from script objects
//! and borrows the bytes.

use core::ops::Range;

/// Which GL error this is depends on the operation.
#[derive(Debug, PartialEq, Eq)]
pub(crate) struct OutOfRange;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct GlSize {
    size: usize,
    size_as_gl_sizei: i32,
}

impl GlSize {
    fn new(size: usize) -> Result<Self, OutOfRange> {
        let size_as_gl_sizei = i32::try_from(size).map_err(|_| OutOfRange)?;
        Ok(Self { size, size_as_gl_sizei })
    }

    pub(crate) fn get(self) -> usize {
        self.size
    }

    pub(crate) fn as_gl_sizei(self) -> i32 {
        self.size_as_gl_sizei
    }

    pub(crate) fn count_of_whole_groups_of(self, group_size: usize) -> Option<i32> {
        if !self.size.is_multiple_of(group_size) {
            return None;
        }
        i32::try_from(self.size.checked_div(group_size)?).ok()
    }
}

pub(crate) const fn gl_sizei_of_array_length(length: usize) -> i32 {
    assert!(length <= i32::MAX as usize);
    length as i32
}

#[derive(Clone, Copy, Debug)]
pub(crate) struct Extent {
    pub(crate) buffer_is_detached: bool,
    pub(crate) buffer_byte_length: usize,
    /// True for every view of a detached buffer as well. Such a view has no bytes.
    pub(crate) is_out_of_bounds: bool,
    pub(crate) byte_offset: usize,
    pub(crate) byte_length: usize,
    pub(crate) element_size: usize,
}

impl Extent {
    fn validate(&self) -> Result<(), OutOfRange> {
        if self.buffer_is_detached || self.is_out_of_bounds {
            return Err(OutOfRange);
        }
        let byte_end = self.byte_offset.checked_add(self.byte_length).ok_or(OutOfRange)?;
        if byte_end > self.buffer_byte_length {
            return Err(OutOfRange);
        }
        Ok(())
    }

    fn bytes_in_buffer(&self, bytes_of_source: &Range<usize>) -> Result<Range<usize>, OutOfRange> {
        let start = self.byte_offset.checked_add(bytes_of_source.start).ok_or(OutOfRange)?;
        let end = self.byte_offset.checked_add(bytes_of_source.end).ok_or(OutOfRange)?;
        Ok(start..end)
    }
}

/// `src_offset` and `src_length_override` count elements. The range is of the bytes of the source, not of its buffer.
fn source_range(extent: &Extent, src_offset: u64, src_length_override: u32) -> Result<Range<usize>, OutOfRange> {
    if extent.buffer_is_detached {
        return Err(OutOfRange);
    }
    if extent.is_out_of_bounds {
        if src_offset != 0 || src_length_override != 0 {
            return Err(OutOfRange);
        }
        return Ok(0..0);
    }

    let start = usize::try_from(src_offset)
        .ok()
        .and_then(|offset| offset.checked_mul(extent.element_size));
    let start = start.filter(|&start| start <= extent.byte_length).ok_or(OutOfRange)?;
    let mut length = extent.byte_length - start;
    if src_length_override != 0 {
        let requested_length = usize::try_from(src_length_override)
            .ok()
            .and_then(|length| length.checked_mul(extent.element_size));
        length = requested_length
            .filter(|&requested_length| requested_length <= length)
            .ok_or(OutOfRange)?;
    }

    let bytes_of_source = start..start + length;
    if extent.bytes_in_buffer(&bytes_of_source)?.end > extent.buffer_byte_length {
        return Err(OutOfRange);
    }
    Ok(bytes_of_source)
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct UploadRange {
    bytes_in_buffer: Range<usize>,
    length: GlSize,
    source_has_bytes: bool,
}

impl UploadRange {
    fn leading_bytes_of(extent: &Extent, source_range: &Range<usize>, length: usize) -> Result<Self, OutOfRange> {
        let length = GlSize::new(length)?;
        let end_of_leading_bytes = source_range.start.checked_add(length.get()).ok_or(OutOfRange)?;
        Ok(Self {
            bytes_in_buffer: extent.bytes_in_buffer(&(source_range.start..end_of_leading_bytes))?,
            length,
            source_has_bytes: !source_range.is_empty(),
        })
    }

    pub(crate) fn bytes_in_buffer(&self) -> Range<usize> {
        self.bytes_in_buffer.clone()
    }

    pub(crate) fn length(&self) -> GlSize {
        self.length
    }

    /// From the offset of the upload on, whether or not the upload records any of them.
    pub(crate) fn source_has_bytes(&self) -> bool {
        self.source_has_bytes
    }
}

pub(crate) fn upload_range(
    extent: &Extent,
    src_offset: u64,
    src_length_override: u32,
) -> Result<UploadRange, OutOfRange> {
    let source_range = source_range(extent, src_offset, src_length_override)?;
    UploadRange::leading_bytes_of(extent, &source_range, source_range.len())
}

/// The whole source when the size of the image is unknown or more than the source has, which leaves it to GL to say
/// what is wrong.
pub(crate) fn upload_range_of_2d_texture(
    extent: &Extent,
    src_offset: u64,
    size_that_the_image_takes: Option<usize>,
) -> Result<UploadRange, OutOfRange> {
    let source_range = source_range(extent, src_offset, 0)?;
    let length = match size_that_the_image_takes {
        Some(size) if size <= source_range.len() => size,
        _ => source_range.len(),
    };
    UploadRange::leading_bytes_of(extent, &source_range, length)
}

pub(crate) fn size_of_zero_filled_texture(
    bytes_per_pixel: usize,
    width: i32,
    height: i32,
) -> Result<GlSize, OutOfRange> {
    let width = usize::try_from(width).map_err(|_| OutOfRange)?;
    let height = usize::try_from(height).map_err(|_| OutOfRange)?;
    let size = bytes_per_pixel
        .checked_mul(width)
        .and_then(|size| size.checked_mul(height));
    GlSize::new(size.ok_or(OutOfRange)?)
}

pub(crate) fn destination_range(extent: &Extent, dst_offset: u64) -> Result<Range<usize>, OutOfRange> {
    extent.validate()?;
    source_range(extent, dst_offset, 0)
}

/// https://registry.khronos.org/webgl/specs/latest/2.0/#3.7.3
pub(crate) fn destination_range_of_get_buffer_sub_data(
    extent: &Extent,
    dst_offset: u64,
    length: u32,
) -> Result<Option<Range<usize>>, OutOfRange> {
    // If dstBuffer is a DataView, let elementSize be 1; otherwise, let elementSize be dstBuffer.BYTES_PER_ELEMENT.
    let element_size = extent.element_size;
    let dst_offset = usize::try_from(dst_offset).map_err(|_| OutOfRange)?;

    // If length is 0:
    let copy_length = if length == 0 {
        // If dstBuffer is a DataView, let copyLength be dstBuffer.byteLength - dstOffset; the typed elements in the
        // text below are bytes. Otherwise, let copyLength be dstBuffer.length - dstOffset.
        let destination_element_count = extent.byte_length.checked_div(element_size).ok_or(OutOfRange)?;
        destination_element_count.checked_sub(dst_offset).ok_or(OutOfRange)?
    }
    // Otherwise, let copyLength be length.
    else {
        usize::try_from(length).map_err(|_| OutOfRange)?
    };

    // If copyLength is 0, no data is written to dstBuffer, but this does not cause a GL error to be generated.
    if copy_length == 0 {
        return Ok(None);
    }

    // If dstOffset is greater than dstBuffer.length (or dstBuffer.byteLength in the case of DataView), generates an
    // INVALID_VALUE error.
    let start = dst_offset
        .checked_mul(element_size)
        .filter(|&start| start <= extent.byte_length)
        .ok_or(OutOfRange)?;

    // If dstOffset + copyLength is greater than dstBuffer.length (or dstBuffer.byteLength in the case of DataView),
    // generates an INVALID_VALUE error.
    let end = copy_length
        .checked_mul(element_size)
        .and_then(|copy_byte_length| start.checked_add(copy_byte_length))
        .filter(|&end| end <= extent.byte_length)
        .ok_or(OutOfRange)?;
    Ok(Some(start..end))
}

/// `starting_offset` counts bytes. No bytes go nowhere, even to a view that is detached or out of bounds.
pub(crate) fn offset_in_buffer_of_checked_write(
    extent: &Extent,
    byte_count: usize,
    starting_offset: usize,
) -> Result<Option<usize>, OutOfRange> {
    if starting_offset > extent.byte_length || byte_count > extent.byte_length - starting_offset {
        return Err(OutOfRange);
    }
    // NB: The elements of a DataView are bytes.
    if !byte_count.is_multiple_of(extent.element_size) {
        return Err(OutOfRange);
    }
    if byte_count == 0 {
        return Ok(None);
    }

    extent.validate()?;
    let write_offset = extent.byte_offset.checked_add(starting_offset).ok_or(OutOfRange)?;
    if write_offset > extent.buffer_byte_length || byte_count > extent.buffer_byte_length - write_offset {
        return Err(OutOfRange);
    }
    Ok(Some(write_offset))
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ListRange {
    pub(crate) elements: Range<usize>,
    bytes: Range<usize>,
    pub(crate) element_count: GlSize,
}

fn elements_of_list(list_length: usize, src_offset: u64, src_length_override: u32) -> Result<Range<usize>, OutOfRange> {
    let first_element = usize::try_from(src_offset).map_err(|_| OutOfRange)?;
    let src_length_override = usize::try_from(src_length_override).map_err(|_| OutOfRange)?;
    let end_of_override = first_element.checked_add(src_length_override).ok_or(OutOfRange)?;
    if end_of_override > list_length {
        return Err(OutOfRange);
    }
    Ok(if src_length_override == 0 {
        first_element..list_length
    } else {
        first_element..end_of_override
    })
}

/// A typed array that is detached or out of bounds holds no numbers.
fn typed_array_list_length(extent: &Extent) -> Result<usize, OutOfRange> {
    extent.byte_length.checked_div(extent.element_size).ok_or(OutOfRange)
}

pub(crate) fn list_range(
    list_length: usize,
    element_size: usize,
    src_offset: u64,
    src_length_override: u32,
) -> Result<ListRange, OutOfRange> {
    let elements = elements_of_list(list_length, src_offset, src_length_override)?;

    let first_byte = elements.start.checked_mul(element_size).ok_or(OutOfRange)?;
    let byte_length = GlSize::new(elements.len().checked_mul(element_size).ok_or(OutOfRange)?)?;
    let end_byte = first_byte.checked_add(byte_length.get()).ok_or(OutOfRange)?;
    Ok(ListRange {
        element_count: GlSize::new(elements.len())?,
        elements,
        bytes: first_byte..end_byte,
    })
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct TypedArrayListRange {
    pub(crate) bytes_in_buffer: Range<usize>,
    pub(crate) element_count: GlSize,
}

pub(crate) fn typed_array_list_range(
    extent: &Extent,
    src_offset: u64,
    src_length_override: u32,
) -> Result<TypedArrayListRange, OutOfRange> {
    let range = list_range(
        typed_array_list_length(extent)?,
        extent.element_size,
        src_offset,
        src_length_override,
    )?;
    Ok(TypedArrayListRange {
        bytes_in_buffer: extent.bytes_in_buffer(&range.bytes)?,
        element_count: range.element_count,
    })
}

/// The elements of a list from an offset on, for an operation that records only the first few of them. GL is not
/// told how many elements the list has, so unlike a ListRange this one is not bounded to what a GLsizei counts.
#[derive(Clone, Debug, PartialEq, Eq)]
pub(crate) struct ListTail {
    elements: Range<usize>,
    element_size: usize,
    /// In its buffer for a typed array, and 0 for a sequence.
    byte_offset_of_list: usize,
}

impl ListTail {
    fn new(elements: Range<usize>, element_size: usize, byte_offset_of_list: usize) -> Result<Self, OutOfRange> {
        // NB: With the end of the tail counted once, the bytes of its leading elements are counted without overflow.
        elements
            .end
            .checked_mul(element_size)
            .and_then(|end| end.checked_add(byte_offset_of_list))
            .ok_or(OutOfRange)?;
        Ok(Self {
            elements,
            element_size,
            byte_offset_of_list,
        })
    }

    /// `None` when the list has fewer elements from the offset on.
    pub(crate) fn bytes_of_leading_elements(&self, count: usize) -> Option<Range<usize>> {
        if count > self.elements.len() {
            return None;
        }
        let start = self.byte_offset_of_list + self.elements.start * self.element_size;
        Some(start..start + count * self.element_size)
    }
}

/// The bytes are those of the sequence.
pub(crate) fn sequence_list_tail(
    list_length: usize,
    element_size: usize,
    src_offset: u64,
) -> Result<ListTail, OutOfRange> {
    ListTail::new(elements_of_list(list_length, src_offset, 0)?, element_size, 0)
}

/// The bytes are those of the buffer of the typed array.
pub(crate) fn typed_array_list_tail(extent: &Extent, src_offset: u64) -> Result<ListTail, OutOfRange> {
    ListTail::new(
        elements_of_list(typed_array_list_length(extent)?, src_offset, 0)?,
        extent.element_size,
        extent.byte_offset,
    )
}

#[cfg(test)]
mod tests {
    use super::*;

    const I32_MAX: usize = i32::MAX as usize;

    fn array_buffer(byte_length: usize) -> Extent {
        Extent {
            buffer_is_detached: false,
            buffer_byte_length: byte_length,
            is_out_of_bounds: false,
            byte_offset: 0,
            byte_length,
            element_size: 1,
        }
    }

    fn view(buffer_byte_length: usize, byte_offset: usize, element_count: usize, element_size: usize) -> Extent {
        Extent {
            buffer_is_detached: false,
            buffer_byte_length,
            is_out_of_bounds: false,
            byte_offset,
            byte_length: element_count * element_size,
            element_size,
        }
    }

    fn out_of_bounds(view: Extent, buffer_byte_length: usize) -> Extent {
        Extent {
            buffer_byte_length,
            is_out_of_bounds: true,
            byte_length: 0,
            ..view
        }
    }

    fn detached_array_buffer() -> Extent {
        Extent {
            buffer_is_detached: true,
            ..array_buffer(0)
        }
    }

    fn detached_view(view: Extent) -> Extent {
        Extent {
            buffer_is_detached: true,
            ..out_of_bounds(view, 0)
        }
    }

    fn uploaded(bytes_in_buffer: Range<usize>, source_has_bytes: bool) -> Result<UploadRange, OutOfRange> {
        Ok(UploadRange {
            length: GlSize::new(bytes_in_buffer.len()).unwrap(),
            bytes_in_buffer,
            source_has_bytes,
        })
    }

    #[test]
    fn a_gl_size_counts_what_a_glsizei_counts() {
        assert_eq!(GlSize::new(0).unwrap().as_gl_sizei(), 0);
        assert_eq!(GlSize::new(I32_MAX).unwrap().as_gl_sizei(), i32::MAX);
        assert_eq!(GlSize::new(I32_MAX).unwrap().get(), I32_MAX);
        assert_eq!(GlSize::new(I32_MAX + 1), Err(OutOfRange));
        assert_eq!(GlSize::new(usize::MAX), Err(OutOfRange));

        assert_eq!(const { gl_sizei_of_array_length(0) }, 0);
        assert_eq!(const { gl_sizei_of_array_length(256) }, 256);
        assert_eq!(const { gl_sizei_of_array_length(I32_MAX) }, i32::MAX);
    }

    #[test]
    fn elements_are_counted_in_groups_when_they_are_whole_groups() {
        let count =
            |element_count: usize, group_size| GlSize::new(element_count).unwrap().count_of_whole_groups_of(group_size);
        assert_eq!(count(0, 4), Some(0));
        assert_eq!(count(16, 1), Some(16));
        assert_eq!(count(16, 4), Some(4));
        assert_eq!(count(16, 16), Some(1));
        assert_eq!(count(32, 16), Some(2));
        assert_eq!(count(15, 4), None);
        assert_eq!(count(17, 16), None);
        assert_eq!(count(3, 4), None);
        assert_eq!(count(I32_MAX, 1), Some(i32::MAX));
        assert_eq!(count(I32_MAX - 1, 2), Some(i32::MAX / 2));
        assert_eq!(count(0, 0), None);
        assert_eq!(count(4, 0), None);
    }

    #[test]
    fn the_source_range_of_a_buffer_is_its_bytes_from_an_offset_on() {
        let buffer = array_buffer(16);
        assert_eq!(source_range(&buffer, 0, 0), Ok(0..16));
        assert_eq!(source_range(&buffer, 4, 0), Ok(4..16));
        assert_eq!(source_range(&buffer, 4, 8), Ok(4..12));
        assert_eq!(source_range(&buffer, 4, 12), Ok(4..16));
        assert_eq!(source_range(&buffer, 0, 16), Ok(0..16));
        assert_eq!(source_range(&array_buffer(0), 0, 0), Ok(0..0));
    }

    #[test]
    fn the_source_range_of_a_view_counts_its_elements() {
        // A Uint16Array of two elements at byte 4 of a buffer of 16 bytes.
        let uint16_array = view(16, 4, 2, 2);
        assert_eq!(source_range(&uint16_array, 0, 0), Ok(0..4));
        assert_eq!(source_range(&uint16_array, 1, 0), Ok(2..4));
        assert_eq!(source_range(&uint16_array, 0, 1), Ok(0..2));
        assert_eq!(source_range(&uint16_array, 1, 1), Ok(2..4));
        assert_eq!(source_range(&uint16_array, 3, 0), Err(OutOfRange));

        // A DataView of four bytes at byte 8 of the same buffer.
        let data_view = view(16, 8, 4, 1);
        assert_eq!(source_range(&data_view, 2, 2), Ok(2..4));
        assert_eq!(source_range(&data_view, 2, 0), Ok(2..4));
        assert_eq!(source_range(&data_view, 2, 3), Err(OutOfRange));

        let float64_array = view(64, 8, 4, 8);
        assert_eq!(source_range(&float64_array, 1, 2), Ok(8..24));
        assert_eq!(source_range(&float64_array, 3, 0), Ok(24..32));
        assert_eq!(source_range(&float64_array, 3, 2), Err(OutOfRange));
    }

    #[test]
    fn an_upload_takes_the_bytes_of_a_view_from_where_the_view_is_in_its_buffer() {
        let buffer: Vec<u8> = (0x10..0x20).collect();
        let uploaded_bytes = |extent: &Extent, src_offset, src_length_override| {
            upload_range(extent, src_offset, src_length_override).map(|range| buffer[range.bytes_in_buffer()].to_vec())
        };

        // A Uint16Array of two elements at byte 4 of the buffer.
        let uint16_array = view(16, 4, 2, 2);
        assert_eq!(uploaded_bytes(&uint16_array, 1, 0), Ok(vec![0x16, 0x17]));
        assert_eq!(uploaded_bytes(&uint16_array, 0, 0), Ok(vec![0x14, 0x15, 0x16, 0x17]));
        assert_eq!(uploaded_bytes(&uint16_array, 0, 1), Ok(vec![0x14, 0x15]));
        assert_eq!(uploaded_bytes(&uint16_array, 2, 0), Ok(vec![]));
        assert_eq!(uploaded_bytes(&uint16_array, 3, 0), Err(OutOfRange));

        // A DataView of four bytes at byte 8 of the buffer.
        let data_view = view(16, 8, 4, 1);
        assert_eq!(uploaded_bytes(&data_view, 2, 2), Ok(vec![0x1a, 0x1b]));
        assert_eq!(uploaded_bytes(&data_view, 1, 0), Ok(vec![0x19, 0x1a, 0x1b]));

        let image_bytes = |extent: &Extent, src_offset, size_that_the_image_takes| {
            upload_range_of_2d_texture(extent, src_offset, Some(size_that_the_image_takes))
                .map(|range| buffer[range.bytes_in_buffer()].to_vec())
        };
        assert_eq!(image_bytes(&uint16_array, 1, 1), Ok(vec![0x16]));
        assert_eq!(image_bytes(&data_view, 1, 2), Ok(vec![0x19, 0x1a]));
        assert_eq!(image_bytes(&data_view, 3, 2), Ok(vec![0x1b]));
    }

    #[test]
    fn an_offset_at_the_end_of_a_source_leaves_no_bytes_and_one_past_it_is_out_of_range() {
        let buffer = array_buffer(16);
        assert_eq!(source_range(&buffer, 16, 0), Ok(16..16));
        assert_eq!(source_range(&buffer, 17, 0), Err(OutOfRange));
        assert_eq!(source_range(&buffer, 16, 1), Err(OutOfRange));

        let uint32_array = view(64, 16, 4, 4);
        assert_eq!(source_range(&uint32_array, 4, 0), Ok(16..16));
        assert_eq!(source_range(&uint32_array, 5, 0), Err(OutOfRange));
        assert_eq!(source_range(&uint32_array, 4, 1), Err(OutOfRange));
    }

    #[test]
    fn a_length_override_past_the_end_of_a_source_is_out_of_range() {
        let buffer = array_buffer(16);
        assert_eq!(source_range(&buffer, 0, 17), Err(OutOfRange));
        assert_eq!(source_range(&buffer, 8, 9), Err(OutOfRange));
        assert_eq!(source_range(&buffer, 0, u32::MAX), Err(OutOfRange));

        let uint32_array = view(64, 16, 4, 4);
        assert_eq!(source_range(&uint32_array, 0, 4), Ok(0..16));
        assert_eq!(source_range(&uint32_array, 0, 5), Err(OutOfRange));
        assert_eq!(source_range(&uint32_array, 3, 2), Err(OutOfRange));
        assert_eq!(source_range(&uint32_array, 0, u32::MAX), Err(OutOfRange));
    }

    #[test]
    fn offsets_and_lengths_that_overflow_are_out_of_range() {
        let float64_array = view(64, 8, 4, 8);
        assert_eq!(source_range(&float64_array, u64::MAX, 0), Err(OutOfRange));
        assert_eq!(source_range(&float64_array, 1 << 61, 0), Err(OutOfRange));
        assert_eq!(source_range(&float64_array, (1 << 61) - 1, 0), Err(OutOfRange));
        assert_eq!(source_range(&float64_array, u64::MAX, u32::MAX), Err(OutOfRange));
        assert_eq!(source_range(&array_buffer(16), u64::MAX, 0), Err(OutOfRange));

        // NB: No view says this of itself. It is here for the additions, which do not rely on that.
        let view_at_the_end_of_memory = Extent {
            byte_offset: usize::MAX - 2,
            ..view(usize::MAX, 0, 4, 1)
        };
        assert_eq!(source_range(&view_at_the_end_of_memory, 0, 0), Err(OutOfRange));
        assert_eq!(source_range(&view_at_the_end_of_memory, 3, 0), Err(OutOfRange));
        assert_eq!(source_range(&view_at_the_end_of_memory, 4, 0), Err(OutOfRange));
        assert_eq!(
            upload_range(&view_at_the_end_of_memory, 0, 2),
            uploaded(usize::MAX - 2..usize::MAX, true)
        );
        assert_eq!(destination_range(&view_at_the_end_of_memory, 0), Err(OutOfRange));
        assert_eq!(
            typed_array_list_range(&view_at_the_end_of_memory, 0, 0),
            Err(OutOfRange)
        );
        assert_eq!(
            typed_array_list_range(&view_at_the_end_of_memory, 3, 0),
            Err(OutOfRange)
        );
        assert_eq!(
            typed_array_list_range(&view_at_the_end_of_memory, 0, 2).map(|range| range.bytes_in_buffer),
            Ok(usize::MAX - 2..usize::MAX)
        );
        assert_eq!(
            offset_in_buffer_of_checked_write(&view_at_the_end_of_memory, 1, 3),
            Err(OutOfRange)
        );
        assert_eq!(typed_array_list_tail(&view_at_the_end_of_memory, 0), Err(OutOfRange));
        assert_eq!(typed_array_list_tail(&view_at_the_end_of_memory, 2), Err(OutOfRange));
        assert_eq!(sequence_list_tail(usize::MAX, 4, 0), Err(OutOfRange));
        assert_eq!(
            sequence_list_tail(usize::MAX / 4, 4, 0).map(|tail| tail.bytes_of_leading_elements(1)),
            Ok(Some(0..4))
        );
    }

    #[test]
    fn a_view_that_its_buffer_is_too_short_for_has_no_source_range() {
        // NB: A view that is not out of bounds does not say this of itself either.
        let view_past_its_buffer = view(8, 4, 8, 1);
        assert_eq!(source_range(&view_past_its_buffer, 0, 0), Err(OutOfRange));
        assert_eq!(source_range(&view_past_its_buffer, 0, 4), Ok(0..4));
        assert_eq!(source_range(&view_past_its_buffer, 4, 0), Err(OutOfRange));
        assert_eq!(source_range(&view_past_its_buffer, 5, 0), Err(OutOfRange));
        assert_eq!(destination_range(&view_past_its_buffer, 0), Err(OutOfRange));
    }

    #[test]
    fn an_out_of_bounds_source_has_no_bytes_from_no_offset_on() {
        // A Uint8Array and a DataView of four bytes at byte 8 of a buffer that was resized from 16 bytes to 4.
        let view_of_bytes = out_of_bounds(view(16, 8, 4, 1), 4);
        assert_eq!(source_range(&view_of_bytes, 0, 0), Ok(0..0));
        assert_eq!(source_range(&view_of_bytes, 1, 0), Err(OutOfRange));
        assert_eq!(source_range(&view_of_bytes, 0, 1), Err(OutOfRange));
        assert_eq!(upload_range(&view_of_bytes, 0, 0), uploaded(8..8, false));

        let uint16_array = out_of_bounds(view(16, 4, 4, 2), 8);
        assert_eq!(source_range(&uint16_array, 0, 0), Ok(0..0));
        assert_eq!(source_range(&uint16_array, 1, 0), Err(OutOfRange));
        assert_eq!(source_range(&uint16_array, 0, 1), Err(OutOfRange));
    }

    #[test]
    fn a_detached_source_has_no_source_range() {
        for source in [
            detached_array_buffer(),
            detached_view(view(16, 4, 2, 2)),
            detached_view(view(16, 0, 16, 1)),
        ] {
            assert_eq!(source_range(&source, 0, 0), Err(OutOfRange));
            assert_eq!(source_range(&source, 1, 0), Err(OutOfRange));
            assert_eq!(source_range(&source, 0, 1), Err(OutOfRange));
            assert_eq!(upload_range(&source, 0, 0), Err(OutOfRange));
            assert_eq!(upload_range_of_2d_texture(&source, 0, Some(0)), Err(OutOfRange));
        }
    }

    #[test]
    fn an_upload_records_no_more_bytes_than_gl_can_count() {
        let largest_upload = array_buffer(I32_MAX);
        assert_eq!(upload_range(&largest_upload, 0, 0), uploaded(0..I32_MAX, true));
        assert_eq!(
            upload_range(&largest_upload, 0, 0).unwrap().length().as_gl_sizei(),
            i32::MAX
        );

        let two_gib = array_buffer(I32_MAX + 1);
        assert_eq!(upload_range(&two_gib, 0, 0), Err(OutOfRange));
        assert_eq!(upload_range(&two_gib, 1, 0), uploaded(1..I32_MAX + 1, true));
        assert_eq!(upload_range(&two_gib, 0, 64), uploaded(0..64, true));
        assert_eq!(
            upload_range(&two_gib, 1 << 31, 0),
            uploaded(I32_MAX + 1..I32_MAX + 1, false)
        );

        let four_gib_of_floats = view(1 << 32, 0, 1 << 30, 4);
        assert_eq!(upload_range(&four_gib_of_floats, 0, 0), Err(OutOfRange));
        assert_eq!(upload_range(&four_gib_of_floats, 0, 1 << 29), Err(OutOfRange));
        assert_eq!(
            upload_range(&four_gib_of_floats, 0, (1 << 29) - 1),
            uploaded(0..I32_MAX - 3, true)
        );
        assert_eq!(
            upload_range(&four_gib_of_floats, (1 << 30) - 16, 0),
            uploaded((1 << 32) - 64..1 << 32, true)
        );
    }

    #[test]
    fn an_upload_tells_a_source_without_bytes_from_one_that_it_records_no_bytes_of() {
        let buffer = array_buffer(16);
        assert_eq!(upload_range(&buffer, 4, 0), uploaded(4..16, true));
        assert_eq!(upload_range(&buffer, 4, 0).unwrap().bytes_in_buffer(), 4..16);
        assert!(upload_range(&buffer, 4, 0).unwrap().source_has_bytes());
        assert_eq!(upload_range(&buffer, 16, 0), uploaded(16..16, false));
        assert_eq!(upload_range(&array_buffer(0), 0, 0), uploaded(0..0, false));

        assert_eq!(upload_range_of_2d_texture(&buffer, 4, Some(0)), uploaded(4..4, true));
        assert_eq!(
            upload_range_of_2d_texture(&buffer, 16, Some(0)),
            uploaded(16..16, false)
        );
        assert_eq!(
            upload_range_of_2d_texture(&buffer, 16, Some(4)),
            uploaded(16..16, false)
        );
        assert_eq!(upload_range_of_2d_texture(&buffer, 16, None), uploaded(16..16, false));
        assert_eq!(
            upload_range_of_2d_texture(&array_buffer(0), 0, Some(0)),
            uploaded(0..0, false)
        );
    }

    #[test]
    fn an_upload_of_a_2d_image_records_the_bytes_that_the_image_takes() {
        let buffer = array_buffer(64);
        assert_eq!(upload_range_of_2d_texture(&buffer, 0, Some(16)), uploaded(0..16, true));
        assert_eq!(upload_range_of_2d_texture(&buffer, 8, Some(16)), uploaded(8..24, true));
        assert_eq!(upload_range_of_2d_texture(&buffer, 0, Some(64)), uploaded(0..64, true));
        assert_eq!(
            upload_range_of_2d_texture(&buffer, 48, Some(16)),
            uploaded(48..64, true)
        );

        // A source that is too short for the image is recorded whole, and so is one of an image of unknown size.
        assert_eq!(upload_range_of_2d_texture(&buffer, 0, Some(65)), uploaded(0..64, true));
        assert_eq!(
            upload_range_of_2d_texture(&buffer, 56, Some(16)),
            uploaded(56..64, true)
        );
        assert_eq!(
            upload_range_of_2d_texture(&buffer, 0, Some(usize::MAX)),
            uploaded(0..64, true)
        );
        assert_eq!(upload_range_of_2d_texture(&buffer, 8, None), uploaded(8..64, true));

        assert_eq!(upload_range_of_2d_texture(&buffer, 65, Some(0)), Err(OutOfRange));

        let uint16_array = view(64, 8, 16, 2);
        assert_eq!(
            upload_range_of_2d_texture(&uint16_array, 2, Some(6)),
            uploaded(12..18, true)
        );
        assert_eq!(
            upload_range_of_2d_texture(&uint16_array, 14, Some(6)),
            uploaded(36..40, true)
        );
    }

    #[test]
    fn an_upload_of_a_2d_image_from_a_large_source_is_bounded_by_what_it_records() {
        // NB: This is the heap of a program that was compiled to WebAssembly, which uploads from where its pixels
        //     are in it.
        let four_gib_heap = array_buffer(1 << 32);
        assert_eq!(
            upload_range_of_2d_texture(&four_gib_heap, 1 << 20, Some(4096)),
            uploaded(1 << 20..(1 << 20) + 4096, true)
        );
        assert_eq!(
            upload_range_of_2d_texture(&four_gib_heap, 0, Some(I32_MAX)),
            uploaded(0..I32_MAX, true)
        );
        assert_eq!(
            upload_range_of_2d_texture(&four_gib_heap, 0, Some(I32_MAX + 1)),
            Err(OutOfRange)
        );
        assert_eq!(upload_range_of_2d_texture(&four_gib_heap, 0, None), Err(OutOfRange));
        assert_eq!(
            upload_range_of_2d_texture(&four_gib_heap, 16, Some(usize::MAX)),
            Err(OutOfRange)
        );
        assert_eq!(
            upload_range_of_2d_texture(&four_gib_heap, (1 << 32) - 16, None),
            uploaded((1 << 32) - 16..1 << 32, true)
        );
    }

    #[test]
    fn a_texture_without_pixels_is_filled_with_as_many_zeros_as_gl_can_count() {
        let size = |bytes_per_pixel, width, height| {
            size_of_zero_filled_texture(bytes_per_pixel, width, height).map(GlSize::as_gl_sizei)
        };
        assert_eq!(size(4, 256, 256), Ok(4 * 256 * 256));
        assert_eq!(size(1, 1, 1), Ok(1));
        assert_eq!(size(3, 5, 7), Ok(105));
        assert_eq!(size(4, 0, 16), Ok(0));
        assert_eq!(size(4, 16, 0), Ok(0));
        assert_eq!(size(4, 0, 0), Ok(0));
        assert_eq!(size(1, i32::MAX, 1), Ok(i32::MAX));
        assert_eq!(size(1, 1, i32::MAX), Ok(i32::MAX));
        assert_eq!(size(4, 16384, 16384), Ok(1 << 30));

        assert_eq!(size(1, -1, 1), Err(OutOfRange));
        assert_eq!(size(1, 1, -1), Err(OutOfRange));
        assert_eq!(size(2, -1, -1), Err(OutOfRange));
        assert_eq!(size(4, i32::MIN, 1), Err(OutOfRange));
        assert_eq!(size(4, 0, -1), Err(OutOfRange));
        assert_eq!(size(4, -1, 0), Err(OutOfRange));

        assert_eq!(size(4, 32768, 32768), Err(OutOfRange));
        assert_eq!(size(2, i32::MAX, 1), Err(OutOfRange));
        assert_eq!(size(1, 1 << 16, 1 << 15), Err(OutOfRange));
        assert_eq!(size(4, i32::MAX, i32::MAX), Err(OutOfRange));
        assert_eq!(size(usize::MAX, 2, 1), Err(OutOfRange));
        assert_eq!(size(usize::MAX, i32::MAX, i32::MAX), Err(OutOfRange));
    }

    #[test]
    fn a_read_back_writes_to_the_bytes_of_a_valid_view_from_an_offset_on() {
        let uint16_array = view(16, 4, 4, 2);
        assert_eq!(destination_range(&uint16_array, 0), Ok(0..8));
        assert_eq!(destination_range(&uint16_array, 3), Ok(6..8));
        assert_eq!(destination_range(&uint16_array, 4), Ok(8..8));
        assert_eq!(destination_range(&uint16_array, 5), Err(OutOfRange));
        assert_eq!(destination_range(&uint16_array, u64::MAX), Err(OutOfRange));

        let data_view = view(16, 8, 4, 1);
        assert_eq!(destination_range(&data_view, 1), Ok(1..4));

        // NB: A source of an upload that is out of bounds is one without bytes. A destination has to be valid.
        let out_of_bounds_view = out_of_bounds(view(16, 8, 4, 1), 4);
        assert_eq!(destination_range(&out_of_bounds_view, 0), Err(OutOfRange));
        assert_eq!(destination_range(&detached_view(uint16_array), 0), Err(OutOfRange));

        // NB: How a destination of more bytes than a GLsizei counts fails depends on the operation.
        let two_gib_view = view(I32_MAX + 1, 0, I32_MAX + 1, 1);
        assert_eq!(destination_range(&two_gib_view, 0), Ok(0..I32_MAX + 1));
    }

    #[test]
    fn get_buffer_sub_data_copies_to_the_elements_of_the_view_from_an_offset_on() {
        let range = destination_range_of_get_buffer_sub_data;
        let uint16_array = view(64, 8, 8, 2);
        assert_eq!(range(&uint16_array, 0, 0), Ok(Some(0..16)));
        assert_eq!(range(&uint16_array, 3, 0), Ok(Some(6..16)));
        assert_eq!(range(&uint16_array, 3, 2), Ok(Some(6..10)));
        assert_eq!(range(&uint16_array, 0, 8), Ok(Some(0..16)));
        assert_eq!(range(&uint16_array, 7, 1), Ok(Some(14..16)));

        let data_view = view(64, 8, 16, 1);
        assert_eq!(range(&data_view, 0, 0), Ok(Some(0..16)));
        assert_eq!(range(&data_view, 5, 0), Ok(Some(5..16)));
        assert_eq!(range(&data_view, 5, 11), Ok(Some(5..16)));
    }

    #[test]
    fn get_buffer_sub_data_copies_nothing_when_the_copy_length_is_zero() {
        let range = destination_range_of_get_buffer_sub_data;
        let uint16_array = view(64, 8, 8, 2);
        assert_eq!(range(&uint16_array, 8, 0), Ok(None));
        assert_eq!(range(&view(64, 8, 0, 2), 0, 0), Ok(None));
        assert_eq!(range(&out_of_bounds(uint16_array, 4), 0, 0), Ok(None));
        assert_eq!(range(&detached_view(uint16_array), 0, 0), Ok(None));
    }

    #[test]
    fn get_buffer_sub_data_does_not_copy_past_the_end_of_the_view() {
        let range = destination_range_of_get_buffer_sub_data;
        let uint16_array = view(64, 8, 8, 2);
        assert_eq!(range(&uint16_array, 9, 0), Err(OutOfRange));
        assert_eq!(range(&uint16_array, 0, 9), Err(OutOfRange));
        assert_eq!(range(&uint16_array, 7, 2), Err(OutOfRange));
        assert_eq!(range(&uint16_array, 8, 1), Err(OutOfRange));
        assert_eq!(range(&uint16_array, 9, 1), Err(OutOfRange));
        assert_eq!(range(&uint16_array, u64::MAX, 0), Err(OutOfRange));
        assert_eq!(range(&uint16_array, u64::MAX, 1), Err(OutOfRange));
        assert_eq!(range(&uint16_array, u64::MAX / 2 + 1, 1), Err(OutOfRange));
        assert_eq!(range(&uint16_array, 0, u32::MAX), Err(OutOfRange));
        assert_eq!(range(&view(64, 0, 8, 8), 1, u32::MAX), Err(OutOfRange));

        let out_of_bounds_view = out_of_bounds(uint16_array, 4);
        assert_eq!(range(&out_of_bounds_view, 1, 0), Err(OutOfRange));
        assert_eq!(range(&out_of_bounds_view, 0, 1), Err(OutOfRange));
        assert_eq!(range(&detached_view(uint16_array), 0, 1), Err(OutOfRange));
    }

    #[test]
    fn a_checked_write_goes_to_where_the_view_is_in_its_buffer() {
        let offset = offset_in_buffer_of_checked_write;
        // A Uint16Array of two elements at byte 4 of a buffer of 16 bytes, and a DataView of four bytes at byte 8.
        let uint16_array = view(16, 4, 2, 2);
        let data_view = view(16, 8, 4, 1);
        assert_eq!(offset(&uint16_array, 2, 2), Ok(Some(6)));
        assert_eq!(offset(&uint16_array, 4, 0), Ok(Some(4)));
        assert_eq!(offset(&data_view, 2, 1), Ok(Some(9)));
        assert_eq!(offset(&data_view, 4, 0), Ok(Some(8)));
        assert_eq!(offset(&data_view, 1, 3), Ok(Some(11)));
    }

    #[test]
    fn a_checked_write_needs_room_in_the_view() {
        let offset = offset_in_buffer_of_checked_write;
        let uint16_array = view(16, 4, 2, 2);
        let data_view = view(16, 8, 4, 1);
        assert_eq!(offset(&uint16_array, 6, 0), Err(OutOfRange));
        assert_eq!(offset(&uint16_array, 4, 2), Err(OutOfRange));
        assert_eq!(offset(&uint16_array, 2, 4), Err(OutOfRange));
        assert_eq!(offset(&uint16_array, 0, 5), Err(OutOfRange));
        assert_eq!(offset(&data_view, 5, 0), Err(OutOfRange));
        assert_eq!(offset(&data_view, 1, 4), Err(OutOfRange));
        assert_eq!(offset(&data_view, usize::MAX, 0), Err(OutOfRange));
        assert_eq!(offset(&data_view, 1, usize::MAX), Err(OutOfRange));
        assert_eq!(offset(&data_view, usize::MAX, usize::MAX), Err(OutOfRange));
    }

    #[test]
    fn a_checked_write_to_a_typed_array_is_of_whole_elements() {
        let offset = offset_in_buffer_of_checked_write;
        let uint16_array = view(16, 4, 2, 2);
        assert_eq!(offset(&uint16_array, 1, 0), Err(OutOfRange));
        assert_eq!(offset(&uint16_array, 3, 0), Err(OutOfRange));
        // NB: Only the number of bytes has to be whole elements. Where they go is counted in bytes.
        assert_eq!(offset(&uint16_array, 2, 1), Ok(Some(5)));

        let float32_array = view(64, 16, 4, 4);
        assert_eq!(offset(&float32_array, 4, 4), Ok(Some(20)));
        assert_eq!(offset(&float32_array, 8, 8), Ok(Some(24)));
        assert_eq!(offset(&float32_array, 16, 0), Ok(Some(16)));
        assert_eq!(offset(&float32_array, 2, 0), Err(OutOfRange));
        assert_eq!(offset(&float32_array, 6, 4), Err(OutOfRange));

        // The elements of a DataView are bytes.
        let data_view = view(16, 8, 4, 1);
        assert_eq!(offset(&data_view, 1, 0), Ok(Some(8)));
        assert_eq!(offset(&data_view, 3, 1), Ok(Some(9)));
    }

    #[test]
    fn a_checked_write_of_no_bytes_goes_nowhere() {
        let offset = offset_in_buffer_of_checked_write;
        let uint16_array = view(16, 4, 2, 2);
        assert_eq!(offset(&uint16_array, 0, 0), Ok(None));
        assert_eq!(offset(&uint16_array, 0, 4), Ok(None));
        assert_eq!(offset(&uint16_array, 0, 3), Ok(None));
    }

    #[test]
    fn a_checked_write_to_a_view_that_is_out_of_bounds_or_detached_is_of_no_bytes() {
        let offset = offset_in_buffer_of_checked_write;
        for view in [
            out_of_bounds(view(16, 8, 4, 1), 4),
            out_of_bounds(view(16, 8, 2, 2), 4),
            detached_view(view(4, 0, 2, 2)),
            detached_view(view(4, 0, 4, 1)),
        ] {
            assert_eq!(offset(&view, 0, 0), Ok(None));
            assert_eq!(offset(&view, 0, 1), Err(OutOfRange));
            assert_eq!(offset(&view, 1, 0), Err(OutOfRange));
            assert_eq!(offset(&view, 2, 0), Err(OutOfRange));
        }
    }

    #[test]
    fn a_list_is_its_numbers_from_an_offset_on() {
        let list = |list_length, src_offset, src_length_override| {
            list_range(list_length, 4, src_offset, src_length_override)
                .map(|range| (range.elements, range.bytes, range.element_count.as_gl_sizei()))
        };
        assert_eq!(list(8, 0, 0), Ok((0..8, 0..32, 8)));
        assert_eq!(list(8, 3, 0), Ok((3..8, 12..32, 5)));
        assert_eq!(list(8, 3, 2), Ok((3..5, 12..20, 2)));
        assert_eq!(list(8, 0, 8), Ok((0..8, 0..32, 8)));
        assert_eq!(list(8, 7, 1), Ok((7..8, 28..32, 1)));
        assert_eq!(list(8, 8, 0), Ok((8..8, 32..32, 0)));
        assert_eq!(list(0, 0, 0), Ok((0..0, 0..0, 0)));

        assert_eq!(list(8, 9, 0), Err(OutOfRange));
        assert_eq!(list(8, 0, 9), Err(OutOfRange));
        assert_eq!(list(8, 7, 2), Err(OutOfRange));
        assert_eq!(list(8, 8, 1), Err(OutOfRange));
        assert_eq!(list(0, 1, 0), Err(OutOfRange));
        assert_eq!(list(0, 0, 1), Err(OutOfRange));
        assert_eq!(list(8, u64::MAX, 0), Err(OutOfRange));
        assert_eq!(list(8, u64::MAX, 1), Err(OutOfRange));
        assert_eq!(list(8, u64::MAX, u32::MAX), Err(OutOfRange));
        assert_eq!(list(8, 0, u32::MAX), Err(OutOfRange));
    }

    #[test]
    fn a_list_is_no_more_bytes_than_gl_can_count() {
        let element_count_of = |list_length, element_size, src_offset, src_length_override| {
            list_range(list_length, element_size, src_offset, src_length_override)
                .map(|range| range.element_count.as_gl_sizei())
        };
        let largest_list_of_floats = I32_MAX / 4;
        assert_eq!(element_count_of(largest_list_of_floats, 4, 0, 0), Ok(i32::MAX / 4));
        assert_eq!(element_count_of(largest_list_of_floats + 1, 4, 0, 0), Err(OutOfRange));
        assert_eq!(element_count_of(largest_list_of_floats + 1, 4, 1, 0), Ok(i32::MAX / 4));
        assert_eq!(element_count_of(1 << 30, 4, 0, 16), Ok(16));
        assert_eq!(element_count_of(1 << 30, 4, (1 << 30) - 16, 0), Ok(16));
        assert_eq!(
            list_range(1 << 30, 4, (1 << 30) - 16, 0).unwrap().bytes,
            (1 << 32) - 64..1 << 32
        );
        assert_eq!(element_count_of(I32_MAX, 1, 0, 0), Ok(i32::MAX));
        assert_eq!(element_count_of(I32_MAX + 1, 1, 0, 0), Err(OutOfRange));
        assert_eq!(element_count_of(usize::MAX, 4, 0, 0), Err(OutOfRange));
        assert_eq!(element_count_of(usize::MAX, 4, 1 << 62, 1), Err(OutOfRange));
        assert_eq!(element_count_of(usize::MAX, 4, (1 << 62) - 1, 1), Err(OutOfRange));
        assert_eq!(element_count_of(usize::MAX, 4, (1 << 62) - 2, 1), Ok(1));
    }

    #[test]
    fn a_typed_array_is_a_list_of_the_numbers_that_it_holds() {
        // A Float32Array of eight elements at byte 16 of a buffer of 64 bytes.
        let float32_array = view(64, 16, 8, 4);
        let range = typed_array_list_range(&float32_array, 3, 2).unwrap();
        assert_eq!(range.bytes_in_buffer, 28..36);
        assert_eq!(range.element_count.as_gl_sizei(), 2);
        let bytes_in_buffer = |src_offset, src_length_override| {
            typed_array_list_range(&float32_array, src_offset, src_length_override).map(|range| range.bytes_in_buffer)
        };
        assert_eq!(bytes_in_buffer(0, 0), Ok(16..48));
        assert_eq!(bytes_in_buffer(7, 0), Ok(44..48));
        assert_eq!(bytes_in_buffer(8, 0), Ok(48..48));
        assert_eq!(bytes_in_buffer(0, 1), Ok(16..20));
        assert_eq!(typed_array_list_range(&float32_array, 9, 0), Err(OutOfRange));
        assert_eq!(typed_array_list_range(&float32_array, 7, 2), Err(OutOfRange));

        let half_gib_of_floats = view(1 << 31, 0, 1 << 29, 4);
        assert_eq!(typed_array_list_range(&half_gib_of_floats, 0, 0), Err(OutOfRange));
        assert_eq!(
            typed_array_list_range(&half_gib_of_floats, 1, 0)
                .unwrap()
                .element_count
                .as_gl_sizei(),
            (1 << 29) - 1
        );
    }

    #[test]
    fn a_list_tail_leads_with_the_elements_of_a_list_from_an_offset_on() {
        let leading_bytes = |list_length, src_offset, count| {
            sequence_list_tail(list_length, 4, src_offset).map(|tail| tail.bytes_of_leading_elements(count))
        };
        assert_eq!(leading_bytes(8, 0, 4), Ok(Some(0..16)));
        assert_eq!(leading_bytes(8, 3, 4), Ok(Some(12..28)));
        assert_eq!(leading_bytes(8, 4, 4), Ok(Some(16..32)));
        assert_eq!(leading_bytes(8, 7, 1), Ok(Some(28..32)));
        assert_eq!(leading_bytes(8, 8, 0), Ok(Some(32..32)));
        assert_eq!(leading_bytes(8, 3, 0), Ok(Some(12..12)));

        assert_eq!(leading_bytes(8, 5, 4), Ok(None));
        assert_eq!(leading_bytes(8, 8, 1), Ok(None));
        assert_eq!(leading_bytes(0, 0, 1), Ok(None));

        assert_eq!(leading_bytes(8, 9, 0), Err(OutOfRange));
        assert_eq!(leading_bytes(0, 1, 0), Err(OutOfRange));
        assert_eq!(leading_bytes(8, u64::MAX, 1), Err(OutOfRange));
    }

    #[test]
    fn a_typed_array_list_tail_leads_with_where_the_typed_array_is_in_its_buffer() {
        // A Float32Array of eight elements at byte 16 of a buffer of 64 bytes.
        let float32_array = view(64, 16, 8, 4);
        let leading_bytes_in_buffer = |src_offset, count| {
            typed_array_list_tail(&float32_array, src_offset).map(|tail| tail.bytes_of_leading_elements(count))
        };
        assert_eq!(leading_bytes_in_buffer(0, 4), Ok(Some(16..32)));
        assert_eq!(leading_bytes_in_buffer(3, 4), Ok(Some(28..44)));
        assert_eq!(leading_bytes_in_buffer(4, 4), Ok(Some(32..48)));
        assert_eq!(leading_bytes_in_buffer(7, 1), Ok(Some(44..48)));
        assert_eq!(leading_bytes_in_buffer(5, 4), Ok(None));
        assert_eq!(leading_bytes_in_buffer(8, 1), Ok(None));
        assert_eq!(leading_bytes_in_buffer(9, 0), Err(OutOfRange));

        for typed_array in [out_of_bounds(view(32, 16, 4, 4), 4), detached_view(view(32, 16, 4, 4))] {
            assert_eq!(
                typed_array_list_tail(&typed_array, 0).map(|tail| tail.bytes_of_leading_elements(1)),
                Ok(None)
            );
            assert_eq!(typed_array_list_tail(&typed_array, 1), Err(OutOfRange));
        }
    }

    #[test]
    fn a_list_tail_is_not_bounded_to_what_gl_can_count() {
        // NB: This is the heap of a program that was compiled to WebAssembly, which hands GL the whole heap and where
        //     its numbers are in it.
        let four_gib_heap_of_floats = view(1 << 32, 0, 1 << 30, 4);
        for address_of_numbers in [0x10, 0x10_0000, 0x4000_0000, 0x7fff_fff0, 0x8000_0400] {
            let src_offset = u64::try_from(address_of_numbers / 4).unwrap();
            assert_eq!(
                typed_array_list_tail(&four_gib_heap_of_floats, src_offset)
                    .map(|tail| tail.bytes_of_leading_elements(4)),
                Ok(Some(address_of_numbers..address_of_numbers + 16))
            );
        }
        assert_eq!(typed_array_list_range(&four_gib_heap_of_floats, 4, 0), Err(OutOfRange));
        assert_eq!(
            typed_array_list_tail(&four_gib_heap_of_floats, (1 << 30) - 1)
                .map(|tail| tail.bytes_of_leading_elements(4)),
            Ok(None)
        );
        assert_eq!(
            typed_array_list_tail(&four_gib_heap_of_floats, (1 << 30) + 1),
            Err(OutOfRange)
        );

        assert_eq!(
            sequence_list_tail(I32_MAX + 1, 1, 0).map(|tail| tail.bytes_of_leading_elements(4)),
            Ok(Some(0..4))
        );
    }

    #[test]
    fn a_typed_array_that_is_out_of_bounds_or_detached_is_an_empty_list() {
        for element_size in [4, 1] {
            for typed_array in [
                out_of_bounds(view(32, 16, 4, element_size), 4),
                detached_view(view(32, 16, 4, element_size)),
            ] {
                let whole_list = typed_array_list_range(&typed_array, 0, 0).unwrap();
                assert!(whole_list.bytes_in_buffer.is_empty());
                assert_eq!(whole_list.element_count.as_gl_sizei(), 0);
                assert_eq!(typed_array_list_range(&typed_array, 1, 0), Err(OutOfRange));
                assert_eq!(typed_array_list_range(&typed_array, 0, 1), Err(OutOfRange));
            }
        }
    }
}
