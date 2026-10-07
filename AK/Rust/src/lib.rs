/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust views of shared AK data structures.

mod scope_guard;

pub use scope_guard::ScopeGuard;

use std::num::NonZeroUsize;
use std::sync::atomic::{AtomicU32, Ordering};

/// Set in the raw word of a string whose bytes are stored inline in the word itself.
pub const SHORT_STRING_FLAG: usize = 1;
/// The byte count of a short string is stored in its tag byte, above the flag.
pub const SHORT_STRING_BYTE_COUNT_SHIFT: u32 = 2;
/// Set in `Utf16StringDataHeader::flags` when the storage holds UTF-16 code units rather than ASCII bytes.
pub const HAS_UTF16_STORAGE: u32 = 1;
const UNKNOWN_CODE_POINT_LENGTH: u32 = u32::MAX;

/// Mirrors `AK::Detail::Utf16StringDataHeader`. The string's storage follows it directly.
#[repr(C, align(8))]
pub struct Utf16StringDataHeader {
    pub reference_count: AtomicU32,
    pub length_in_code_units: u32,
    pub length_in_code_points: AtomicU32,
    pub hash: AtomicU32,
    pub flags: AtomicU32,
}

const _: () = assert!(size_of::<Utf16StringDataHeader>() == 24);
const _: () = assert!(align_of::<Utf16StringDataHeader>() == 8);
const _: () = assert!(std::mem::offset_of!(Utf16StringDataHeader, reference_count) == 0);
const _: () = assert!(std::mem::offset_of!(Utf16StringDataHeader, length_in_code_units) == 4);
const _: () = assert!(std::mem::offset_of!(Utf16StringDataHeader, length_in_code_points) == 8);
const _: () = assert!(std::mem::offset_of!(Utf16StringDataHeader, hash) == 12);
const _: () = assert!(std::mem::offset_of!(Utf16StringDataHeader, flags) == 16);

/// A borrowed view of an `AK::Utf16String` or `AK::Utf16FlyString`.
pub enum Utf16StringUnits<'a> {
    Ascii(&'a [u8]),
    Utf16(&'a [u16]),
}

unsafe extern "C" {
    fn ladybird_utf16_string_create_uninitialized(length: usize, has_ascii_storage: bool) -> usize;
    fn ladybird_utf16_fly_string_from_utf8(data: *const u8, length: usize) -> usize;
    fn ladybird_utf16_fly_string_from_utf16(data: *const u16, length: usize) -> usize;
    fn ladybird_utf16_string_unref(raw: usize);
    fn ladybird_utf16_validate(data: *const u16, length: usize) -> bool;
    fn ladybird_utf16_validate_prefix(data: *const u16, length: usize, valid_code_units: *mut usize) -> bool;
    fn ladybird_convert_valid_utf16_to_utf8(data: *const u16, length: usize, output: *mut u8) -> usize;
    fn ladybird_utf16_string_from_utf8(data: *const u8, length: usize) -> usize;
    fn ladybird_utf16_string_to_well_formed(data: *const u16, length: usize) -> usize;
    fn ladybird_size_required_to_decode_base64(data: *const u8, length: usize, has_ascii_storage: bool) -> usize;
    fn ladybird_decode_base64_into(
        data: *const u8,
        length: usize,
        has_ascii_storage: bool,
        url: bool,
        last_chunk_handling: u8,
        output: *mut u8,
        output_length: *mut usize,
        read: *mut usize,
    ) -> u8;
    fn ladybird_encode_base64_to_utf16(data: *const u8, length: usize, url: bool, omit_padding: bool) -> usize;
    fn ladybird_convert_to_decimal_exponential_form(
        value: f64,
        sign: *mut bool,
        fraction: *mut u64,
        exponent: *mut i32,
    );
}

// AK string data is immutable, and its reference count and cached metadata are atomic.
// The fly-string table synchronizes interning and destruction across threads.
// A raw string is never zero, which is what lets `Option` of an owner stay one word, the same as
// `AK::Optional<AK::Utf16String>`.
#[repr(transparent)]
struct OwnedUtf16String {
    raw: NonZeroUsize,
}

impl OwnedUtf16String {
    #[inline]
    const fn empty() -> Self {
        Self {
            raw: NonZeroUsize::new(SHORT_STRING_FLAG).unwrap(),
        }
    }

    #[inline]
    unsafe fn from_raw(raw: usize) -> Self {
        Self {
            raw: NonZeroUsize::new(raw).expect("raw AK strings are never zero"),
        }
    }

    #[inline]
    fn into_raw(self) -> usize {
        let this = std::mem::ManuallyDrop::new(self);
        this.raw.get()
    }

    #[inline]
    fn raw_word(&self) -> &usize {
        // SAFETY: NonZeroUsize has the same layout as usize.
        unsafe { &*std::ptr::from_ref(&self.raw).cast::<usize>() }
    }

    #[inline]
    fn as_units(&self) -> Utf16StringUnits<'_> {
        // SAFETY: This owner keeps the raw string alive for the returned lifetime.
        unsafe { utf16_string_units(self.raw_word()) }
    }
}

impl Clone for OwnedUtf16String {
    #[inline]
    fn clone(&self) -> Self {
        // SAFETY: This owner keeps the raw string alive while adding a reference.
        unsafe { reference_utf16_string(self.raw.get()) };
        // SAFETY: The new reference is transferred to the returned owner.
        unsafe { Self::from_raw(self.raw.get()) }
    }
}

impl Drop for OwnedUtf16String {
    #[inline]
    fn drop(&mut self) {
        // SAFETY: This object owns one reference to its raw string.
        unsafe {
            release_utf16_string_with(self.raw.get(), |raw| ladybird_utf16_string_unref(raw));
        }
    }
}

/// A one-word Rust owner for the same storage as `AK::Utf16String`.
#[repr(transparent)]
pub struct Utf16String(OwnedUtf16String);

/// A one-word Rust owner for the same interned storage as `AK::Utf16FlyString`.
#[repr(transparent)]
pub struct Utf16FlyString(OwnedUtf16String);

const _: () = assert!(size_of::<OwnedUtf16String>() == size_of::<usize>());
const _: () = assert!(align_of::<OwnedUtf16String>() == align_of::<usize>());
const _: () = assert!(size_of::<Utf16String>() == size_of::<usize>());
const _: () = assert!(align_of::<Utf16String>() == align_of::<usize>());
const _: () = assert!(size_of::<Utf16FlyString>() == size_of::<usize>());
const _: () = assert!(align_of::<Utf16FlyString>() == align_of::<usize>());
const _: () = assert!(size_of::<Option<Utf16String>>() == size_of::<usize>());
const _: () = assert!(size_of::<Option<Utf16FlyString>>() == size_of::<usize>());

const _: fn() = || {
    fn assert_send_sync<T: Send + Sync>() {}
    assert_send_sync::<Utf16String>();
    assert_send_sync::<Utf16FlyString>();
};

macro_rules! impl_utf16_string_owner {
    ($name:ident) => {
        impl $name {
            /// Adopts an existing C++ ownership reference without changing its count.
            ///
            /// # Safety
            ///
            /// `raw` must be a valid `AK::Utf16String` raw representation for which
            /// the caller owns one reference. The reference must not be released
            /// separately after this call.
            #[inline]
            pub unsafe fn from_raw_owned(raw: usize) -> Self {
                // SAFETY: The caller transfers a valid ownership reference.
                Self(unsafe { OwnedUtf16String::from_raw(raw) })
            }

            /// Transfers this owner's existing reference to the caller.
            #[inline]
            pub fn into_raw(self) -> usize {
                self.0.into_raw()
            }

            /// Returns the shared one-word identity without transferring ownership.
            #[inline]
            pub fn raw_identity(&self) -> usize {
                self.0.raw.get()
            }

            /// Borrows the shared ASCII or UTF-16 character storage directly.
            #[inline]
            pub fn as_units(&self) -> Utf16StringUnits<'_> {
                self.0.as_units()
            }

            /// Borrows UTF-16 storage, widening ASCII only when a UTF-16 slice is needed.
            pub fn to_utf16(&self) -> std::borrow::Cow<'_, [u16]> {
                match self.as_units() {
                    Utf16StringUnits::Ascii(units) => units.iter().map(|&unit| u16::from(unit)).collect(),
                    Utf16StringUnits::Utf16(units) => std::borrow::Cow::Borrowed(units),
                }
            }

            /// Returns whether the string has no code units.
            #[inline]
            pub fn is_empty(&self) -> bool {
                match self.as_units() {
                    Utf16StringUnits::Ascii(units) => units.is_empty(),
                    Utf16StringUnits::Utf16(units) => units.is_empty(),
                }
            }
        }

        impl Default for $name {
            #[inline]
            fn default() -> Self {
                Self(OwnedUtf16String::empty())
            }
        }

        impl Clone for $name {
            #[inline]
            fn clone(&self) -> Self {
                Self(self.0.clone())
            }
        }
    };
}

impl_utf16_string_owner!(Utf16String);
impl_utf16_string_owner!(Utf16FlyString);

impl Utf16String {
    /// Creates a string in AK's native representation and initializes its storage from UTF-8.
    pub fn from_utf8(string: &str) -> Self {
        if string.is_ascii() {
            return Self::from_ascii_without_validation(string.as_bytes());
        }

        // SAFETY: The string remains alive for the duration of the call.
        let raw = unsafe { ladybird_utf16_string_from_utf8(string.as_ptr(), string.len()) };
        // SAFETY: The constructor transfers one ownership reference.
        unsafe { Self::from_raw_owned(raw) }
    }

    /// Creates a string in AK's native representation and initializes its storage from UTF-16.
    pub fn from_utf16(string: &[u16]) -> Self {
        if string.iter().all(|code_unit| *code_unit <= 0x7f) {
            if string.len() < size_of::<usize>() {
                let mut bytes = [0; size_of::<usize>() - 1];
                for (index, code_unit) in string.iter().enumerate() {
                    bytes[index] = *code_unit as u8;
                }
                return Self::from_short_ascii(&bytes[..string.len()]);
            }

            let result = Self::create_uninitialized(string.len(), true);
            let storage = result.long_storage();
            for (index, code_unit) in string.iter().enumerate() {
                // SAFETY: The allocation has space for exactly `string.len()` ASCII bytes.
                unsafe { storage.add(index).write(*code_unit as u8) };
            }
            return result;
        }

        let result = Self::create_uninitialized(string.len(), false);
        // SAFETY: The source and destination are non-overlapping and the allocation has space for
        // exactly `string.len()` UTF-16 code units.
        unsafe { std::ptr::copy_nonoverlapping(string.as_ptr(), result.long_storage().cast(), string.len()) };
        result
    }

    /// Creates a string in AK's native representation from ASCII bytes.
    #[inline]
    pub fn from_ascii(string: &[u8]) -> Self {
        assert!(string.is_ascii(), "an ASCII string holds ASCII bytes");
        Self::from_ascii_without_validation(string)
    }

    /// Creates a string in AK's native representation from the ASCII bytes of `pieces` one after the other, which
    /// add up to `length` bytes, copying each piece into the string's storage directly.
    pub fn from_ascii_concatenation<'a>(length: usize, pieces: impl IntoIterator<Item = &'a [u8]>) -> Self {
        if length < size_of::<usize>() {
            let mut bytes = [0; size_of::<usize>() - 1];
            let mut byte_count = 0;
            for piece in pieces {
                bytes[byte_count..byte_count + piece.len()].copy_from_slice(piece);
                byte_count += piece.len();
            }
            assert_eq!(byte_count, length, "the pieces add up to the length of the string");
            return Self::from_ascii(&bytes[..length]);
        }

        let result = Self::create_uninitialized(length, true);
        let storage = result.long_storage();
        let mut byte_count = 0;
        for piece in pieces {
            assert!(
                piece.len() <= length - byte_count,
                "the pieces add up to the length of the string"
            );
            assert!(piece.is_ascii(), "an ASCII string holds ASCII bytes");
            // SAFETY: The allocation has space for `length` ASCII bytes, of which `byte_count` are written, and the
            // piece fits in the rest.
            unsafe { std::ptr::copy_nonoverlapping(piece.as_ptr(), storage.add(byte_count), piece.len()) };
            byte_count += piece.len();
        }
        assert_eq!(byte_count, length, "the pieces add up to the length of the string");
        result
    }

    /// Creates a string in AK's native representation of `length` ASCII bytes, which `fill` writes into the string's
    /// storage directly.
    pub fn from_ascii_with(length: usize, fill: impl FnOnce(&mut [u8])) -> Self {
        if length < size_of::<usize>() {
            let mut bytes = [0; size_of::<usize>() - 1];
            fill(&mut bytes[..length]);
            return Self::from_ascii(&bytes[..length]);
        }

        let result = Self::create_uninitialized(length, true);
        // SAFETY: The allocation has space for exactly `length` ASCII bytes, which are zeroed before they are lent out.
        let storage = unsafe {
            std::ptr::write_bytes(result.long_storage(), 0, length);
            std::slice::from_raw_parts_mut(result.long_storage(), length)
        };
        fill(storage);
        assert!(storage.is_ascii(), "an ASCII string holds ASCII bytes");
        result
    }

    #[inline]
    fn from_ascii_without_validation(string: &[u8]) -> Self {
        if string.len() < size_of::<usize>() {
            return Self::from_short_ascii(string);
        }

        let result = Self::create_uninitialized(string.len(), true);
        // SAFETY: The source and destination are non-overlapping and the allocation has space for
        // exactly `string.len()` ASCII bytes.
        unsafe { std::ptr::copy_nonoverlapping(string.as_ptr(), result.long_storage(), string.len()) };
        result
    }

    #[inline]
    fn from_short_ascii(string: &[u8]) -> Self {
        assert!(string.len() < size_of::<usize>());
        let mut bytes = [0; size_of::<usize>()];
        let tag = ((string.len() as u8) << SHORT_STRING_BYTE_COUNT_SHIFT) | SHORT_STRING_FLAG as u8;
        #[cfg(target_endian = "little")]
        {
            bytes[0] = tag;
            bytes[1..][..string.len()].copy_from_slice(string);
        }
        #[cfg(target_endian = "big")]
        {
            bytes[size_of::<usize>() - 1] = tag;
            bytes[..string.len()].copy_from_slice(string);
        }
        // SAFETY: The constructed word is AK's short-string representation and owns no allocation.
        unsafe { Self::from_raw_owned(usize::from_ne_bytes(bytes)) }
    }

    fn create_uninitialized(length: usize, has_ascii_storage: bool) -> Self {
        assert!(length < UNKNOWN_CODE_POINT_LENGTH as usize);
        let unit_size = if has_ascii_storage { 1 } else { size_of::<u16>() };
        assert!(
            size_of::<Utf16StringDataHeader>()
                .checked_add(
                    length
                        .checked_mul(unit_size)
                        .expect("UTF-16 string allocation size overflow")
                )
                .is_some()
        );
        // SAFETY: C++ returns a fully initialized native string owner with trailing storage sized
        // for `length` units. The caller initializes that storage before publishing the owner.
        let raw = unsafe { ladybird_utf16_string_create_uninitialized(length, has_ascii_storage) };
        // SAFETY: The allocator transfers one ownership reference.
        unsafe { Self::from_raw_owned(raw) }
    }

    #[inline]
    fn long_storage(&self) -> *mut u8 {
        assert!(has_long_storage(self.raw_identity()));
        std::ptr::with_exposed_provenance_mut::<u8>(self.raw_identity())
            .wrapping_add(size_of::<Utf16StringDataHeader>())
    }
}

impl Utf16String {
    pub fn well_formed_from_utf16(string: &[u16]) -> Self {
        // SAFETY: The slice remains alive for the duration of the call.
        let raw = unsafe { ladybird_utf16_string_to_well_formed(string.as_ptr(), string.len()) };
        // SAFETY: The constructor transfers one ownership reference.
        unsafe { Self::from_raw_owned(raw) }
    }
}

pub fn validate_utf16(string: &[u16]) -> bool {
    // SAFETY: The slice remains alive for the duration of the call.
    unsafe { ladybird_utf16_validate(string.as_ptr(), string.len()) }
}

pub fn append_utf16_as_utf8_up_to_unpaired_surrogate(output: &mut Vec<u8>, string: &[u16]) -> usize {
    let mut valid_code_units = 0;
    // SAFETY: The slice remains alive for the duration of the call.
    unsafe { ladybird_utf16_validate_prefix(string.as_ptr(), string.len(), &raw mut valid_code_units) };
    assert!(valid_code_units <= string.len());

    output.reserve(3 * valid_code_units);
    // SAFETY: The first `valid_code_units` code units are well-formed UTF-16, of which every code unit becomes at most
    // three bytes of UTF-8, and the output has room for them past its length.
    unsafe {
        let written = ladybird_convert_valid_utf16_to_utf8(
            string.as_ptr(),
            valid_code_units,
            output.as_mut_ptr().add(output.len()),
        );
        output.set_len(output.len() + written);
    }
    valid_code_units
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct DecimalExponentialForm {
    pub sign: bool,
    pub fraction: u64,
    pub exponent: i32,
}

pub fn convert_to_decimal_exponential_form(value: f64) -> DecimalExponentialForm {
    assert!(value.is_finite());
    let mut form = DecimalExponentialForm {
        sign: false,
        fraction: 0,
        exponent: 0,
    };
    // SAFETY: The outputs live for the duration of the call.
    unsafe {
        ladybird_convert_to_decimal_exponential_form(
            value,
            &raw mut form.sign,
            &raw mut form.fraction,
            &raw mut form.exponent,
        );
    }
    form
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Base64Alphabet {
    Base64,
    Base64Url,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum LastChunkHandling {
    Loose,
    Strict,
    StopBeforePartial,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Base64DecodeError {
    ExtraBits,
    InputRemainder,
    InvalidCharacter,
    InvalidData,
}

pub struct Base64DecodeResult {
    pub bytes: Vec<u8>,
    pub read: usize,
    pub error: Option<Base64DecodeError>,
}

fn raw_units(string: Utf16StringUnits<'_>) -> (*const u8, usize, bool) {
    match string {
        Utf16StringUnits::Ascii(bytes) => (bytes.as_ptr(), bytes.len(), true),
        Utf16StringUnits::Utf16(units) => (units.as_ptr().cast(), units.len(), false),
    }
}

pub fn decode_base64(
    input: Utf16StringUnits<'_>,
    alphabet: Base64Alphabet,
    last_chunk_handling: LastChunkHandling,
    max_length: Option<usize>,
) -> Base64DecodeResult {
    let (data, length, has_ascii_storage) = raw_units(input);
    let capacity = max_length.unwrap_or_else(|| {
        // SAFETY: The string's storage remains alive for the duration of the call.
        unsafe { ladybird_size_required_to_decode_base64(data, length, has_ascii_storage) }
    });

    let mut bytes = Vec::with_capacity(capacity);
    let mut output_length = capacity;
    let mut read = 0;
    // SAFETY: The string's storage remains alive for the duration of the call, and the output has room for
    // `capacity` bytes, of which the call writes the first `output_length`.
    let error = unsafe {
        ladybird_decode_base64_into(
            data,
            length,
            has_ascii_storage,
            alphabet == Base64Alphabet::Base64Url,
            last_chunk_handling as u8,
            bytes.as_mut_ptr(),
            &raw mut output_length,
            &raw mut read,
        )
    };
    assert!(output_length <= capacity);
    // SAFETY: The call initialized the first `output_length` bytes.
    unsafe { bytes.set_len(output_length) };

    let error = match error {
        0 => None,
        1 => Some(Base64DecodeError::ExtraBits),
        2 => Some(Base64DecodeError::InputRemainder),
        3 => Some(Base64DecodeError::InvalidCharacter),
        _ => Some(Base64DecodeError::InvalidData),
    };
    Base64DecodeResult { bytes, read, error }
}

pub fn encode_base64(input: &[u8], alphabet: Base64Alphabet, omit_padding: bool) -> Utf16String {
    // SAFETY: The slice remains alive for the duration of the call.
    let raw = unsafe {
        ladybird_encode_base64_to_utf16(
            input.as_ptr(),
            input.len(),
            alphabet == Base64Alphabet::Base64Url,
            omit_padding,
        )
    };
    // SAFETY: The constructor transfers one ownership reference.
    unsafe { Utf16String::from_raw_owned(raw) }
}

impl Utf16FlyString {
    /// Creates a string through AK's authoritative UTF-8 fly-string table.
    pub fn from_utf8(string: &str) -> Self {
        if let Some(raw) = utf16_short_string_raw(string) {
            // SAFETY: Short strings own no allocation or reference count.
            return Self(unsafe { OwnedUtf16String::from_raw(raw) });
        }

        // SAFETY: The string's storage remains alive for the duration of the call.
        let raw = unsafe { ladybird_utf16_fly_string_from_utf8(string.as_ptr(), string.len()) };
        // SAFETY: The constructor transfers one ownership reference.
        unsafe { Self::from_raw_owned(raw) }
    }

    /// Creates a string through AK's authoritative UTF-16 fly-string table.
    pub fn from_utf16(string: &[u16]) -> Self {
        // SAFETY: The slice remains alive for the duration of the call.
        let raw = unsafe { ladybird_utf16_fly_string_from_utf16(string.as_ptr(), string.len()) };
        // SAFETY: The constructor transfers one ownership reference.
        unsafe { Self::from_raw_owned(raw) }
    }
}

/// Returns AK's one-word representation for a short ASCII string.
pub const fn utf16_short_string_raw(string: &str) -> Option<usize> {
    let bytes = string.as_bytes();
    if bytes.len() >= size_of::<usize>() {
        return None;
    }

    let mut raw = (bytes.len() << SHORT_STRING_BYTE_COUNT_SHIFT) | SHORT_STRING_FLAG;
    let mut index = 0;
    while index < bytes.len() {
        if !bytes[index].is_ascii() {
            return None;
        }
        #[cfg(target_endian = "little")]
        {
            raw |= (bytes[index] as usize) << ((index + 1) * 8);
        }
        #[cfg(target_endian = "big")]
        {
            raw |= (bytes[index] as usize) << ((size_of::<usize>() - index - 1) * 8);
        }
        index += 1;
    }
    Some(raw)
}

impl PartialEq for Utf16String {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        // Mirrors AK::Utf16StringBase::operator==(): the same word is the same string, and short strings, whose
        // characters are in the word itself, are equal only if their words are.
        let (left, right) = (self.raw_identity(), other.raw_identity());
        if left == right {
            return true;
        }
        if left & SHORT_STRING_FLAG != 0 && right & SHORT_STRING_FLAG != 0 {
            return false;
        }
        match (self.as_units(), other.as_units()) {
            (Utf16StringUnits::Ascii(left), Utf16StringUnits::Ascii(right)) => left == right,
            (Utf16StringUnits::Utf16(left), Utf16StringUnits::Utf16(right)) => left == right,
            (Utf16StringUnits::Ascii(left), Utf16StringUnits::Utf16(right))
            | (Utf16StringUnits::Utf16(right), Utf16StringUnits::Ascii(left)) => {
                left.len() == right.len() && left.iter().zip(right).all(|(left, right)| u16::from(*left) == *right)
            }
        }
    }
}

impl Eq for Utf16String {}

impl PartialEq for Utf16FlyString {
    #[inline]
    fn eq(&self, other: &Self) -> bool {
        self.raw_identity() == other.raw_identity() || (self.is_empty() && other.is_empty())
    }
}

impl Eq for Utf16FlyString {}

impl std::hash::Hash for Utf16FlyString {
    #[inline]
    fn hash<H: std::hash::Hasher>(&self, state: &mut H) {
        self.raw_identity().hash(state);
    }
}

impl From<Utf16FlyString> for Utf16String {
    #[inline]
    fn from(string: Utf16FlyString) -> Self {
        // SAFETY: `into_raw` transfers the existing reference to this owner.
        unsafe { Self::from_raw_owned(string.into_raw()) }
    }
}

impl From<&Utf16FlyString> for Utf16String {
    #[inline]
    fn from(string: &Utf16FlyString) -> Self {
        // SAFETY: The source owner keeps the raw string alive while adding a reference.
        unsafe { reference_utf16_string(string.raw_identity()) };
        // SAFETY: The new reference is transferred to this owner.
        unsafe { Self::from_raw_owned(string.raw_identity()) }
    }
}

#[inline]
fn has_long_storage(raw: usize) -> bool {
    raw != 0 && raw & SHORT_STRING_FLAG == 0
}

/// Adds an ownership reference to a raw `AK::Utf16String` representation.
///
/// # Safety
///
/// `raw` must be zero, a valid short string, or a live long string allocation.
#[inline]
pub unsafe fn reference_utf16_string(raw: usize) {
    if !has_long_storage(raw) {
        return;
    }

    // SAFETY: The caller guarantees that `raw` points to a live long string allocation.
    let header = unsafe { &*std::ptr::with_exposed_provenance::<Utf16StringDataHeader>(raw) };
    let result = header
        .reference_count
        .fetch_update(Ordering::Relaxed, Ordering::Relaxed, |count| {
            (count != 0).then(|| count.checked_add(1)).flatten()
        });
    assert!(result.is_ok(), "invalid UTF-16 string reference count");
}

/// Releases an ownership reference to a raw `AK::Utf16String` representation.
/// Calls `release_last` when C++ must perform the potentially final release.
///
/// # Safety
///
/// `raw` must be zero, a valid short string, or a live long string allocation
/// for which the caller owns one reference. `release_last` must perform the
/// release-ordered decrement and acquire fence required before destruction.
#[inline]
pub unsafe fn release_utf16_string_with(raw: usize, release_last: impl FnOnce(usize)) {
    if !has_long_storage(raw) {
        return;
    }

    // SAFETY: The caller guarantees that `raw` points to a live long string allocation.
    let header = unsafe { &*std::ptr::with_exposed_provenance::<Utf16StringDataHeader>(raw) };
    loop {
        let reference_count = header.reference_count.load(Ordering::Relaxed);
        assert!(reference_count != 0, "invalid UTF-16 string reference count");
        if reference_count == 1 {
            // The callback performs the final release decrement and acquire fence.
            release_last(raw);
            return;
        }
        if header
            .reference_count
            .compare_exchange_weak(
                reference_count,
                reference_count - 1,
                Ordering::Release,
                Ordering::Relaxed,
            )
            .is_ok()
        {
            return;
        }
    }
}

/// Decodes the storage referenced by a raw `AK::Utf16String` representation.
///
/// # Safety
///
/// `raw` must remain at a stable address for the returned lifetime. Its value
/// must be zero, a valid short string, or a live long string allocation that
/// remains alive for the returned lifetime.
#[inline]
pub unsafe fn utf16_string_units(raw: &usize) -> Utf16StringUnits<'_> {
    if *raw == 0 {
        return Utf16StringUnits::Ascii(&[]);
    }

    if !has_long_storage(*raw) {
        let bytes = raw.to_ne_bytes();
        #[cfg(target_endian = "little")]
        let tag = bytes[0];
        #[cfg(target_endian = "big")]
        let tag = bytes[size_of::<usize>() - 1];
        let length = usize::from(tag >> SHORT_STRING_BYTE_COUNT_SHIFT);
        assert!(length < size_of::<usize>());

        let pointer = std::ptr::from_ref(raw).cast::<u8>();
        #[cfg(target_endian = "little")]
        // SAFETY: A short string stores its bytes after the tag byte.
        let pointer = unsafe { pointer.add(1) };

        // SAFETY: The short-string bytes are stored inline in `raw` and `length`
        // is constrained to the remaining bytes in the word.
        return Utf16StringUnits::Ascii(unsafe { std::slice::from_raw_parts(pointer, length) });
    }

    // SAFETY: The caller guarantees that `raw` points to a live long string allocation.
    let header = unsafe { &*std::ptr::with_exposed_provenance::<Utf16StringDataHeader>(*raw) };
    let length = header.length_in_code_units as usize;
    let storage = std::ptr::from_ref(header)
        .cast::<u8>()
        .wrapping_add(size_of::<Utf16StringDataHeader>());
    if header.flags.load(Ordering::Acquire) & HAS_UTF16_STORAGE == 0 {
        // SAFETY: Long ASCII storage starts immediately after the header and contains `length` bytes.
        Utf16StringUnits::Ascii(unsafe { std::slice::from_raw_parts(storage, length) })
    } else {
        // SAFETY: Long UTF-16 storage starts immediately after the aligned header and contains
        // `length` u16 code units.
        Utf16StringUnits::Utf16(unsafe { std::slice::from_raw_parts(storage.cast::<u16>(), length) })
    }
}
