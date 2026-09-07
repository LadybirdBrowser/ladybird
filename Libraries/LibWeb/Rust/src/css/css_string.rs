/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Immutable strings owned by the parsed CSS graph, independent of the host's atom table.

use crate::css::css_tokenizer::TokenizerInput;
use crate::css::retained_fly_string::RetainedUtf16FlyString;
use std::sync::Arc;

struct StringData {
    units: Box<[u16]>,
}

#[repr(C)]
pub struct CssString {
    // An owned Arc<StringData>, or zero for an absent optional string.
    raw: usize,
}

impl CssString {
    pub(crate) fn from_utf16(units: &[u16]) -> Self {
        let data = Arc::new(StringData { units: units.into() });
        Self {
            raw: Arc::into_raw(data) as usize,
        }
    }

    pub(crate) fn none() -> Self {
        Self { raw: 0 }
    }

    pub(crate) fn as_ptr(&self) -> *const std::ffi::c_void {
        if self.raw == 0 {
            std::ptr::null()
        } else {
            (self as *const Self).cast()
        }
    }

    pub(crate) fn units(&self) -> &[u16] {
        if self.raw == 0 {
            return &[];
        }
        // SAFETY: Every nonzero raw value owns a reference to StringData.
        &unsafe { &*(self.raw as *const StringData) }.units
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
            ak::Utf16StringUnits::Ascii(bytes) => {
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

    /// Bind a name on the document thread for a consumer outside the parsed graph.
    /// The host atom table is not thread-safe; background parsing must only use `units()`.
    pub(crate) fn to_fly_string(&self) -> RetainedUtf16FlyString {
        if self.raw == 0 {
            return RetainedUtf16FlyString::none();
        }
        crate::css::ffi_stats::bump_cpp_callback(crate::css::ffi_stats::FfiOp::InternUtf16FlyStringCallback);
        let string = ak::Utf16FlyString::from_utf16(self.units());
        unsafe { RetainedUtf16FlyString::from_leaked_raw(string.into_raw()) }
    }
}

impl Clone for CssString {
    fn clone(&self) -> Self {
        if self.raw != 0 {
            unsafe { Arc::increment_strong_count(self.raw as *const StringData) };
        }
        Self { raw: self.raw }
    }
}

impl Drop for CssString {
    fn drop(&mut self) {
        if self.raw != 0 {
            unsafe { Arc::decrement_strong_count(self.raw as *const StringData) };
        }
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

    /// # Safety
    /// `strings` must contain `length` leaked AK::Utf16FlyString references.
    pub(crate) unsafe fn from_raw(strings: *const usize, length: usize) -> Self {
        Self::from_strings(
            (0..length)
                .map(|index| unsafe { CssString::from_leaked_raw(*strings.add(index)) })
                .collect(),
        )
    }

    pub(crate) fn as_slice(&self) -> &[CssString] {
        if self.length == 0 {
            return &[];
        }
        unsafe { std::slice::from_raw_parts(self.pointer, self.length) }
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

/// # Safety
/// `string` must point to a live CssString. The returned view borrows its storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_css_string_view(string: *const std::ffi::c_void) -> CssStringView {
    let units = unsafe { &*string.cast::<CssString>() }.units();
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
