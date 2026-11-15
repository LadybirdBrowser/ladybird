/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

//! The tape of one display list: a recording this process made, with the visual context tree it was recorded against,
//! or a tape that came from another process and passed validation.

use super::builder::RecordedDisplayList;
use super::commands::{ContextRef, DisplayListCommandRun};
use super::effect_clip_plan::EffectClipPlan;
use super::summary::{DisplayListSummary, summarize};
use super::validate::validate_tape;
use crate::visual_context::VisualContextTree;
use std::sync::{Arc, OnceLock};

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

pub struct DisplayListStorage {
    tape: Tape,
    // The tree a list this process recorded was recorded against, which it replays against and goes
    // to the compositor with. A tape from another process comes without one.
    visual_context_tree: Option<Arc<VisualContextTree>>,
    // The placement of the runs' clips and effects. The list is replayed against trees of one structural epoch only, so
    // one plan serves every replay.
    effect_clip_plan: OnceLock<EffectClipPlan>,
    summary: OnceLock<DisplayListSummary>,
}

impl DisplayListStorage {
    fn new(tape: Tape, visual_context_tree: Option<Arc<VisualContextTree>>) -> Arc<Self> {
        Arc::new(Self {
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn published_bytes_survive_the_recorder_and_can_be_released_on_another_thread() {
        fn assert_send_sync<T: Send + Sync>() {}
        assert_send_sync::<RecordedDisplayList>();
        assert_send_sync::<DisplayListStorage>();
        let mut recorded = Arc::new(RecordedDisplayList {
            bytes: vec![1, 2, 3],
            ..Default::default()
        });
        let storage = DisplayListStorage::recorded(recorded.clone(), Arc::new(VisualContextTree::create_identity()));
        Arc::make_mut(&mut recorded).bytes[0] = 9;
        assert_eq!(storage.bytes(), &[1, 2, 3]);
        drop(recorded);
        std::thread::spawn(move || {
            assert_eq!(storage.bytes(), &[1, 2, 3]);
            drop(storage);
        })
        .join()
        .unwrap();
    }
}
