/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Borrowing and copying the script memory that WebGL operations read and write. Which bytes an operation may have
//! is the arithmetic of byte_ranges.rs.

use core::ops::Range;

use libjs_rust::api::{ArrayBuffer, Gc, TypedArrayBase};

use super::byte_ranges::{
    Extent, GlSize, ListTail, OutOfRange, UploadRange, destination_range, destination_range_of_get_buffer_sub_data,
    list_range, offset_in_buffer_of_checked_write, sequence_list_tail, typed_array_list_range, typed_array_list_tail,
    upload_range, upload_range_of_2d_texture,
};
use super::canvas::{is_valid_2d_pixel_unpack_state, required_2d_texture_data_size};
use super::pixel_unpacking::ImageUnpackConversion;
use super::proxy::recorder::{PlainNumber, bytes_of};
use super::state::ContextState;
use super::types::Float32List;
use super::web_gl_rendering_context_overloads::Int32List;
use super::web_gl2_rendering_context_base::Uint32List;
use crate::bindings::conversions::{
    ArrayBufferView, BufferSource, BufferSourceExtent, copy_of_bytes_of_buffer, with_bytes_of_buffer,
};

fn buffer_and_extent_of(source: &BufferSource) -> (Gc<ArrayBuffer>, Extent) {
    let BufferSourceExtent {
        buffer,
        is_out_of_bounds,
        byte_offset,
        byte_length,
        element_size,
    } = source.extent();
    let extent = Extent {
        buffer_is_detached: buffer.is_detached(),
        buffer_byte_length: buffer.byte_length(),
        is_out_of_bounds,
        byte_offset,
        byte_length,
        element_size,
    };
    (buffer, extent)
}

fn extent_of(source: &BufferSource) -> Extent {
    let (_, extent) = buffer_and_extent_of(source);
    extent
}

pub(crate) struct SourceBytes {
    buffer: Gc<ArrayBuffer>,
    range: UploadRange,
}

pub(crate) fn source_bytes(
    source: BufferSource,
    src_offset: u64,
    src_length_override: u32,
) -> Result<SourceBytes, OutOfRange> {
    let (buffer, extent) = buffer_and_extent_of(&source);
    let range = upload_range(&extent, src_offset, src_length_override)?;
    Ok(SourceBytes { buffer, range })
}

impl SourceBytes {
    pub(crate) fn len(&self) -> usize {
        self.range.length().get()
    }

    pub(crate) fn gl_size(&self) -> i32 {
        self.range.length().as_gl_sizei()
    }

    /// # Safety
    ///
    /// The function must not run script or allocate a cell, either of which could detach, resize or free the buffer
    /// that the bytes are in.
    unsafe fn with<R>(&self, function: impl FnOnce(&[u8]) -> R) -> R {
        // SAFETY: The caller keeps the function from running script and from allocating.
        unsafe { with_bytes_of_buffer(&self.buffer, self.range.bytes_in_buffer(), function) }
            .expect("nothing has run since the bytes were found")
    }

    fn copy(&self) -> Vec<u8> {
        copy_of_bytes_of_buffer(&self.buffer, self.range.bytes_in_buffer())
            .expect("nothing has run since the bytes were found")
    }
}

/// The pixels that texImage2D or texSubImage2D upload from an ArrayBufferView.
pub(crate) struct TextureData {
    bytes: SourceBytes,
    /// `None` when GL takes the pixels as they are.
    conversion: Option<ImageUnpackConversion>,
}

impl TextureData {
    pub(crate) fn gl_size(&self) -> i32 {
        self.bytes.gl_size()
    }
}

impl ContextState {
    /// # Safety
    ///
    /// `record` gets the bytes while they are borrowed from script memory, so it must not run script or allocate a
    /// cell, either of which could detach, resize or free the buffer that they are in. Recording one command with
    /// them does neither.
    pub(crate) unsafe fn upload<R>(&self, bytes: &SourceBytes, record: impl FnOnce(&[u8]) -> R) -> R {
        // SAFETY: The caller keeps `record` from running script and from allocating, and the recorder defers what
        //         would call out of the process until the bytes are given back.
        self.proxy
            .record_with_borrowed_data(bytes.len(), || unsafe { bytes.with(record) })
    }

    /// `record` gets `None` when the source has no bytes from the offset of the upload on, and otherwise the data,
    /// even if the upload records none of it.
    ///
    /// # Safety
    ///
    /// As for upload().
    pub(crate) unsafe fn upload_through_nullable_pointer<R>(
        &self,
        bytes: &SourceBytes,
        record: impl FnOnce(Option<&[u8]>) -> R,
    ) -> R {
        let source_has_bytes = bytes.range.source_has_bytes();
        // SAFETY: The caller keeps `record` from running script and from allocating.
        unsafe { self.upload(bytes, |bytes| record(source_has_bytes.then_some(bytes))) }
    }

    pub(crate) fn texture_data_for_2d_upload(
        &self,
        source: ArrayBufferView,
        src_offset: u64,
        width: i32,
        height: i32,
        format: u32,
        r#type: u32,
    ) -> Result<TextureData, OutOfRange> {
        let unpack_state = self.unpack_state.get();
        if !is_valid_2d_pixel_unpack_state(width, &unpack_state) {
            return Err(OutOfRange);
        }
        let size_that_the_image_takes = required_2d_texture_data_size(width, height, format, r#type, &unpack_state);
        let (buffer, extent) = buffer_and_extent_of(&BufferSource::from(source));
        let range = upload_range_of_2d_texture(&extent, src_offset, size_that_the_image_takes)?;

        let flip_y = self.unpack_flip_y.get();
        let premultiply_alpha = self.unpack_premultiply_alpha.get();
        let conversion = (flip_y || premultiply_alpha).then_some(ImageUnpackConversion {
            width,
            height,
            format,
            r#type,
            unpack_state,
            flip_y,
            premultiply_alpha,
        });
        Ok(TextureData {
            bytes: SourceBytes { buffer, range },
            conversion,
        })
    }

    /// `record` gets `None` when the source has no bytes from the offset of the upload on, and otherwise the pixels,
    /// flipped and premultiplied as UNPACK_FLIP_Y_WEBGL and UNPACK_PREMULTIPLY_ALPHA_WEBGL ask. Converted pixels are
    /// a copy, which leaves the memory of script as it is.
    ///
    /// # Safety
    ///
    /// As for upload().
    pub(crate) unsafe fn upload_texture_data<R>(
        &self,
        texture_data: &TextureData,
        record: impl FnOnce(Option<&[u8]>) -> R,
    ) -> R {
        let Some(conversion) = texture_data.conversion else {
            // SAFETY: The caller keeps `record` from running script and from allocating.
            return unsafe { self.upload_through_nullable_pointer(&texture_data.bytes, record) };
        };
        let mut pixels = texture_data.bytes.copy();
        conversion.convert(&mut pixels);
        record(texture_data.bytes.range.source_has_bytes().then_some(&pixels))
    }
}

pub(crate) fn destination_bytes(view: &ArrayBufferView, dst_offset: u64) -> Result<Range<usize>, OutOfRange> {
    destination_range(&extent_of(&BufferSource::from(*view)), dst_offset)
}

pub(crate) fn destination_bytes_of_get_buffer_sub_data(
    dst_buffer: &ArrayBufferView,
    dst_offset: u64,
    length: u32,
) -> Result<Option<Range<usize>>, OutOfRange> {
    destination_range_of_get_buffer_sub_data(&extent_of(&BufferSource::from(*dst_buffer)), dst_offset, length)
}

pub(crate) fn write_checked(view: &ArrayBufferView, bytes: &[u8], starting_offset: usize) -> Result<(), OutOfRange> {
    let (buffer, extent) = buffer_and_extent_of(&BufferSource::from(*view));
    if let Some(write_offset) = offset_in_buffer_of_checked_write(&extent, bytes.len(), starting_offset)? {
        buffer.overwrite(write_offset, bytes);
    }
    Ok(())
}

/// A copy, so the numbers stay what they are whatever script does.
pub(crate) struct ListBytes {
    bytes: Vec<u8>,
    element_count: GlSize,
}

impl ListBytes {
    pub(crate) fn gl_count(&self) -> i32 {
        self.element_count.as_gl_sizei()
    }

    pub(crate) fn gl_count_of_whole_groups_of(&self, group_size: usize) -> Option<i32> {
        self.element_count.count_of_whole_groups_of(group_size)
    }

    pub(crate) fn as_bytes(&self) -> &[u8] {
        &self.bytes
    }
}

pub(crate) trait NumberList {
    type Number: PlainNumber;

    fn typed_array(&self) -> Option<Gc<TypedArrayBase>>;
    fn sequence(&self) -> &[Self::Number];
}

impl NumberList for Float32List {
    type Number = f32;

    fn typed_array(&self) -> Option<Gc<TypedArrayBase>> {
        match self {
            Self::Float32Array(typed_array) => Some(*typed_array),
            Self::Sequence(_) => None,
        }
    }

    fn sequence(&self) -> &[f32] {
        match self {
            Self::Float32Array(_) => &[],
            Self::Sequence(numbers) => numbers,
        }
    }
}

impl NumberList for Int32List {
    type Number = i32;

    fn typed_array(&self) -> Option<Gc<TypedArrayBase>> {
        match self {
            Self::Int32Array(typed_array) => Some(*typed_array),
            Self::Sequence(_) => None,
        }
    }

    fn sequence(&self) -> &[i32] {
        match self {
            Self::Int32Array(_) => &[],
            Self::Sequence(numbers) => numbers,
        }
    }
}

impl NumberList for Uint32List {
    type Number = u32;

    fn typed_array(&self) -> Option<Gc<TypedArrayBase>> {
        match self {
            Self::Uint32Array(typed_array) => Some(*typed_array),
            Self::Sequence(_) => None,
        }
    }

    fn sequence(&self) -> &[u32] {
        match self {
            Self::Uint32Array(_) => &[],
            Self::Sequence(numbers) => numbers,
        }
    }
}

pub(crate) fn list_bytes<L: NumberList>(
    list: &L,
    src_offset: u64,
    src_length_override: u32,
) -> Result<ListBytes, OutOfRange> {
    let Some(typed_array) = list.typed_array() else {
        let numbers = list.sequence();
        let range = list_range(numbers.len(), size_of::<L::Number>(), src_offset, src_length_override)?;
        let numbers = numbers.get(range.elements).ok_or(OutOfRange)?;
        return Ok(ListBytes {
            bytes: bytes_of(numbers).to_vec(),
            element_count: range.element_count,
        });
    };

    let (buffer, extent) = buffer_and_extent_of(&BufferSource::TypedArray(typed_array));
    let range = typed_array_list_range(&extent, src_offset, src_length_override)?;
    let bytes = copy_of_bytes_of_buffer(&buffer, range.bytes_in_buffer).ok_or(OutOfRange)?;
    Ok(ListBytes {
        bytes,
        element_count: range.element_count,
    })
}

enum NumbersOfList<'list, N> {
    Sequence(&'list [N]),
    TypedArray(Gc<ArrayBuffer>),
}

/// For an operation that records only the first few numbers of a list from an offset on. GL is not told how many
/// numbers the list has, so unlike list_bytes() this takes a list of any length, and copies only what is recorded.
pub(crate) struct NumbersFromOffset<'list, N> {
    numbers: NumbersOfList<'list, N>,
    tail: ListTail,
}

pub(crate) fn numbers_from_offset<L: NumberList>(
    list: &L,
    src_offset: u64,
) -> Result<NumbersFromOffset<'_, L::Number>, OutOfRange> {
    let Some(typed_array) = list.typed_array() else {
        let numbers = list.sequence();
        return Ok(NumbersFromOffset {
            tail: sequence_list_tail(numbers.len(), size_of::<L::Number>(), src_offset)?,
            numbers: NumbersOfList::Sequence(numbers),
        });
    };

    let (buffer, extent) = buffer_and_extent_of(&BufferSource::TypedArray(typed_array));
    Ok(NumbersFromOffset {
        tail: typed_array_list_tail(&extent, src_offset)?,
        numbers: NumbersOfList::TypedArray(buffer),
    })
}

impl<N: PlainNumber> NumbersFromOffset<'_, N> {
    /// `None` when the list has fewer numbers from the offset on.
    pub(crate) fn copy_of_first(&self, count: usize) -> Option<Vec<u8>> {
        let bytes = self.tail.bytes_of_leading_elements(count)?;
        match self.numbers {
            NumbersOfList::Sequence(numbers) => bytes_of(numbers).get(bytes).map(<[u8]>::to_vec),
            NumbersOfList::TypedArray(buffer) => copy_of_bytes_of_buffer(&buffer, bytes),
        }
    }
}

/// `None` when the list has fewer numbers.
pub(crate) fn copy_of_first_numbers<L: NumberList>(list: &L, count: usize) -> Option<Vec<u8>> {
    numbers_from_offset(list, 0).ok()?.copy_of_first(count)
}
