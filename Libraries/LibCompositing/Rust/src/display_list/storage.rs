/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tape of one display list as C++ holds it: a recording this process made, or a tape that came
//! from another process and passed validation. C++ keeps the storage as an opaque handle and passes
//! it back to the entry points of this crate.

use super::builder::RecordedDisplayList;
use super::commands::DisplayListCommandRun;
use super::effect_clip_plan::EffectClipPlan;
use super::validate::validate_tape;
use crate::visual_context::VisualContextTree;
use std::ffi::c_void;
use std::sync::{Arc, OnceLock};

// A read-only view into a storage's tape. An empty Vec's pointer is dangling, so a reader never
// dereferences a pointer whose count is zero.
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

    fn of(bytes: &[u8], command_runs: &[DisplayListCommandRun]) -> Self {
        Self {
            bytes: bytes.as_ptr(),
            byte_count: bytes.len(),
            command_runs: command_runs.as_ptr(),
            command_run_count: command_runs.len(),
        }
    }
}

// Tape bytes in memory aligned for every command struct, which a Vec<u8> does not promise.
struct AlignedBytes {
    words: Vec<u64>,
    size: usize,
}

impl AlignedBytes {
    fn try_zeroed(size: usize) -> Option<Self> {
        let mut words = Vec::new();
        words.try_reserve_exact(size.div_ceil(8)).ok()?;
        words.resize(size.div_ceil(8), 0);
        Some(Self { words, size })
    }

    fn as_slice(&self) -> &[u8] {
        // SAFETY: The words hold at least `size` initialized bytes.
        unsafe { std::slice::from_raw_parts(self.words.as_ptr().cast(), self.size) }
    }
}

enum Tape {
    Empty,
    Recorded(Arc<RecordedDisplayList>),
    Received {
        bytes: AlignedBytes,
        command_runs: Vec<DisplayListCommandRun>,
    },
}

#[repr(C)]
pub struct DisplayListStorage {
    // The view comes first and keeps its layout, as code from another copy of this crate in the
    // process reads it.
    view: FfiRecordedDisplayList,
    tape: Tape,
    // The placement of the runs' clips and effects. The C++ list that owns this storage replays it
    // against trees of one structural epoch only, so one plan serves every replay.
    effect_clip_plan: OnceLock<EffectClipPlan>,
}

// SAFETY: The view's pointers address the tape the storage owns, and nothing changes the tape.
unsafe impl Send for DisplayListStorage {}
// SAFETY: As above; the plan is published through a OnceLock.
unsafe impl Sync for DisplayListStorage {}

impl DisplayListStorage {
    fn new(tape: Tape) -> Arc<Self> {
        let view = match &tape {
            Tape::Empty => FfiRecordedDisplayList::empty(),
            Tape::Recorded(recorded) => FfiRecordedDisplayList::of(&recorded.bytes, &recorded.command_runs),
            Tape::Received { bytes, command_runs } => FfiRecordedDisplayList::of(bytes.as_slice(), command_runs),
        };
        Arc::new(Self {
            view,
            tape,
            effect_clip_plan: OnceLock::new(),
        })
    }

    pub fn bytes(&self) -> &[u8] {
        match &self.tape {
            Tape::Empty => &[],
            Tape::Recorded(recorded) => &recorded.bytes,
            Tape::Received { bytes, .. } => bytes.as_slice(),
        }
    }

    pub fn command_runs(&self) -> &[DisplayListCommandRun] {
        match &self.tape {
            Tape::Empty => &[],
            Tape::Recorded(recorded) => &recorded.command_runs,
            Tape::Received { command_runs, .. } => command_runs,
        }
    }

    pub fn effect_clip_plan(&self, tree: &VisualContextTree) -> &EffectClipPlan {
        self.effect_clip_plan.get_or_init(|| {
            EffectClipPlan::new(tree, self.command_runs()).expect("a display list's runs name live nodes of its tree")
        })
    }
}

/// # Safety
///
/// `storage` must be a live storage handle for the duration of the borrow.
pub unsafe fn storage_from_handle<'a>(storage: *const c_void) -> &'a DisplayListStorage {
    assert!(!storage.is_null());
    // SAFETY: The caller guarantees a live handle.
    unsafe { &*storage.cast::<DisplayListStorage>() }
}

fn into_handle(storage: Arc<DisplayListStorage>) -> *const c_void {
    Arc::into_raw(storage).cast()
}

#[unsafe(no_mangle)]
pub extern "C" fn display_list_storage_create_empty() -> *const c_void {
    into_handle(DisplayListStorage::new(Tape::Empty))
}

/// # Safety
/// `recorded` must point to a live RecordedDisplayList held by an Arc. Returns `recorded` with one
/// more Arc reference to it, which the caller passes to `display_list_storage_adopt_recorded`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_retain_command_storage(recorded: *const c_void) -> *const c_void {
    assert!(!recorded.is_null());
    unsafe { Arc::increment_strong_count(recorded.cast::<RecordedDisplayList>()) };
    recorded
}

/// # Safety
/// `recorded` must be one unconsumed Arc reference to a RecordedDisplayList, which the returned
/// storage takes over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_adopt_recorded(recorded: *const c_void) -> *const c_void {
    assert!(!recorded.is_null());
    let recorded = unsafe { Arc::from_raw(recorded.cast::<RecordedDisplayList>()) };
    into_handle(DisplayListStorage::new(Tape::Recorded(recorded)))
}

/// # Safety
/// `storage` is null or one unconsumed storage reference. It may be released from any thread.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_release(storage: *const c_void) {
    if !storage.is_null() {
        drop(unsafe { Arc::from_raw(storage.cast::<DisplayListStorage>()) });
    }
}

/// # Safety
/// `storage` must be a live storage handle. The returned spans remain valid while it lives.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_view(storage: *const c_void) -> FfiRecordedDisplayList {
    assert!(!storage.is_null());
    // SAFETY: The view is the first field of the #[repr(C)] storage.
    unsafe { *storage.cast::<FfiRecordedDisplayList>() }
}

// A tape and run table that C++ writes into before they are checked.
pub struct PendingDisplayListStorage {
    bytes: AlignedBytes,
    run_bytes: Vec<u8>,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiPendingDisplayListStorage {
    pub pending: *mut c_void,
    pub tape: *mut u8,
    pub run_bytes: *mut u8,
}

/// Allocates room for a tape of `tape_size` bytes and a table of `run_count` runs that arrive from
/// another process. Returns a null `pending` when that much memory is not available. The caller
/// fills both and passes `pending` to `display_list_storage_finish` or
/// `display_list_storage_abandon`.
#[unsafe(no_mangle)]
pub extern "C" fn display_list_storage_begin(tape_size: usize, run_count: usize) -> FfiPendingDisplayListStorage {
    let failed = FfiPendingDisplayListStorage {
        pending: std::ptr::null_mut(),
        tape: std::ptr::null_mut(),
        run_bytes: std::ptr::null_mut(),
    };
    let Some(run_bytes_size) = run_count.checked_mul(std::mem::size_of::<DisplayListCommandRun>()) else {
        return failed;
    };
    let Some(bytes) = AlignedBytes::try_zeroed(tape_size) else {
        return failed;
    };
    let mut run_bytes = Vec::new();
    if run_bytes.try_reserve_exact(run_bytes_size).is_err() {
        return failed;
    }
    run_bytes.resize(run_bytes_size, 0);
    let mut pending = Box::new(PendingDisplayListStorage { bytes, run_bytes });
    FfiPendingDisplayListStorage {
        tape: pending.bytes.words.as_mut_ptr().cast(),
        run_bytes: pending.run_bytes.as_mut_ptr(),
        pending: Box::into_raw(pending).cast(),
    }
}

/// Checks a filled tape and run table. Returns the new storage, or null with a static error message
/// whose address and size it writes.
///
/// # Safety
/// `pending` must come from `display_list_storage_begin` and is consumed; `error` and `error_size`
/// must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_finish(
    pending: *mut c_void,
    error: *mut *const u8,
    error_size: *mut usize,
) -> *const c_void {
    assert!(!pending.is_null());
    let pending = unsafe { Box::from_raw(pending.cast::<PendingDisplayListStorage>()) };
    match validate_tape(pending.bytes.as_slice(), &pending.run_bytes) {
        Ok(command_runs) => into_handle(DisplayListStorage::new(Tape::Received {
            bytes: pending.bytes,
            command_runs,
        })),
        Err(message) => {
            // SAFETY: The caller guarantees both are writable.
            unsafe {
                *error = message.as_ptr();
                *error_size = message.len();
            }
            std::ptr::null()
        }
    }
}

/// # Safety
/// `pending` must come from `display_list_storage_begin` and is consumed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_abandon(pending: *mut c_void) {
    if !pending.is_null() {
        drop(unsafe { Box::from_raw(pending.cast::<PendingDisplayListStorage>()) });
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
        let storage = unsafe { display_list_storage_adopt_recorded(published.cast()) };
        Arc::make_mut(&mut recorded).bytes[0] = 9;
        let view = unsafe { display_list_storage_view(storage) };
        assert_eq!(
            unsafe { std::slice::from_raw_parts(view.bytes, view.byte_count) },
            &[1, 2, 3]
        );
        drop(recorded);
        // An FFI consumer transfers the opaque owner without lending any arena state.
        let transferred = storage as usize;
        std::thread::spawn(move || {
            let storage = transferred as *const c_void;
            let view = unsafe { display_list_storage_view(storage) };
            assert_eq!(
                unsafe { std::slice::from_raw_parts(view.bytes, view.byte_count) },
                &[1, 2, 3]
            );
            unsafe { display_list_storage_release(storage) };
        })
        .join()
        .unwrap();
    }
}
