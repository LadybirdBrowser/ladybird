/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! Immutable command storage shared by the recording cache and C++ display list. Only bytes
//! and run metadata cross this boundary; layout state, hit paths and resource handles do not.

use super::builder::RecordedDisplayList;
use super::commands::DisplayListCommandRun;
use std::ffi::c_void;
use std::sync::Arc;

// A read-only view into retained command storage. The owner must outlive the view. An empty
// Vec's pointer is dangling, so the host never dereferences a pointer whose count is zero.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiRecordedDisplayList {
    pub bytes: *const u8,
    pub byte_count: usize,
    pub command_runs: *const DisplayListCommandRun,
    pub command_run_count: usize,
}

impl FfiRecordedDisplayList {
    pub const fn empty() -> Self {
        Self {
            bytes: std::ptr::null(),
            byte_count: 0,
            command_runs: std::ptr::null(),
            command_run_count: 0,
        }
    }
}

impl From<&RecordedDisplayList> for FfiRecordedDisplayList {
    fn from(recorded: &RecordedDisplayList) -> Self {
        Self {
            bytes: recorded.bytes.as_ptr(),
            byte_count: recorded.bytes.len(),
            command_runs: recorded.command_runs.as_ptr(),
            command_run_count: recorded.command_runs.len(),
        }
    }
}

/// # Safety
/// `storage` must own a live Arc reference to a RecordedDisplayList. The returned spans remain
/// valid until that reference is released; C++ must not modify their contents.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_command_storage_view(storage: *const c_void) -> FfiRecordedDisplayList {
    assert!(!storage.is_null());
    FfiRecordedDisplayList::from(unsafe { &*storage.cast::<RecordedDisplayList>() })
}

/// # Safety
/// `storage` is null or one unconsumed Arc reference transferred to C++. It may be released
/// from a compositor thread, independently of the layout arena and recording cache.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_release_command_storage(storage: *const c_void) {
    if !storage.is_null() {
        drop(unsafe { Arc::from_raw(storage.cast::<RecordedDisplayList>()) });
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn published_bytes_survive_the_recorder_and_can_be_released_on_another_thread() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<RecordedDisplayList>();
        let mut recorded = Arc::new(RecordedDisplayList {
            bytes: vec![1, 2, 3],
            ..Default::default()
        });
        let published = Arc::into_raw(recorded.clone());
        Arc::make_mut(&mut recorded).bytes[0] = 9;
        let view = unsafe { display_list_command_storage_view(published.cast()) };
        assert_eq!(
            unsafe { std::slice::from_raw_parts(view.bytes, view.byte_count) },
            &[1, 2, 3]
        );
        drop(recorded);
        // An FFI consumer transfers the opaque owner without lending any arena state.
        let transferred = published as usize;
        std::thread::spawn(move || {
            let storage = transferred as *const c_void;
            let view = unsafe { display_list_command_storage_view(storage) };
            assert_eq!(
                unsafe { std::slice::from_raw_parts(view.bytes, view.byte_count) },
                &[1, 2, 3]
            );
            unsafe { display_list_release_command_storage(storage) };
        })
        .join()
        .unwrap();
    }
}
