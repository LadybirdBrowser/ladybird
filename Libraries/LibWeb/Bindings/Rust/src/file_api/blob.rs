/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Blob, https://w3c.github.io/FileAPI/#blob-section

use core::cell::{Ref as CellRef, RefCell};
use core::ffi::c_void;
use core::ops::Deref;
use core::ptr::NonNull;

use ak::{Utf16String, Utf16StringUnits};
use libjs_rust::api::{Gc, MarkedVec, Object, RawNativeFunctionResult, Trace};
use libwebcommon_rust::mime_sniff::mime_type::MimeType;

use super::file::File;
use crate::bindings::conversions::{BufferSource, CppInterface, UsvString};
use crate::bindings::interface::{Inherits, Ref, Wrappable, allocate, cell_into_abi};
use crate::bindings::structured_data::{Serializable, SerializationReader, SerializationWriter};
use crate::bindings::{CallContext, WebIdlResult};

unsafe extern "C" {
    fn web_blob_bytes_destroy(owner: *mut c_void);
    fn web_blob_get_stream(realm: *mut c_void, bytes: ak::Span<'_, u8>) -> *mut c_void;
    fn web_blob_text(realm: *mut c_void, bytes: ak::Span<'_, u8>) -> *mut c_void;
    fn web_blob_array_buffer(realm: *mut c_void, bytes: ak::Span<'_, u8>) -> *mut c_void;
    fn web_blob_bytes(realm: *mut c_void, bytes: ak::Span<'_, u8>) -> *mut c_void;
    fn web_blob_deserialize_byte_sequence(
        serialized: *mut c_void,
        out_bytes: &mut ForeignBytesAbi,
    ) -> RawNativeFunctionResult;
}

/// The enumeration EndingType of FileAPI/Blob.idl.
#[derive(Clone, Copy, Eq, PartialEq)]
pub(crate) enum EndingType {
    Transparent,
    Native,
}

/// The dictionary BlobPropertyBag of FileAPI/Blob.idl, whose members the bindings convert.
pub(crate) struct BlobPropertyBag {
    pub(crate) endings: EndingType,
    pub(crate) r#type: Utf16String,
}

/// An object of Blob or of File, which inherits from it.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum AnyBlob {
    Blob(Ref<Blob>),
    File(Ref<File>),
}

impl Deref for AnyBlob {
    type Target = Blob;

    fn deref(&self) -> &Blob {
        match self {
            Self::Blob(blob) => blob,
            Self::File(file) => {
                let file: &File = file;
                file.parent()
            }
        }
    }
}

impl AnyBlob {
    /// The cell of the object, as LibWeb's C++ code holds it.
    pub(crate) fn cell_into_abi(self) -> *mut c_void {
        match self {
            Self::Blob(blob) => cell_into_abi(blob),
            Self::File(file) => cell_into_abi(file),
        }
    }
}

/// The union (BufferSource or Blob or USVString) of FileAPI/Blob.idl, whose members the bindings convert.
#[derive(Clone, Trace)]
#[gc(crate = libjs_rust)]
pub(crate) enum BlobPart {
    BufferSource(BufferSource),
    Blob(AnyBlob),
    USVString(#[gc(untraced)] UsvString),
}

/// Bytes that C++ owns for a Blob, as it hands them over: the owner, which web_blob_bytes_destroy() destroys, and the
/// bytes, which live as long as it.
#[repr(C)]
pub(crate) struct ForeignBytesAbi {
    pub(crate) owner: *mut c_void,
    pub(crate) data: *const u8,
    pub(crate) length: usize,
}

/// Bytes that a C++ Web::FileAPI::BlobBytes owns: those that C++ created the Blob with, or moved to shared memory.
struct ForeignBytes {
    owner: NonNull<c_void>,
    data: *const u8,
    length: usize,
}

impl ForeignBytes {
    /// # Safety
    ///
    /// `bytes` must be bytes that C++ hands over, which nothing else destroys.
    unsafe fn from_abi(bytes: ForeignBytesAbi) -> Self {
        Self {
            owner: NonNull::new(bytes.owner).expect("C++ hands over the owner of the bytes"),
            data: bytes.data,
            length: bytes.length,
        }
    }

    fn as_slice(&self) -> &[u8] {
        // SAFETY: The owner keeps the bytes alive and unchanged for as long as it lives.
        unsafe { ak::slice_from_raw_parts(self.data, self.length) }
    }
}

impl Drop for ForeignBytes {
    fn drop(&mut self) {
        // SAFETY: The value owns the owner, which nothing uses after this.
        unsafe { web_blob_bytes_destroy(self.owner.as_ptr()) };
    }
}

/// https://w3c.github.io/FileAPI/#dfn-byte-sequence
enum ByteSequence {
    Owned(Vec<u8>),
    Foreign(ForeignBytes),
}

impl ByteSequence {
    fn as_slice(&self) -> &[u8] {
        match self {
            Self::Owned(bytes) => bytes,
            Self::Foreign(bytes) => bytes.as_slice(),
        }
    }
}

#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct Blob {
    // https://w3c.github.io/FileAPI/#dfn-byte-sequence
    // NB: The bytes move to shared memory the first time they are shared, and stay there.
    #[gc(untraced)]
    byte_sequence: RefCell<ByteSequence>,
    // https://w3c.github.io/FileAPI/#dfn-type
    #[gc(untraced)]
    r#type: Utf16String,
}

impl Wrappable for Blob {}

fn append_utf8(bytes: &mut Vec<u8>, string: &UsvString) {
    let length = string.utf8_length();
    bytes.reserve(length);
    // SAFETY: The spare capacity has room for the encoding, which encode_utf8() initializes before the length grows
    //         over it.
    unsafe {
        let written = string.encode_utf8(bytes.spare_capacity_mut());
        bytes.set_len(bytes.len() + written);
    }
}

// https://w3c.github.io/FileAPI/#convert-line-endings-to-native
fn convert_line_endings_to_native(s: &str) -> String {
    // 1. Let native line ending be the code point U+000A LF.
    let native_line_ending = "\n";

    // 2. If the underlying platform’s conventions are to represent newlines as a carriage return and line feed
    //    sequence, set native line ending to the code point U+000D CR followed by the code point U+000A LF.
    // NOTE: this step is a no-op since LibWeb does not compile on Windows, which is the only platform we know of that
    //       that uses a carriage return and line feed sequence for line endings.

    let code_points = s.as_bytes();
    let collect_token = |position: &mut usize| {
        let token_start = *position;
        while *position < code_points.len() && code_points[*position] != b'\n' && code_points[*position] != b'\r' {
            *position += 1;
        }
        &s[token_start..*position]
    };

    // 3. Set result to the empty string.
    let mut result = String::with_capacity(s.len());

    // 4. Let position be a position variable for s, initially pointing at the start of s.
    let mut position = 0;

    // 5. Let token be the result of collecting a sequence of code points that are not equal to U+000A LF or U+000D CR
    //    from s given position.
    let token = collect_token(&mut position);

    // 6. Append token to result.
    result.push_str(token);

    // 7. While position is not past the end of s:
    while position < code_points.len() {
        // 1. If the code point at position within s equals U+000D CR:
        if code_points[position] == b'\r' {
            // 1. Append native line ending to result.
            result.push_str(native_line_ending);

            // 2. Advance position by 1.
            position += 1;

            // 3. If position is not past the end of s and the code point at position within s equals U+000A LF advance
            //    position by 1.
            if position < code_points.len() && code_points[position] == b'\n' {
                position += 1;
            }
        }
        // 2. Otherwise if the code point at position within s equals U+000A LF, advance position by 1 and append native
        //    line ending to result.
        else if code_points[position] == b'\n' {
            position += 1;
            result.push_str(native_line_ending);
        }

        // 3. Let token be the result of collecting a sequence of code points that are not equal to U+000A LF or U+000D
        //    CR from s given position.
        let token = collect_token(&mut position);

        // 4. Append token to result.
        result.push_str(token);
    }

    // 8. Return result.
    result
}

// https://w3c.github.io/FileAPI/#process-blob-parts
pub(crate) fn process_blob_parts(blob_parts: &MarkedVec<'_, BlobPart>, options: &BlobPropertyBag) -> Vec<u8> {
    // 1. Let bytes be an empty sequence of bytes.
    let mut bytes = Vec::new();

    // 2. For each element in parts:
    blob_parts.with_values(|blob_parts| {
        for element in blob_parts {
            match element {
                // 1. If element is a USVString, run the following sub-steps:
                BlobPart::USVString(string) => {
                    // 1. Let s be element.
                    // 2. If the endings member of options is "native", set s to the result of converting line endings
                    //    to native of element.
                    // 3. Append the result of UTF-8 encoding s to bytes.
                    if options.endings == EndingType::Native {
                        bytes.extend_from_slice(convert_line_endings_to_native(&string.to_utf8()).as_bytes());
                    } else {
                        append_utf8(&mut bytes, string);
                    }
                }
                // 2. If element is a BufferSource, get a copy of the bytes held by the buffer source, and append those
                //    bytes to bytes.
                BlobPart::BufferSource(buffer_source) => {
                    // SAFETY: Appending the bytes neither runs script nor allocates a cell.
                    unsafe { buffer_source.with_bytes(|held_bytes| bytes.extend_from_slice(held_bytes)) };
                }
                // 3. If element is a Blob, append the bytes it represents to bytes.
                BlobPart::Blob(blob) => bytes.extend_from_slice(&blob.raw_bytes()),
            }
        }
    });

    // 3. Return bytes.
    bytes
}

fn is_basic_latin(string: &Utf16String) -> bool {
    match string.as_units() {
        Utf16StringUnits::Ascii(bytes) => bytes.iter().all(|&byte| (0x20..=0x7E).contains(&byte)),
        Utf16StringUnits::Utf16(code_units) => code_units.iter().all(|&code_unit| (0x20..=0x7E).contains(&code_unit)),
    }
}

/// The type of a Blob or a File that is constructed with the type member of its options.
pub(crate) fn type_from_options(r#type: &Utf16String) -> Utf16String {
    // FIXME: 1. If the type member is provided and is not the empty string, let t be set to the type dictionary member.
    //    If t contains any characters outside the range U+0020 to U+007E, then set t to the empty string and return
    //    from these substeps.
    // FIXME: 2. Convert every character in t to ASCII lowercase.

    // NOTE: The spec is out of date, and we are supposed to call into the MimeType parser here.
    if !is_basic_latin(r#type) {
        return Utf16String::default();
    }
    let input: Vec<char> = r#type
        .to_utf16()
        .iter()
        .map(|&code_unit| char::from(code_unit as u8))
        .collect();
    MimeType::parse(&input).map_or_else(Utf16String::default, |mime_type| {
        Utf16String::from_ascii(mime_type.serialize().as_bytes())
    })
}

impl Blob {
    pub(crate) fn new(bytes: Vec<u8>, r#type: Utf16String) -> Self {
        Self {
            byte_sequence: RefCell::new(ByteSequence::Owned(bytes)),
            r#type,
        }
    }

    /// A Blob that refers to bytes that C++ hands over.
    ///
    /// # Safety
    ///
    /// `bytes` must be bytes that C++ hands over, which nothing else destroys.
    pub(crate) unsafe fn with_foreign_bytes(bytes: ForeignBytesAbi, r#type: Utf16String) -> Self {
        Self {
            // SAFETY: The caller hands over the bytes.
            byte_sequence: RefCell::new(ByteSequence::Foreign(unsafe { ForeignBytes::from_abi(bytes) })),
            r#type,
        }
    }

    pub(crate) fn raw_bytes(&self) -> CellRef<'_, [u8]> {
        CellRef::map(self.byte_sequence.borrow(), ByteSequence::as_slice)
    }

    pub(crate) fn foreign_bytes_owner(&self) -> *mut c_void {
        match &*self.byte_sequence.borrow() {
            ByteSequence::Owned(_) => core::ptr::null_mut(),
            ByteSequence::Foreign(bytes) => bytes.owner.as_ptr(),
        }
    }

    pub(crate) fn type_string(&self) -> &Utf16String {
        &self.r#type
    }

    /// Makes the Blob refer to the same bytes in storage that C++ hands over, as it does when it moves them to shared
    /// memory.
    ///
    /// # Safety
    ///
    /// `bytes` must be bytes that C++ hands over, which nothing else destroys.
    pub(crate) unsafe fn set_foreign_bytes(&self, bytes: ForeignBytesAbi) {
        // SAFETY: The caller hands over the bytes.
        let bytes = unsafe { ForeignBytes::from_abi(bytes) };
        *self.byte_sequence.borrow_mut() = ByteSequence::Foreign(bytes);
    }

    // https://w3c.github.io/FileAPI/#ref-for-dom-blob-blob
    pub(crate) fn construct(
        _: &mut CallContext,
        blob_parts: Option<MarkedVec<'_, BlobPart>>,
        options: BlobPropertyBag,
    ) -> WebIdlResult<Self> {
        // 1. If invoked with zero parameters, return a new Blob object consisting of 0 bytes, with size set to 0, and
        //    with type set to the empty string.
        // NB: The steps below do that for a Blob constructed with zero parameters, whose options are an empty
        //     dictionary.

        // 2. Let bytes be the result of processing blob parts given blobParts and options.
        let bytes = blob_parts.map_or_else(Vec::new, |blob_parts| process_blob_parts(&blob_parts, &options));

        // 3. If the type member of the options argument is not the empty string, run the following sub-steps:
        let r#type = if options.r#type.is_empty() {
            Utf16String::default()
        } else {
            type_from_options(&options.r#type)
        };

        // 4. Return a Blob object referring to bytes as its associated byte sequence, with its size set to the length
        //    of bytes, and its type set to the value of t from the substeps above.
        Ok(Self::new(bytes, r#type))
    }

    // https://w3c.github.io/FileAPI/#dfn-size
    pub(crate) fn size(&self, _: &mut CallContext) -> WebIdlResult<u64> {
        Ok(self.raw_bytes().len() as u64)
    }

    // https://w3c.github.io/FileAPI/#dfn-type
    pub(crate) fn r#type(&self, _: &mut CallContext) -> WebIdlResult<Utf16String> {
        Ok(self.r#type.clone())
    }

    // https://w3c.github.io/FileAPI/#dfn-slice
    pub(crate) fn slice(
        &self,
        cx: &mut CallContext,
        start: Option<i64>,
        end: Option<i64>,
        content_type: Option<Utf16String>,
    ) -> WebIdlResult<Ref<Blob>> {
        // 1. Let sliceStart, sliceEnd, and sliceContentType be null.
        // 2. If start is given, set sliceStart to start.
        // 3. If end is given, set sliceEnd to end.
        // 3. If contentType is given, set sliceContentType to contentType.
        // 4. Return the result of slice blob given this, sliceStart, sliceEnd, and sliceContentType.
        Ok(allocate(cx.vm(), self.slice_blob(start, end, content_type.as_ref())))
    }

    // https://w3c.github.io/FileAPI/#slice-blob
    pub(crate) fn slice_blob(&self, start: Option<i64>, end: Option<i64>, content_type: Option<&Utf16String>) -> Blob {
        let bytes = self.raw_bytes();

        // 1. Let originalSize be blob’s size.
        let original_size = i64::try_from(bytes.len()).expect("a Blob has fewer than 2^63 bytes");

        // 2. The start parameter, if non-null, is a value for the start point of a slice blob call, and must be treated
        //    as a byte-order position, with the zeroth position representing the first byte. User agents must normalize
        //    start according to the following:
        let relative_start = match start {
            // a. If start is null, let relativeStart be 0.
            None => 0,
            // b. If start is negative, let relativeStart be max((originalSize + start), 0).
            Some(start) if start < 0 => (original_size + start).max(0),
            // c. Otherwise, let relativeStart be min(start, originalSize).
            Some(start) => start.min(original_size),
        };

        // 3. The end parameter, if non-null. is a value for the end point of a slice blob call. User agents must
        //    normalize end according to the following:
        let relative_end = match end {
            // a. If end is null, let relativeEnd be originalSize.
            None => original_size,
            // b. If end is negative, let relativeEnd be max((originalSize + end), 0).
            Some(end) if end < 0 => (original_size + end).max(0),
            // c. Otherwise, let relativeEnd be min(end, originalSize).
            Some(end) => end.min(original_size),
        };

        // 4. The contentType parameter, if non-null, is used to set the ASCII-encoded string in lower case representing
        //    the media type of the Blob. User agents must normalize contentType according to the following:
        let relative_content_type = match content_type {
            // a. If contentType is null, let relativeContentType be set to the empty string.
            None => Utf16String::default(),
            // b. Otherwise, let relativeContentType be set to contentType and run the substeps below:
            // 1. If relativeContentType contains any characters outside the range of U+0020 to U+007E, then set
            //    relativeContentType to the empty string and return from these substeps:
            Some(content_type) if !is_basic_latin(content_type) => Utf16String::default(),
            // 2. Convert every character in relativeContentType to ASCII lowercase.
            Some(content_type) => {
                let lowercase: Vec<u8> = content_type
                    .to_utf16()
                    .iter()
                    .map(|&code_unit| (code_unit as u8).to_ascii_lowercase())
                    .collect();
                Utf16String::from_ascii(&lowercase)
            }
        };

        // 5. Let span be max((relativeEnd - relativeStart), 0).
        let span = (relative_end - relative_start).max(0);

        // 6. Return a new Blob object S with the following characteristics:
        // a. S refers to span consecutive bytes from blob’s associated byte sequence, beginning with the byte at
        //    byte-order position relativeStart.
        // b. S.size = span.
        // c. S.type = relativeContentType.
        let relative_start = usize::try_from(relative_start).expect("relativeStart is not negative");
        let span = usize::try_from(span).expect("span is not negative");
        Blob::new(
            bytes[relative_start..relative_start + span].to_vec(),
            relative_content_type,
        )
    }

    // https://w3c.github.io/FileAPI/#dom-blob-stream
    pub(crate) fn stream(&self, cx: &mut CallContext) -> WebIdlResult<CppInterface> {
        // SAFETY: The realm is live, LibWeb copies the bytes, and it returns the wrapper of a new ReadableStream.
        unsafe {
            let stream = web_blob_get_stream(cx.realm().as_ptr().cast(), ak::Span::new(&self.raw_bytes()));
            Ok(CppInterface::from_wrapper(object_from_abi(stream)))
        }
    }

    // https://w3c.github.io/FileAPI/#dom-blob-text
    pub(crate) fn text(&self, cx: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        // SAFETY: The realm is live, LibWeb copies the bytes, and it returns a new promise.
        Ok(unsafe {
            object_from_abi(web_blob_text(
                cx.realm().as_ptr().cast(),
                ak::Span::new(&self.raw_bytes()),
            ))
        })
    }

    // https://w3c.github.io/FileAPI/#dom-blob-arraybuffer
    pub(crate) fn array_buffer(&self, cx: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        // SAFETY: The realm is live, LibWeb copies the bytes, and it returns a new promise.
        Ok(unsafe {
            object_from_abi(web_blob_array_buffer(
                cx.realm().as_ptr().cast(),
                ak::Span::new(&self.raw_bytes()),
            ))
        })
    }

    // https://w3c.github.io/FileAPI/#dom-blob-bytes
    pub(crate) fn bytes(&self, cx: &mut CallContext) -> WebIdlResult<Gc<Object>> {
        // SAFETY: The realm is live, LibWeb copies the bytes, and it returns a new promise.
        Ok(unsafe {
            object_from_abi(web_blob_bytes(
                cx.realm().as_ptr().cast(),
                ak::Span::new(&self.raw_bytes()),
            ))
        })
    }

    pub(crate) fn serialize(&self, serialized: &mut SerializationWriter) {
        // FIXME: 1. Set serialized.[[SnapshotState]] to value’s snapshot state.

        // NON-STANDARD: FileAPI spec doesn't specify that type should be serialized, although
        //               to be conformant with other browsers this needs to be serialized.
        serialized.write_utf16_string(&self.r#type);

        // 2. Set serialized.[[ByteSequence]] to value’s underlying byte sequence.
        serialized.write_bytes(&self.raw_bytes());
    }

    pub(crate) fn deserialize(serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        // FIXME: 1. Set value’s snapshot state to serialized.[[SnapshotState]].

        // NON-STANDARD: FileAPI spec doesn't specify that type should be deserialized, although
        //               to be conformant with other browsers this needs to be deserialized.
        let r#type = serialized.read_utf16_string()?;

        // 2. Set value’s underlying byte sequence to serialized.[[ByteSequence]].
        let mut bytes = ForeignBytesAbi {
            owner: core::ptr::null_mut(),
            data: core::ptr::null(),
            length: 0,
        };
        // SAFETY: The reader is lent for the steps.
        unsafe { web_blob_deserialize_byte_sequence(serialized.as_raw(), &mut bytes) }.into_completion()?;

        // SAFETY: C++ hands over the bytes it decoded.
        Ok(unsafe { Self::with_foreign_bytes(bytes, r#type) })
    }
}

/// The object that LibWeb returns.
///
/// # Safety
///
/// `object` must be a live object.
unsafe fn object_from_abi(object: *mut c_void) -> Gc<Object> {
    let object = NonNull::new(object.cast::<Object>()).expect("LibWeb returns an object");
    // SAFETY: The caller passes a live object.
    unsafe { Gc::from_non_null(object) }
}

impl Serializable for Blob {
    fn serialization_steps(
        &self,
        _: &mut CallContext,
        serialized: &mut SerializationWriter,
        _for_storage: bool,
    ) -> WebIdlResult<()> {
        self.serialize(serialized);
        Ok(())
    }

    fn deserialization_steps(_: &mut CallContext, serialized: &mut SerializationReader) -> WebIdlResult<Self> {
        Self::deserialize(serialized)
    }
}
