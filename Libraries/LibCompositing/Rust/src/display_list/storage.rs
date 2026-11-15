/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tape of one display list as C++ holds it: a recording this process made, with the visual
//! context tree it was recorded against, or a tape that came from another process and passed
//! validation. C++ keeps the storage as an opaque handle and passes it back to the entry points of
//! this crate.

use super::builder::RecordedDisplayList;
use super::commands::{ContextRef, DisplayListCommandRun};
use super::effect_clip_plan::EffectClipPlan;
use super::summary::{DisplayListSummary, summarize};
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
        unsafe { ak::slice_from_raw_parts(self.words.as_ptr().cast(), self.size) }
    }

    fn as_mut_slice(&mut self) -> &mut [u8] {
        // SAFETY: The words hold at least `size` initialized bytes, which this borrows uniquely.
        unsafe { ak::slice_from_raw_parts_mut(self.words.as_mut_ptr().cast(), self.size) }
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
    // The tree a list this process recorded was recorded against, which it replays against and goes
    // to the compositor with. A tape from another process comes without one.
    visual_context_tree: Option<Arc<VisualContextTree>>,
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
    fn new(tape: Tape, visual_context_tree: Option<Arc<VisualContextTree>>) -> Arc<Self> {
        let view = match &tape {
            Tape::Empty => TapeView::empty(),
            Tape::Recorded(recorded) => TapeView::of(&recorded.bytes, &recorded.command_runs),
            Tape::Received { bytes, command_runs } => TapeView::of(bytes.as_slice(), command_runs),
        };
        Arc::new(Self {
            view,
            tape,
            visual_context_tree,
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

    pub fn visual_context_tree(&self) -> Option<&Arc<VisualContextTree>> {
        self.visual_context_tree.as_ref()
    }

    fn recorded_visual_context_tree(&self) -> &VisualContextTree {
        self.visual_context_tree
            .as_deref()
            .expect("a list this process recorded holds the tree it was recorded against")
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

/// A storage handle for a list recorded against `visual_context_tree`, which the caller owns.
pub fn recorded_storage_handle(
    recorded: Arc<RecordedDisplayList>,
    visual_context_tree: Arc<VisualContextTree>,
) -> *const c_void {
    into_handle(DisplayListStorage::recorded(recorded, visual_context_tree))
}

impl DisplayListStorage {
    /// A list this process recorded against `visual_context_tree`.
    pub fn recorded(recorded: Arc<RecordedDisplayList>, visual_context_tree: Arc<VisualContextTree>) -> Arc<Self> {
        Self::new(Tape::Recorded(recorded), Some(visual_context_tree))
    }

    /// An empty list, recorded against a tree with no node but the visual viewport.
    pub fn empty() -> Arc<Self> {
        Self::new(Tape::Empty, Some(Arc::new(VisualContextTree::create_identity())))
    }
}

/// An empty list, recorded against a tree with no node but the visual viewport.
#[unsafe(no_mangle)]
pub extern "C" fn display_list_storage_create_empty() -> *const c_void {
    into_handle(DisplayListStorage::new(
        Tape::Empty,
        Some(Arc::new(VisualContextTree::create_identity())),
    ))
}

/// # Safety
/// `recorded` and `visual_context_tree` must each be one unconsumed Arc reference, to a
/// RecordedDisplayList and to the tree it was recorded against, which the returned storage takes
/// over.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_adopt_recorded(
    recorded: *const c_void,
    visual_context_tree: *const c_void,
) -> *const c_void {
    assert!(!recorded.is_null() && !visual_context_tree.is_null());
    // SAFETY: The caller hands over one reference to each.
    let (recorded, visual_context_tree) = unsafe {
        (
            Arc::from_raw(recorded.cast::<RecordedDisplayList>()),
            Arc::from_raw(visual_context_tree.cast::<VisualContextTree>()),
        )
    };
    recorded_storage_handle(recorded, visual_context_tree)
}

/// # Safety
/// `recorded` and `visual_context_tree` must point to a live RecordedDisplayList and the tree it was
/// recorded against, each held by an Arc for the call. The returned storage takes a reference of its
/// own to each.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_share_recorded(
    recorded: *const c_void,
    visual_context_tree: *const c_void,
) -> *const c_void {
    assert!(!recorded.is_null() && !visual_context_tree.is_null());
    // SAFETY: The caller lends a reference to each, of which this takes one more.
    unsafe {
        Arc::increment_strong_count(recorded.cast::<RecordedDisplayList>());
        Arc::increment_strong_count(visual_context_tree.cast::<VisualContextTree>());
        display_list_storage_adopt_recorded(recorded, visual_context_tree)
    }
}

/// Whether the list was recorded against `visual_context_tree` itself, not a copy of it.
///
/// # Safety
/// `storage` must be a live storage handle, and `visual_context_tree` a tree held by an Arc or null.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_was_recorded_against(
    storage: *const c_void,
    visual_context_tree: *const c_void,
) -> bool {
    unsafe { storage_from_handle(storage) }
        .visual_context_tree
        .as_ref()
        .is_some_and(|tree| std::ptr::eq(Arc::as_ptr(tree).cast(), visual_context_tree))
}

/// # Safety
/// `storage` must be a live storage handle of a list this process recorded.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_visual_context_tree_structural_epoch(storage: *const c_void) -> u64 {
    unsafe { storage_from_handle(storage) }
        .recorded_visual_context_tree()
        .structural_epoch
}

/// The tree a list this process recorded was recorded against, serialized for the compositor.
///
/// # Safety
/// `storage` must be a live storage handle of a list this process recorded; `append` is called
/// synchronously with `sink`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_serialize_visual_context_tree(
    storage: *const c_void,
    sink: *mut c_void,
    append: unsafe extern "C" fn(*mut c_void, *const u8, usize),
) {
    let bytes = unsafe { storage_from_handle(storage) }
        .recorded_visual_context_tree()
        .to_bytes();
    // SAFETY: The C++ sink copies the bytes synchronously.
    unsafe { append(sink, bytes.as_ptr(), bytes.len()) };
}

/// Calls `visit` with `context` for the id of every image frame that a filter or backdrop filter of
/// an effect node of the list's tree draws.
///
/// # Safety
/// `storage` must be a live storage handle of a list this process recorded; `visit` is called
/// synchronously.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn display_list_storage_for_each_visual_context_tree_filter_image_frame_id(
    storage: *const c_void,
    context: *mut c_void,
    visit: unsafe extern "C" fn(*mut c_void, u64),
) {
    let tree = unsafe { storage_from_handle(storage) }.recorded_visual_context_tree();
    // SAFETY: The caller guarantees `visit` accepts `context`.
    tree.for_each_effects_filter_image_frame_id(&mut |id| unsafe { visit(context, id) });
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
            ak::slice_from_raw_parts(view.bytes, view.byte_count),
            ak::slice_from_raw_parts(view.command_runs, view.command_run_count),
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
/// The size of a tape and run table packed for transport.
pub fn packed_size(tape_size: usize, run_count: usize) -> Option<usize> {
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

impl DisplayListStorage {
    /// The size of the tape and run table packed for transport.
    pub fn packed_size(&self) -> usize {
        packed_size(self.bytes().len(), self.command_runs().len()).expect("a stored tape fits in memory")
    }

    /// Writes the tape and run table packed for transport to `destination`, which is `packed_size()` bytes long.
    pub fn write_packed(&self, destination: &mut [u8]) {
        let (tape, runs) = (self.bytes(), self.command_runs());
        assert_eq!(destination.len(), self.packed_size());
        let (tape_destination, runs_destination) = destination.split_at_mut(tape.len());
        tape_destination.copy_from_slice(tape);
        let (run_destinations, _) =
            runs_destination.as_chunks_mut::<{ std::mem::size_of::<DisplayListCommandRun>() }>();
        for (run, out) in runs.iter().zip(run_destinations) {
            write_run(run, out);
        }
    }

    /// The tape and run table packed for transport.
    pub fn packed(&self) -> Vec<u8> {
        let mut packed = vec![0; self.packed_size()];
        self.write_packed(&mut packed);
        packed
    }
}

/// Copies a packed tape and run table out of the bytes of a message, and checks the copy.
pub fn receive_packed(
    packed: &[u8],
    tape_size: usize,
    run_count: usize,
) -> Result<Arc<DisplayListStorage>, &'static str> {
    let size = packed_size(tape_size, run_count).ok_or("Display list sizes overflow")?;
    if size > packed.len() {
        return Err("Display list sizes exceed the bytes that hold it");
    }
    // Every run holds at least one record, so more runs than headers cannot describe the tape.
    if run_count > tape_size / super::builder::HEADER_SIZE {
        return Err("Display list run table is larger than its tape allows");
    }
    let mut bytes = AlignedBytes::try_zeroed(tape_size).ok_or("Display list is too large to receive")?;
    bytes.as_mut_slice().copy_from_slice(&packed[..tape_size]);
    let command_runs = validate_tape(bytes.as_slice(), &packed[tape_size..size])?;
    Ok(DisplayListStorage::new(Tape::Received { bytes, command_runs }, None))
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
    let destination = unsafe { ak::slice_from_raw_parts_mut(destination, size) };
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
    // SAFETY: The caller guarantees `packed` addresses `packed_size_in_bytes` readable bytes.
    match unsafe { receive_packed_from_shared_memory(packed, packed_size_in_bytes, tape_size, run_count) } {
        Ok(storage) => into_handle(storage),
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

/// Copies a packed tape and run table out of memory that another process can still write to, and checks the copy.
///
/// # Safety
/// `packed` must address `packed_size_in_bytes` readable bytes, or be null with a size of zero.
pub unsafe fn receive_packed_from_shared_memory(
    packed: *const u8,
    packed_size_in_bytes: usize,
    tape_size: usize,
    run_count: usize,
) -> Result<Arc<DisplayListStorage>, &'static str> {
    let size = packed_size(tape_size, run_count).ok_or("Display list sizes overflow")?;
    if size > packed_size_in_bytes {
        return Err("Display list sizes exceed the buffer that holds it");
    }
    // Every run holds at least one record, so more runs than headers cannot describe the tape.
    if run_count > tape_size / super::builder::HEADER_SIZE {
        return Err("Display list run table is larger than its tape allows");
    }
    let mut bytes = AlignedBytes::try_zeroed(tape_size).ok_or("Display list is too large to receive")?;
    let mut run_bytes = Vec::new();
    run_bytes
        .try_reserve_exact(size - tape_size)
        .map_err(|_| "Display list is too large to receive")?;
    run_bytes.resize(size - tape_size, 0);
    if size > 0 {
        // SAFETY: The caller guarantees `packed` addresses at least `size` bytes, and both copies have room for their
        // part.
        unsafe {
            std::ptr::copy_nonoverlapping(packed, bytes.as_mut_slice().as_mut_ptr(), tape_size);
            std::ptr::copy_nonoverlapping(packed.add(tape_size), run_bytes.as_mut_ptr(), size - tape_size);
        }
    }
    let command_runs = validate_tape(bytes.as_slice(), &run_bytes)?;
    Ok(DisplayListStorage::new(Tape::Received { bytes, command_runs }, None))
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
    pub canvas_ids: *const u64,
    pub canvas_id_count: usize,
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
        canvas_ids: summary.canvas_ids.as_ptr(),
        canvas_id_count: summary.canvas_ids.len(),
    }
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
        Ok(command_runs) => into_handle(DisplayListStorage::new(
            Tape::Received {
                bytes: pending.bytes,
                command_runs,
            },
            None,
        )),
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
        let tree = Arc::into_raw(Arc::new(VisualContextTree::create_identity()));
        let storage = unsafe { display_list_storage_adopt_recorded(published.cast(), tree.cast()) };
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
