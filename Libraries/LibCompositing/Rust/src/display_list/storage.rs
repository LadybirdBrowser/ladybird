/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tape of one display list as C++ holds it: a recording this process made, or a tape that came
//! from another process and passed validation. C++ keeps the storage as an opaque handle and passes
//! it back to the entry points of this crate.

use super::builder::RecordedDisplayList;
use super::commands::{ContextRef, DisplayListCommandRun, DisplayListCommandType};
use super::effect_clip_plan::EffectClipPlan;
use super::summary::{
    DisplayListSummary, for_each_compositor_metadata, for_each_indexed_record,
    requires_direct_replay_without_nested_lists, summarize,
};
use super::validate::validate_tape;
use crate::visual_context::VisualContextTree;
use std::ffi::c_void;
use std::sync::{Arc, OnceLock};

// A read-only view into a storage's tape. An empty Vec's pointer is dangling, so a reader never
// dereferences a pointer whose count is zero.
#[derive(Clone, Copy, Debug)]
#[repr(C)]
struct TapeView {
    bytes: *const u8,
    byte_count: usize,
    command_runs: *const DisplayListCommandRun,
    command_run_count: usize,
}

impl TapeView {
    const fn empty() -> Self {
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
    view: TapeView,
    tape: Tape,
    // The placement of the runs' clips and effects. The C++ list that owns this storage replays it
    // against trees of one structural epoch only, so one plan serves every replay.
    effect_clip_plan: OnceLock<EffectClipPlan>,
    summary: OnceLock<DisplayListSummary>,
}

// SAFETY: The view's pointers address the tape the storage owns, and nothing changes the tape.
unsafe impl Send for DisplayListStorage {}
// SAFETY: As above; the plan is published through a OnceLock.
unsafe impl Sync for DisplayListStorage {}

impl DisplayListStorage {
    fn new(tape: Tape) -> Arc<Self> {
        let view = match &tape {
            Tape::Empty => TapeView::empty(),
            Tape::Recorded(recorded) => TapeView::of(&recorded.bytes, &recorded.command_runs),
            Tape::Received { bytes, command_runs } => TapeView::of(bytes.as_slice(), command_runs),
        };
        Arc::new(Self {
            view,
            tape,
            effect_clip_plan: OnceLock::new(),
            summary: OnceLock::new(),
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

    pub fn summary(&self) -> &DisplayListSummary {
        self.summary
            .get_or_init(|| summarize(self.bytes(), self.command_runs()))
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

/// The tape and run table of a storage. This reads only the view at the start of the storage, so
/// code from any copy of this crate in the process can call it.
///
/// # Safety
/// `storage` must be a live storage handle for the duration of the borrow.
pub unsafe fn tape_of<'a>(storage: *const c_void) -> (&'a [u8], &'a [DisplayListCommandRun]) {
    assert!(!storage.is_null());
    // SAFETY: The view is the first field of the #[repr(C)] storage, and its spans address the tape
    // the storage owns.
    unsafe {
        let view = *storage.cast::<TapeView>();
        (
            crate::ffi::ffi_slice(view.bytes, view.byte_count),
            crate::ffi::ffi_slice(view.command_runs, view.command_run_count),
        )
    }
}

/// # Safety
/// `storage` must be a live storage handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_tape_size(storage: *const c_void) -> usize {
    unsafe { storage_from_handle(storage) }.bytes().len()
}

/// # Safety
/// `storage` must be a live storage handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_run_count(storage: *const c_void) -> usize {
    unsafe { storage_from_handle(storage) }.command_runs().len()
}

// The tape and its run table packed together for transport: the tape first, then the runs.
fn packed_size(tape_size: usize, run_count: usize) -> Option<usize> {
    run_count
        .checked_mul(std::mem::size_of::<DisplayListCommandRun>())?
        .checked_add(tape_size)
}

// Writes a run field by field, so its padding is written too.
pub(super) fn write_run(run: &DisplayListCommandRun, out: &mut [u8]) {
    use std::mem::offset_of;
    out.fill(0);
    let mut put = |offset: usize, bytes: &[u8]| out[offset..offset + bytes.len()].copy_from_slice(bytes);
    let context = offset_of!(DisplayListCommandRun, context);
    let ink_bounds = offset_of!(DisplayListCommandRun, ink_bounds);
    put(offset_of!(DisplayListCommandRun, offset), &run.offset.to_ne_bytes());
    put(offset_of!(DisplayListCommandRun, size), &run.size.to_ne_bytes());
    put(
        context + offset_of!(ContextRef, spatial),
        &run.context.spatial.0.to_ne_bytes(),
    );
    put(
        context + offset_of!(ContextRef, clip),
        &run.context.clip.0.to_ne_bytes(),
    );
    put(
        context + offset_of!(ContextRef, effect),
        &run.context.effect.0.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, x),
        &run.ink_bounds.x.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, y),
        &run.ink_bounds.y.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, width),
        &run.ink_bounds.width.to_ne_bytes(),
    );
    put(
        ink_bounds + offset_of!(libgfx_rust::IntRect, height),
        &run.ink_bounds.height.to_ne_bytes(),
    );
    put(
        offset_of!(DisplayListCommandRun, has_compositor_metadata),
        &[u8::from(run.has_compositor_metadata)],
    );
}

/// The size of the storage's tape and run table packed for transport.
///
/// # Safety
/// `storage` must be a live storage handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_packed_size(storage: *const c_void) -> usize {
    let storage = unsafe { storage_from_handle(storage) };
    packed_size(storage.bytes().len(), storage.command_runs().len()).expect("a stored tape fits in memory")
}

/// Writes the storage's tape and run table packed for transport.
///
/// # Safety
/// `storage` must be a live storage handle and `destination` must address
/// `display_list_storage_packed_size(storage)` writable bytes.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_write_packed(storage: *const c_void, destination: *mut u8) {
    let storage = unsafe { storage_from_handle(storage) };
    let (tape, runs) = (storage.bytes(), storage.command_runs());
    let size = packed_size(tape.len(), runs.len()).expect("a stored tape fits in memory");
    if size == 0 {
        return;
    }
    // SAFETY: The caller guarantees `destination` addresses `size` writable bytes.
    let destination = unsafe { std::slice::from_raw_parts_mut(destination, size) };
    let (tape_destination, runs_destination) = destination.split_at_mut(tape.len());
    tape_destination.copy_from_slice(tape);
    let (run_destinations, _) = runs_destination.as_chunks_mut::<{ std::mem::size_of::<DisplayListCommandRun>() }>();
    for (run, out) in runs.iter().zip(run_destinations) {
        write_run(run, out);
    }
}

/// Copies a packed tape and run table out of memory that another process can still write to, and
/// checks the copy. Returns the new storage, or null with a static error message whose address and
/// size it writes.
///
/// # Safety
/// `packed` must address `packed_size` readable bytes, or be null with a size of zero; `error` and
/// `error_size` must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_receive_packed(
    packed: *const u8,
    packed_size_in_bytes: usize,
    tape_size: usize,
    run_count: usize,
    error: *mut *const u8,
    error_size: *mut usize,
) -> *const c_void {
    let fail = |message: &'static str| {
        // SAFETY: The caller guarantees both are writable.
        unsafe {
            *error = message.as_ptr();
            *error_size = message.len();
        }
        std::ptr::null()
    };
    let Some(size) = packed_size(tape_size, run_count) else {
        return fail("Display list sizes overflow");
    };
    if size > packed_size_in_bytes {
        return fail("Display list sizes exceed the buffer that holds it");
    }
    // Every run holds at least one record, so more runs than headers cannot describe the tape.
    if run_count > tape_size / super::builder::HEADER_SIZE {
        return fail("Display list run table is larger than its tape allows");
    }
    let pending = display_list_storage_begin(tape_size, run_count);
    if pending.pending.is_null() {
        return fail("Display list is too large to receive");
    }
    if size > 0 {
        // SAFETY: The caller guarantees `packed` addresses at least `size` bytes, and `begin` made room
        // for both parts.
        unsafe {
            std::ptr::copy_nonoverlapping(packed, pending.tape, tape_size);
            std::ptr::copy_nonoverlapping(packed.add(tape_size), pending.run_bytes, size - tape_size);
        }
    }
    unsafe { display_list_storage_finish(pending.pending, error, error_size) }
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiReferencedResourceIds {
    pub font_ids: *const u64,
    pub font_id_count: usize,
    pub image_frame_ids: *const u64,
    pub image_frame_id_count: usize,
    pub video_sink_ids: *const u64,
    pub video_sink_id_count: usize,
    pub display_list_ids: *const u64,
    pub display_list_id_count: usize,
}

/// The ids of the resources the commands of the list reference, including the commands nested in
/// others, each once. The spans remain valid while the storage lives.
///
/// # Safety
/// `storage` must be a live storage handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_referenced_resource_ids(
    storage: *const c_void,
) -> FfiReferencedResourceIds {
    let summary = unsafe { storage_from_handle(storage) }.summary();
    FfiReferencedResourceIds {
        font_ids: summary.font_ids.as_ptr(),
        font_id_count: summary.font_ids.len(),
        image_frame_ids: summary.image_frame_ids.as_ptr(),
        image_frame_id_count: summary.image_frame_ids.len(),
        video_sink_ids: summary.video_sink_ids.as_ptr(),
        video_sink_id_count: summary.video_sink_ids.len(),
        display_list_ids: summary.display_list_ids.as_ptr(),
        display_list_id_count: summary.display_list_ids.len(),
    }
}

/// # Safety
/// `storage` must be a live storage handle and `tree` a live retained tree handle whose nodes the
/// list's runs name.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_requires_direct_replay_without_nested_lists(
    storage: *const c_void,
    tree: *const c_void,
) -> bool {
    let storage = unsafe { storage_from_handle(storage) };
    let tree = unsafe { crate::ffi::tree_from_handle(tree) };
    requires_direct_replay_without_nested_lists(storage.bytes(), storage.command_runs(), tree)
}

/// Calls `visit` with each compositor metadata command, its run's context and its payload.
///
/// # Safety
/// `storage` must be a live storage handle. `visit` runs synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_for_each_compositor_metadata(
    storage: *const c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, ContextRef, DisplayListCommandType, *const u8, usize),
) {
    let storage = unsafe { storage_from_handle(storage) };
    for_each_compositor_metadata(
        storage.bytes(),
        storage.command_runs(),
        |run_context, command_type, payload| {
            // SAFETY: The caller reads the payload synchronously.
            unsafe { visit(context, run_context, command_type, payload.as_ptr(), payload.len()) };
        },
    );
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiIndexedRecordKind {
    DrawnCanvas,
    Caret,
}

/// Calls `visit` with each top-level DrawCanvas or PaintCaret record of the list: its run's context,
/// its bounding rect, and its payload.
///
/// # Safety
/// `storage` must be a live storage handle. `visit` runs synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_for_each_indexed_record(
    storage: *const c_void,
    kind: FfiIndexedRecordKind,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, ContextRef, libgfx_rust::IntRect, *const u8, usize),
) {
    let storage = unsafe { storage_from_handle(storage) };
    let summary = storage.summary();
    let records = match kind {
        FfiIndexedRecordKind::DrawnCanvas => &summary.drawn_canvases,
        FfiIndexedRecordKind::Caret => &summary.carets,
    };
    for_each_indexed_record(
        storage.bytes(),
        storage.command_runs(),
        records,
        |run_context, header, payload| {
            // SAFETY: The caller reads the payload synchronously.
            unsafe {
                visit(
                    context,
                    run_context,
                    header.bounding_rect,
                    payload.as_ptr(),
                    payload.len(),
                );
            }
        },
    );
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
    pub run_bytes_size: usize,
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
        run_bytes_size: 0,
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
        run_bytes_size,
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
        assert_eq!(unsafe { tape_of(storage) }.0, &[1, 2, 3]);
        drop(recorded);
        // An FFI consumer transfers the opaque owner without lending any arena state.
        let transferred = storage as usize;
        std::thread::spawn(move || {
            let storage = transferred as *const c_void;
            assert_eq!(unsafe { tape_of(storage) }.0, &[1, 2, 3]);
            unsafe { display_list_storage_release(storage) };
        })
        .join()
        .unwrap();
    }
}
