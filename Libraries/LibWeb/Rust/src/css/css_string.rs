/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Immutable strings owned by the parsed CSS graph, independent of the host's atom table.

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use std::sync::atomic::{AtomicU32, Ordering};

/// The header of a string's one allocation, which its UTF-16 code units follow: a count of its
/// handles and its length, in 8 bytes.
#[repr(C)]
struct StringHeader {
    references: AtomicU32,
    length: u32,
}

const HEADER_BYTES: usize = size_of::<StringHeader>();

fn layout(length: usize) -> std::alloc::Layout {
    let bytes = length
        .checked_mul(size_of::<u16>())
        .and_then(|bytes| bytes.checked_add(HEADER_BYTES))
        .expect("CSS string size overflow");
    std::alloc::Layout::from_size_align(bytes, align_of::<StringHeader>()).expect("CSS string size overflow")
}

fn allocate_string(units: &[u16]) -> usize {
    let length = u32::try_from(units.len()).expect("CSS string size overflow");
    let layout = layout(units.len());
    // SAFETY: The layout is nonzero; the header and every code unit are written before any read.
    unsafe {
        let base = std::alloc::alloc(layout);
        if base.is_null() {
            std::alloc::handle_alloc_error(layout);
        }
        base.cast::<StringHeader>().write(StringHeader {
            references: AtomicU32::new(1),
            length,
        });
        std::ptr::copy_nonoverlapping(units.as_ptr(), base.add(HEADER_BYTES).cast::<u16>(), units.len());
        base as usize
    }
}

#[repr(C)]
pub struct CssString {
    // One handle to a counted allocation of a `StringHeader` and the code units, or zero for an
    // absent optional string.
    raw: usize,
}

impl CssString {
    pub(crate) fn from_utf16(units: &[u16]) -> Self {
        if units.is_empty() {
            return Self::default();
        }
        Self {
            raw: allocate_string(units),
        }
    }

    fn header(&self) -> &StringHeader {
        debug_assert_ne!(self.raw, 0);
        // SAFETY: Every nonzero handle refers to a live allocation `allocate_string` made.
        unsafe { &*(self.raw as *const StringHeader) }
    }

    pub(crate) fn none() -> Self {
        Self { raw: 0 }
    }

    pub(crate) fn as_ptr(&self) -> *const Self {
        if self.raw == 0 { std::ptr::null() } else { self }
    }

    /// The bytes of the string's counted allocation, the first time `sizer` reaches it.
    pub(crate) fn heap_size(&self, sizer: &mut crate::heap_size::HeapSizer) -> usize {
        if !sizer.first_sight(self.raw as *const u8) {
            return 0;
        }
        HEADER_BYTES + size_of_val::<[u16]>(self.units())
    }

    pub(crate) fn units(&self) -> &[u16] {
        if self.raw == 0 {
            return &[];
        }
        let length = self.header().length as usize;
        // SAFETY: The code units follow the header, `length` of them.
        unsafe { ak::slice_from_raw_parts((self.raw + HEADER_BYTES) as *const u16, length) }
    }

    /// Copy a borrowed AK string into Rust-owned storage at the host boundary.
    ///
    /// # Safety
    /// `raw` must be zero or a live AK::Utf16FlyString raw representation.
    pub(crate) unsafe fn from_borrowed_raw(raw: usize) -> Self {
        if raw == 0 {
            return Self::none();
        }
        match unsafe { ak::utf16_string_units(&raw) } {
            ak::Utf16StringUnits::Ascii(bytes) | ak::Utf16StringUnits::Latin1(bytes) => {
                Self::from_utf16(&bytes.iter().map(|&byte| u16::from(byte)).collect::<Vec<_>>())
            }
            ak::Utf16StringUnits::Utf16(units) => Self::from_utf16(units),
        }
    }

    /// Copy an owned AK string into Rust, then release the host reference.
    ///
    /// # Safety
    /// `raw` must be zero or one leaked AK::Utf16FlyString reference.
    pub(crate) unsafe fn from_leaked_raw(raw: usize) -> Self {
        let string = unsafe { Self::from_borrowed_raw(raw) };
        if raw != 0 {
            unsafe { ak::release_utf16_string_with(raw, crate::css::ffi_stats::release_utf16_fly_string) };
        }
        string
    }

    /// Intern a name for a consumer outside the parsed graph.
    pub(crate) fn to_fly_string(&self) -> RetainedUtf16FlyString {
        if self.raw == 0 {
            return RetainedUtf16FlyString::none();
        }
        RetainedUtf16FlyString::from_utf16(self.units())
    }
}

// An empty string is present, unlike the null handle used for an absent optional string.
impl Default for CssString {
    fn default() -> Self {
        static EMPTY: std::sync::OnceLock<CssString> = std::sync::OnceLock::new();
        EMPTY
            .get_or_init(|| Self {
                raw: allocate_string(&[]),
            })
            .clone()
    }
}

impl From<&[u16]> for CssString {
    fn from(units: &[u16]) -> Self {
        Self::from_utf16(units)
    }
}

impl From<Vec<u16>> for CssString {
    fn from(units: Vec<u16>) -> Self {
        Self::from_utf16(&units)
    }
}

impl std::ops::Deref for CssString {
    type Target = [u16];

    fn deref(&self) -> &Self::Target {
        self.units()
    }
}

impl AsRef<[u16]> for CssString {
    fn as_ref(&self) -> &[u16] {
        self.units()
    }
}

impl Clone for CssString {
    fn clone(&self) -> Self {
        if self.raw != 0 {
            let previous = self.header().references.fetch_add(1, Ordering::Relaxed);
            assert!(previous < u32::MAX / 2, "CSS string reference count overflow");
        }
        Self { raw: self.raw }
    }
}

impl Drop for CssString {
    fn drop(&mut self) {
        if self.raw == 0 || self.header().references.fetch_sub(1, Ordering::Release) != 1 {
            return;
        }
        std::sync::atomic::fence(Ordering::Acquire);
        let length = self.header().length as usize;
        // SAFETY: The last handle frees the allocation `allocate_string` made.
        unsafe { std::alloc::dealloc(self.raw as *mut u8, layout(length)) };
    }
}

impl PartialEq for CssString {
    fn eq(&self, other: &Self) -> bool {
        self.raw == other.raw || (self.raw != 0 && other.raw != 0 && self.units() == other.units())
    }
}

impl Eq for CssString {}

impl std::fmt::Debug for CssString {
    fn fmt(&self, formatter: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        self.units().fmt(formatter)
    }
}

impl<'a> From<&'a CssString> for TokenizerInput<'a> {
    fn from(string: &'a CssString) -> Self {
        Self::Utf16(string.units())
    }
}

#[repr(C)]
pub struct CssStringList {
    pointer: *mut CssString,
    length: usize,
}

// SAFETY: This handle owns its boxed slice and only exposes shared access to its strings.
unsafe impl Send for CssStringList where CssString: Send {}
unsafe impl Sync for CssStringList where CssString: Sync {}

impl CssStringList {
    pub(crate) fn from_strings(strings: Vec<CssString>) -> Self {
        let strings = strings.into_boxed_slice();
        let length = strings.len();
        Self {
            pointer: Box::into_raw(strings).cast(),
            length,
        }
    }

    pub(crate) fn as_slice(&self) -> &[CssString] {
        if self.length == 0 {
            return &[];
        }
        unsafe { ak::slice_from_raw_parts(self.pointer, self.length) }
    }
}

impl Clone for CssStringList {
    fn clone(&self) -> Self {
        Self::from_strings(self.as_slice().to_vec())
    }
}

impl Drop for CssStringList {
    fn drop(&mut self) {
        if !self.pointer.is_null() {
            drop(unsafe { Box::from_raw(std::ptr::slice_from_raw_parts_mut(self.pointer, self.length)) });
        }
    }
}

impl PartialEq for CssStringList {
    fn eq(&self, other: &Self) -> bool {
        self.as_slice() == other.as_slice()
    }
}

#[repr(C)]
pub struct CssStringView {
    pub data: *const u16,
    pub length: usize,
}

/// The returned view borrows the storage of `string`.
#[unsafe(no_mangle)]
pub extern "C" fn rust_css_string_view(string: &CssString) -> CssStringView {
    let units = string.units();
    CssStringView {
        data: units.as_ptr(),
        length: units.len(),
    }
}

#[cfg(test)]
mod tests {
    use super::CssString;

    #[test]
    fn string_owners_share_storage_across_threads() {
        let units = "a shared CSS identifier".encode_utf16().collect::<Vec<_>>();
        let first = CssString::from_utf16(&units);
        let second = first.clone();
        assert_eq!(first.raw, second.raw);
        drop(first);
        std::thread::spawn(move || assert_eq!(second.units(), units))
            .join()
            .unwrap();
    }

    #[test]
    fn compare_independently_parsed_strings_by_content() {
        let first = CssString::from_utf16(&[0x61, 0x62]);
        let second = CssString::from_utf16(&[0x61, 0x62]);
        assert_ne!(first.raw, second.raw);
        assert_eq!(first, second);
    }

    #[test]
    fn preserve_utf16_code_units() {
        let units = [0x61, 0xd800, 0xdc00, 0xdc00];
        let string = CssString::from_utf16(&units);
        assert_eq!(string.units(), units);
        assert_ne!(CssString::none(), CssString::from_utf16(&[]));
    }
}
