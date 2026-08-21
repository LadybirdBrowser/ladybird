/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Rust views of shared AK data structures.

use std::sync::atomic::{AtomicU32, Ordering};

const SHORT_STRING_FLAG: usize = 1;
const SHORT_STRING_BYTE_COUNT_SHIFT: u32 = 2;
const HAS_UTF16_STORAGE: u32 = 1;
const UNKNOWN_CODE_POINT_LENGTH: u32 = u32::MAX;

/// Mirrors `AK::Detail::Utf16StringDataHeader`.
#[repr(C, align(8))]
struct Utf16StringDataHeader {
    reference_count: AtomicU32,
    length_in_code_units: u32,
    length_in_code_points: AtomicU32,
    hash: AtomicU32,
    flags: AtomicU32,
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
}

#[repr(transparent)]
struct OwnedUtf16String {
    raw: usize,
    _not_send_or_sync: std::marker::PhantomData<*const ()>,
}

impl OwnedUtf16String {
    const fn empty() -> Self {
        Self {
            raw: SHORT_STRING_FLAG,
            _not_send_or_sync: std::marker::PhantomData,
        }
    }

    unsafe fn from_raw(raw: usize) -> Self {
        Self {
            raw,
            _not_send_or_sync: std::marker::PhantomData,
        }
    }

    fn into_raw(self) -> usize {
        let this = std::mem::ManuallyDrop::new(self);
        this.raw
    }

    fn as_units(&self) -> Utf16StringUnits<'_> {
        // SAFETY: This owner keeps the raw string alive for the returned lifetime.
        unsafe { utf16_string_units(&self.raw) }
    }
}

impl Clone for OwnedUtf16String {
    fn clone(&self) -> Self {
        // SAFETY: This owner keeps the raw string alive while adding a reference.
        unsafe { reference_utf16_string(self.raw) };
        // SAFETY: The new reference is transferred to the returned owner.
        unsafe { Self::from_raw(self.raw) }
    }
}

impl Drop for OwnedUtf16String {
    fn drop(&mut self) {
        // SAFETY: This object owns one reference to its raw string.
        unsafe {
            release_utf16_string_with(self.raw, |raw| ladybird_utf16_string_unref(raw));
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
            pub unsafe fn from_raw_owned(raw: usize) -> Self {
                // SAFETY: The caller transfers a valid ownership reference.
                Self(unsafe { OwnedUtf16String::from_raw(raw) })
            }

            /// Transfers this owner's existing reference to the caller.
            pub fn into_raw(self) -> usize {
                self.0.into_raw()
            }

            /// Returns the shared one-word identity without transferring ownership.
            pub fn raw_identity(&self) -> usize {
                self.0.raw
            }

            /// Borrows the shared ASCII or UTF-16 character storage directly.
            pub fn as_units(&self) -> Utf16StringUnits<'_> {
                self.0.as_units()
            }

            /// Returns whether the string has no code units.
            pub fn is_empty(&self) -> bool {
                match self.as_units() {
                    Utf16StringUnits::Ascii(units) => units.is_empty(),
                    Utf16StringUnits::Utf16(units) => units.is_empty(),
                }
            }
        }

        impl Default for $name {
            fn default() -> Self {
                Self(OwnedUtf16String::empty())
            }
        }

        impl Clone for $name {
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
            return Self::from_ascii(string.as_bytes());
        }

        let length = string.encode_utf16().count();
        let result = Self::create_uninitialized(length, false);
        let storage = result.long_storage().cast::<u16>();
        for (index, code_unit) in string.encode_utf16().enumerate() {
            // SAFETY: The allocation has space for exactly `length` UTF-16 code units.
            unsafe { storage.add(index).write(code_unit) };
        }
        result
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

    fn from_ascii(string: &[u8]) -> Self {
        if string.len() < size_of::<usize>() {
            return Self::from_short_ascii(string);
        }

        let result = Self::create_uninitialized(string.len(), true);
        // SAFETY: The source and destination are non-overlapping and the allocation has space for
        // exactly `string.len()` ASCII bytes.
        unsafe { std::ptr::copy_nonoverlapping(string.as_ptr(), result.long_storage(), string.len()) };
        result
    }

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

    fn long_storage(&self) -> *mut u8 {
        assert!(has_long_storage(self.raw_identity()));
        std::ptr::with_exposed_provenance_mut::<u8>(self.raw_identity())
            .wrapping_add(size_of::<Utf16StringDataHeader>())
    }
}

impl Utf16FlyString {
    /// Creates a string through AK's authoritative UTF-8 fly-string table.
    pub fn from_utf8(string: &str) -> Self {
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

impl PartialEq for Utf16String {
    fn eq(&self, other: &Self) -> bool {
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
    fn eq(&self, other: &Self) -> bool {
        self.raw_identity() == other.raw_identity() || (self.is_empty() && other.is_empty())
    }
}

impl Eq for Utf16FlyString {}

impl From<Utf16FlyString> for Utf16String {
    fn from(string: Utf16FlyString) -> Self {
        // SAFETY: `into_raw` transfers the existing reference to this owner.
        unsafe { Self::from_raw_owned(string.into_raw()) }
    }
}

impl From<&Utf16FlyString> for Utf16String {
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
