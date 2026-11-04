/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Storage bottles, https://storage.spec.whatwg.org/#storage-bottle

use core::ffi::c_void;

use ak::Utf16String;
use libjs_rust::api::Trace;

use crate::bindings::cpp_cell::CppCell;

unsafe extern "C" {
    fn web_storage_api_storage_bottle_size(bottle: *mut c_void) -> usize;
    fn web_storage_api_storage_bottle_keys(
        bottle: *mut c_void,
        keys: *mut c_void,
        append: unsafe extern "C" fn(*mut c_void, usize),
    );
    fn web_storage_api_storage_bottle_get(bottle: *mut c_void, key: usize, value: *mut usize) -> bool;
    /// 0 when the quota refuses the value, 1 for a new key, and 2 for a key that had the old value.
    fn web_storage_api_storage_bottle_set(
        bottle: *mut c_void,
        key: usize,
        value: usize,
        document_url: ak::Span<'_, u8>,
        old_value: *mut usize,
    ) -> u8;
    fn web_storage_api_storage_bottle_remove(bottle: *mut c_void, key: usize, document_url: ak::Span<'_, u8>);
    fn web_storage_api_storage_bottle_clear(bottle: *mut c_void, document_url: ak::Span<'_, u8>);
    fn web_storage_api_storage_bottle_quota(bottle: *mut c_void, quota: *mut u64) -> bool;
}

/// The value cannot be stored, as the bottle's quota does not leave room for it.
pub(crate) struct QuotaExceeded;

/// A storage bottle of LibWeb's C++ code, StorageAPI::StorageBottle. Its map is this process's mirror of the one the
/// browser process keeps.
#[derive(Trace)]
#[gc(crate = libjs_rust)]
pub(crate) struct StorageBottle(CppCell);

unsafe extern "C" fn append_key(keys: *mut c_void, key: usize) {
    // SAFETY: keys is the list of the caller, and LibWeb hands over the key.
    unsafe { (*keys.cast::<Vec<Utf16String>>()).push(Utf16String::from_raw_owned(key)) };
}

impl StorageBottle {
    /// # Safety
    ///
    /// `bottle` must be a live StorageAPI::StorageBottle.
    pub(crate) unsafe fn from_raw(bottle: *mut c_void) -> Self {
        // SAFETY: The caller passes a live bottle.
        Self(unsafe { CppCell::from_raw(bottle) })
    }

    pub(crate) fn as_ptr(&self) -> *mut c_void {
        self.0.as_ptr()
    }

    pub(crate) fn size(&self) -> usize {
        // SAFETY: The bottle is live.
        unsafe { web_storage_api_storage_bottle_size(self.as_ptr()) }
    }

    /// https://infra.spec.whatwg.org/#map-getting-the-keys
    pub(crate) fn keys(&self) -> Vec<Utf16String> {
        let mut keys: Vec<Utf16String> = Vec::new();
        // SAFETY: The bottle is live, and the keys outlive the call.
        unsafe { web_storage_api_storage_bottle_keys(self.as_ptr(), (&raw mut keys).cast(), append_key) };
        keys
    }

    pub(crate) fn get(&self, key: &Utf16String) -> Option<Utf16String> {
        let mut value = 0;
        // SAFETY: The bottle is live, and the key and the value outlive the call.
        let exists = unsafe { web_storage_api_storage_bottle_get(self.as_ptr(), key.raw_identity(), &raw mut value) };
        // SAFETY: LibWeb hands over the value it found.
        exists.then(|| unsafe { Utf16String::from_raw_owned(value) })
    }

    /// The value the key had, if it existed, or the refusal of the bottle's quota.
    pub(crate) fn set(
        &self,
        key: &Utf16String,
        value: &Utf16String,
        document_url: &str,
    ) -> Result<Option<Utf16String>, QuotaExceeded> {
        let mut old_value = 0;
        // SAFETY: The bottle is live, and the strings and the old value outlive the call.
        let result = unsafe {
            web_storage_api_storage_bottle_set(
                self.as_ptr(),
                key.raw_identity(),
                value.raw_identity(),
                ak::Span::new(document_url.as_bytes()),
                &raw mut old_value,
            )
        };
        match result {
            0 => Err(QuotaExceeded),
            1 => Ok(None),
            // SAFETY: LibWeb hands over the old value.
            _ => Ok(Some(unsafe { Utf16String::from_raw_owned(old_value) })),
        }
    }

    pub(crate) fn remove(&self, key: &Utf16String, document_url: &str) {
        // SAFETY: The bottle is live, and the strings outlive the call.
        unsafe {
            web_storage_api_storage_bottle_remove(
                self.as_ptr(),
                key.raw_identity(),
                ak::Span::new(document_url.as_bytes()),
            );
        }
    }

    pub(crate) fn clear(&self, document_url: &str) {
        // SAFETY: The bottle is live, and the URL outlives the call.
        unsafe { web_storage_api_storage_bottle_clear(self.as_ptr(), ak::Span::new(document_url.as_bytes())) };
    }

    /// https://storage.spec.whatwg.org/#storage-bottle-quota
    pub(crate) fn quota(&self) -> Option<u64> {
        let mut quota = 0;
        // SAFETY: The bottle is live, and the quota outlives the call.
        unsafe { web_storage_api_storage_bottle_quota(self.as_ptr(), &raw mut quota) }.then_some(quota)
    }
}
