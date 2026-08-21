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
