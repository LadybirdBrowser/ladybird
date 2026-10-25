/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::mem::MaybeUninit;
use std::ptr::NonNull;

/// What the layout stage keeps for itself: the slot-indexed table the runs of a layout pass lend
/// their records through, and the stack the records live on. They are the stage's scratch rather
/// than arena columns: nothing outside a running pass reads them, and every run releases its
/// records before it ends, so between passes they hold no live record.
pub(crate) struct LayoutScratch {
    run_used_records: RefCell<Vec<RunRecordSlot>>,
    next_run_nonce: Cell<u64>,
    live_run_nonces: RefCell<Vec<u64>>,
    run_record_stack: RunRecordStack,
    /// The intrinsic sizes passes measured, kept for the passes after them.
    pub(crate) intrinsic_size_caches: super::layout_node_arena::IntrinsicSizeCaches,
    /// The inline items a block container generated, kept for its later runs in the same pass. The
    /// items borrow fonts for the current pass, so the stashes are cleared when the pass ends.
    inline_item_stashes: RefCell<HashMap<NodeSlotId, super::inline_level_iterator::StashedInlineItems>>,
    measurement_memo: RefCell<MeasurementMemo>,
    /// The styles of the boxes being laid out with one of their position options, which the style reads of each box
    /// see in place of the style of its row.
    /// https://drafts.csswg.org/css-anchor-position-1/#apply-a-position-option
    position_option_styles: RefCell<Vec<(NodeSlotId, super::node_data::StylePayloadsRef)>>,
    has_position_option_styles: Cell<bool>,
    /// What each run in progress found out for the host so far, the innermost last, and what the pass found out
    /// outside any run.
    host_findings_of_runs_in_progress: RefCell<Vec<HostFindings>>,
    host_findings_outside_runs: RefCell<HostFindings>,
    /// How many runs whose results are discarded are in progress, which read no memoized layout from before them and
    /// keep none.
    discarded_runs_in_progress: Cell<u32>,
}

/// What layout found out that the host hears of once the layout that found it is committed: the nodes whose SVG
/// lengths it resolved without the container they are relative to, what the container-relative lengths it resolved
/// depend on, and the natural size it negotiated for the box of an `<svg>` document element. A run hands them to its
/// parent with its result, so a discarded result tells the host nothing.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct HostFindings {
    pub(crate) nodes_with_unresolved_container_lengths: Vec<NodeSlotId>,
    pub(crate) messages: Vec<super::commit::FfiCommitMessage>,
    pub(crate) document_svg_root_natural_size: Option<(NodeSlotId, crate::css::style::NaturalSize)>,
}

impl HostFindings {
    pub(crate) fn is_empty(&self) -> bool {
        self.nodes_with_unresolved_container_lengths.is_empty()
            && self.messages.is_empty()
            && self.document_svg_root_natural_size.is_none()
    }

    pub(crate) fn note_unresolved_container_lengths(&mut self, node: NodeSlotId) {
        if !self.nodes_with_unresolved_container_lengths.contains(&node) {
            self.nodes_with_unresolved_container_lengths.push(node);
        }
    }

    pub(crate) fn note_container_length_dependencies(
        &mut self,
        element: crate::css::style::tree::StyleNodeID,
        bases: &super::svg_formatting_context::FfiContainerLengthBases,
    ) {
        use super::commit::{FfiCommitMessage, FfiCommitMessageKind};
        self.note_message(FfiCommitMessage::new(
            element.raw(),
            FfiCommitMessageKind::StyleDependsOnSizeContainerQuery,
        ));
        for (container, awaits_layout) in [
            (bases.width_query_container, bases.width_query_container_awaits_layout),
            (bases.height_query_container, bases.height_query_container_awaits_layout),
        ] {
            if container == 0 {
                continue;
            }
            self.note_message(FfiCommitMessage::new(
                container,
                FfiCommitMessageKind::SizeQueryContainerResolvedAgainst,
            ));
            if awaits_layout {
                self.note_message(FfiCommitMessage::new(
                    container,
                    FfiCommitMessageKind::ContainerQueryEvaluationNeededAfterLayout,
                ));
            }
        }
    }

    fn note_message(&mut self, message: super::commit::FfiCommitMessage) {
        if !self.messages.contains(&message) {
            self.messages.push(message);
        }
    }

    pub(crate) fn receive(&mut self, other: HostFindings) {
        for node in other.nodes_with_unresolved_container_lengths {
            self.note_unresolved_container_lengths(node);
        }
        for message in other.messages {
            self.note_message(message);
        }
        if other.document_svg_root_natural_size.is_some() {
            self.document_svg_root_natural_size = other.document_svg_root_natural_size;
        }
    }
}

#[derive(Default)]
struct MeasurementMemo {
    depth: usize,
    runs: HashMap<NodeSlotId, Vec<MemoizedMeasurement>>,
}

struct MemoizedMeasurement {
    purpose: super::formatting_context::LayoutPurpose,
    layout_mode: super::formatting_context::LayoutMode,
    key: super::fc_run_cache::FcRunCacheKey,
    outputs: super::formatting_context::RunOutputs,
}

// Between passes the scratch holds no record, so it can move to the thread a layout stage runs on.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<LayoutScratch>();
};

/// Which run holds a slot's record, and where on the record stack it is. The records stay on the
/// stack, so the table holds no reference.
#[derive(Clone, Copy, Default)]
struct RunRecordSlot {
    nonce: u64, // 0 = vacant
    record: u32,
}

impl Default for LayoutScratch {
    fn default() -> Self {
        Self {
            run_used_records: RefCell::new(Vec::new()),
            next_run_nonce: Cell::new(1),
            live_run_nonces: RefCell::new(Vec::new()),
            run_record_stack: RunRecordStack::default(),
            intrinsic_size_caches: Default::default(),
            inline_item_stashes: RefCell::new(HashMap::default()),
            measurement_memo: RefCell::new(MeasurementMemo::default()),
            position_option_styles: RefCell::new(Vec::new()),
            has_position_option_styles: Cell::new(false),
            host_findings_of_runs_in_progress: RefCell::new(Vec::new()),
            host_findings_outside_runs: RefCell::new(HostFindings::default()),
            discarded_runs_in_progress: Cell::new(0),
        }
    }
}

impl LayoutScratch {
    /// Makes the style reads of `node` see `style` until [`Self::pop_position_option_style`].
    pub(crate) fn push_position_option_style(&self, node: NodeSlotId, style: super::node_data::StylePayloadsRef) {
        self.position_option_styles.borrow_mut().push((node, style));
        self.has_position_option_styles.set(true);
    }

    pub(crate) fn pop_position_option_style(&self, node: NodeSlotId) {
        let mut styles = self.position_option_styles.borrow_mut();
        let popped = styles.pop();
        debug_assert!(popped.is_some_and(|(popped, _)| popped == node));
        self.has_position_option_styles.set(!styles.is_empty());
    }

    /// The style the style reads of `node` see in place of the style of its row, if any.
    #[inline]
    pub(crate) fn position_option_style(&self, node: NodeSlotId) -> Option<super::node_data::StylePayloadsRef> {
        if !self.has_position_option_styles.get() {
            return None;
        }
        self.position_option_styles
            .borrow()
            .iter()
            .rev()
            .find(|(styled, _)| *styled == node)
            .map(|(_, style)| *style)
    }

    pub(crate) fn store_inline_item_stash(
        &self,
        block_container: NodeSlotId,
        stash: super::inline_level_iterator::StashedInlineItems,
    ) {
        if self.reads_no_memoized_layout() {
            return;
        }
        self.inline_item_stashes.borrow_mut().insert(block_container, stash);
    }

    pub(crate) fn inline_item_stash(
        &self,
        block_container: NodeSlotId,
    ) -> Option<super::inline_level_iterator::StashedInlineItems> {
        if self.reads_no_memoized_layout() {
            return None;
        }
        self.inline_item_stashes.borrow().get(&block_container).cloned()
    }

    /// Lays out `run`, whose result is discarded, without reading memoized layout from before it or keeping any, so
    /// that it computes everything afresh and leaves nothing for the runs after it.
    pub(crate) fn lay_out_discarded_run<R>(&self, run: impl FnOnce() -> R) -> R {
        let in_progress = &self.discarded_runs_in_progress;
        in_progress.set(in_progress.get() + 1);
        let result = self.intrinsic_size_caches.set_aside_while(run);
        in_progress.set(in_progress.get() - 1);
        result
    }

    /// Whether a run whose result is discarded is in progress, which reads no memoized layout from before it and keeps
    /// none: the run cache, the in-flow block cache, inline item stashes, memoized measurements and intrinsic sizes.
    pub(crate) fn reads_no_memoized_layout(&self) -> bool {
        self.discarded_runs_in_progress.get() > 0
    }

    /// Notes what the innermost run in progress found out for the host, or the pass outside any run.
    pub(crate) fn note_host_findings(&self, note: impl FnOnce(&mut HostFindings)) {
        let mut runs = self.host_findings_of_runs_in_progress.borrow_mut();
        match runs.last_mut() {
            Some(findings) => note(findings),
            None => note(&mut self.host_findings_outside_runs.borrow_mut()),
        }
    }

    /// Hands what a finished run found out to the run in progress that used its result.
    pub(crate) fn receive_host_findings(&self, findings: Option<Box<HostFindings>>) {
        if let Some(findings) = findings {
            self.note_host_findings(|into| into.receive(*findings));
        }
    }

    /// What the pass found out outside any run, which its driver reports itself.
    pub(crate) fn take_host_findings_outside_runs(&self) -> HostFindings {
        std::mem::take(&mut self.host_findings_outside_runs.borrow_mut())
    }

    pub(crate) fn begin_measurement_memo(&self) {
        self.measurement_memo.borrow_mut().depth += 1;
    }

    pub(crate) fn end_measurement_memo(&self) {
        let mut memo = self.measurement_memo.borrow_mut();
        memo.depth -= 1;
        if memo.depth == 0 {
            memo.runs.clear();
        }
    }

    pub(crate) fn memoizes_measurements(&self) -> bool {
        self.measurement_memo.borrow().depth > 0 && !self.reads_no_memoized_layout()
    }

    pub(super) fn memoized_measurement(
        &self,
        box_: NodeSlotId,
        purpose: super::formatting_context::LayoutPurpose,
        layout_mode: super::formatting_context::LayoutMode,
        key: &super::fc_run_cache::FcRunCacheKey,
    ) -> Option<super::formatting_context::RunOutputs> {
        self.measurement_memo
            .borrow()
            .runs
            .get(&box_)?
            .iter()
            .find(|memoized| memoized.purpose == purpose && memoized.layout_mode == layout_mode && memoized.key == *key)
            .map(|memoized| memoized.outputs.clone())
    }

    pub(super) fn memoize_measurement(
        &self,
        box_: NodeSlotId,
        purpose: super::formatting_context::LayoutPurpose,
        layout_mode: super::formatting_context::LayoutMode,
        key: super::fc_run_cache::FcRunCacheKey,
        outputs: &super::formatting_context::RunOutputs,
    ) {
        let mut memo = self.measurement_memo.borrow_mut();
        if memo.depth == 0 {
            return;
        }
        memo.runs.entry(box_).or_default().push(MemoizedMeasurement {
            purpose,
            layout_mode,
            key,
            outputs: outputs.clone(),
        });
    }

    /// Lets go of what the pass that just committed kept for itself.
    pub(crate) fn end_layout_pass(&self) {
        self.inline_item_stashes.borrow_mut().clear();
        self.run_record_stack.release_spare_chunks();
    }

    fn begin_run(&self) -> u64 {
        let nonce = self.next_run_nonce.get();
        self.next_run_nonce
            .set(nonce.checked_add(1).expect("layout run nonce space exhausted"));
        self.live_run_nonces.borrow_mut().push(nonce);
        self.host_findings_of_runs_in_progress
            .borrow_mut()
            .push(HostFindings::default());
        nonce
    }

    fn end_run(&self, nonce: u64) {
        let ended = self.live_run_nonces.borrow_mut().pop();
        assert_eq!(ended, Some(nonce), "layout runs ended out of order");
        self.host_findings_of_runs_in_progress.borrow_mut().pop();
    }

    fn innermost_run_nonce(&self) -> Option<u64> {
        self.live_run_nonces.borrow().last().copied()
    }

    fn run_record(&self, slot_index: u32, run_nonce: u64) -> Option<u32> {
        let records = self.run_used_records.borrow();
        let slot = records.get(slot_index as usize)?;
        (slot.nonce == run_nonce).then_some(slot.record)
    }

    fn claim_run_record(&self, slot_index: u32, run_nonce: u64, record: u32) -> RunRecordClaim {
        let mut records = self.run_used_records.borrow_mut();
        // The table grows with the slot space: nearly every slot gets a run record each layout
        // pass, and the table outlives the pass, so this resizes rarely.
        if records.len() <= slot_index as usize {
            records.resize_with(slot_index as usize + 1, RunRecordSlot::default);
        }
        let slot = &mut records[slot_index as usize];
        if slot.nonce == run_nonce {
            return RunRecordClaim::AlreadyClaimed;
        }
        // Runs nest, so the nonces of the runs in progress ascend. An entry left by a run that
        // returned is free.
        if slot.nonce != 0 && self.live_run_nonces.borrow().binary_search(&slot.nonce).is_ok() {
            return RunRecordClaim::HeldByEnclosingRun;
        }
        *slot = RunRecordSlot {
            nonce: run_nonce,
            record,
        };
        RunRecordClaim::Claimed
    }
}

/// The per-run registry of UsedValues records, found through the slot-indexed side table in the
/// layout stage's scratch. A run holds its root's record itself and allocates every other record
/// on the scratch's record stack, which releases them when the run returns. The scoped
/// constructors lend records to the run, so callers cannot extend the registry's lifetime.
pub(crate) struct RunRecords<'arena> {
    root: Node,
    root_used: Option<&'arena UsedValues>,
    scratch: &'arena LayoutScratch,
    nonce: u64,
    stack_length_at_start: usize,
    records_outside_table: RefCell<HashMap<Node, NonNull<UsedValues>>>,
    // Wrapper sizing hands this state to the wrapper's child run, which consumes it while laying
    // out the table box. Keeping it on the run prevents measurement state from escaping a pass.
    table_inline_layouts: RefCell<HashMap<Node, table_formatting_context::TableInlineLayout>>,
    content_sizing: ContentSizing,
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ContentSizing {
    Layout,
    RootContributions,
    RootContentInlineSizes,
}

enum RunRecordClaim {
    Claimed,
    AlreadyClaimed,
    HeldByEnclosingRun,
}

impl<'arena> RunRecords<'arena> {
    /// Runs `run` with the records of a run rooted at `root`, and the pass as that run sees it.
    pub(crate) fn with_root<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_used: &'arena UsedValues,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some(root_used),
            ContentSizing::Layout,
            run,
        )
    }

    pub(crate) fn with_content_sizing_root<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_used: &'arena UsedValues,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some(root_used),
            ContentSizing::RootContributions,
            run,
        )
    }

    pub(crate) fn with_content_inline_sizes_root<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_used: &'arena UsedValues,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some(root_used),
            ContentSizing::RootContentInlineSizes,
            run,
        )
    }

    pub(crate) fn with_unrooted<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(callbacks, root, root_containing_block, None, ContentSizing::Layout, run)
    }

    fn enter<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_used: Option<&'arena UsedValues>,
        content_sizing: ContentSizing,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        debug_assert!(
            !callbacks.arena().has_intrinsic_size_cache_drops(),
            "the layout stage drops released intrinsic size cache entries before its runs read a cache"
        );
        let scratch = callbacks.scratch();
        let records = Self {
            root,
            root_used,
            scratch,
            nonce: scratch.begin_run(),
            stack_length_at_start: scratch.run_record_stack.length(),
            records_outside_table: RefCell::new(HashMap::default()),
            table_inline_layouts: RefCell::new(HashMap::default()),
            content_sizing,
        };
        run(&records, callbacks.for_run(root, root_containing_block))
    }

    /// What this run found out for the host, which its outputs carry to the run that uses its result.
    pub(crate) fn take_host_findings(&self) -> Option<Box<HostFindings>> {
        assert_eq!(
            self.scratch.innermost_run_nonce(),
            Some(self.nonce),
            "only the innermost layout run may hand over what it found out"
        );
        let findings = std::mem::take(
            self.scratch
                .host_findings_of_runs_in_progress
                .borrow_mut()
                .last_mut()
                .expect("a run in progress has a findings entry"),
        );
        (!findings.is_empty()).then(|| Box::new(findings))
    }

    pub(crate) fn is_content_sizing(&self) -> bool {
        self.content_sizing != ContentSizing::Layout
    }

    pub(crate) fn measures_content_inline_sizes_of(&self, node: Node) -> bool {
        self.content_sizing == ContentSizing::RootContentInlineSizes && node == self.root
    }

    pub(crate) fn register(&self, node: Node, used: UsedValues) -> &UsedValues {
        let slot_index = node.slot_index();
        let registered_twice = || -> ! {
            panic!(
                "slot {} registered twice in the run rooted at slot {}",
                slot_index,
                self.root.slot_index()
            )
        };
        if node == self.root && self.root_used.is_some() {
            registered_twice();
        }
        assert_eq!(
            self.scratch.innermost_run_nonce(),
            Some(self.nonce),
            "only the innermost layout run may register records"
        );
        let (index, record) = self.scratch.run_record_stack.push(used);
        match self.scratch.claim_run_record(slot_index, self.nonce, index) {
            RunRecordClaim::Claimed => {}
            RunRecordClaim::AlreadyClaimed => registered_twice(),
            RunRecordClaim::HeldByEnclosingRun => {
                if self.records_outside_table.borrow_mut().insert(node, record).is_some() {
                    registered_twice();
                }
            }
        }
        // SAFETY: The record stays on the stack until this run returns, which ends this borrow of the run.
        unsafe { record.as_ref() }
    }

    pub(crate) fn create_used_values(
        &self,
        callbacks: &LayoutPass<'_>,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &UsedValues {
        self.register(node, used_values::create_used_values(callbacks, node, constraints))
    }

    #[track_caller]
    pub(crate) fn used_values(&self, node: Node) -> &UsedValues {
        let caller = std::panic::Location::caller();
        self.used_values_if_owned(node).unwrap_or_else(|| {
            panic!(
                "the run rooted at slot {} does not own the record for slot {} (read at {caller})",
                self.root.slot_index(),
                node.slot_index(),
            )
        })
    }

    pub(crate) fn used_values_if_owned(&self, node: Node) -> Option<&UsedValues> {
        if node == self.root
            && let Some(root_used) = self.root_used
        {
            return Some(root_used);
        }
        let record = self
            .scratch
            .run_record(node.slot_index(), self.nonce)
            .map(|index| self.scratch.run_record_stack.get(index))
            .or_else(|| {
                let records = self.records_outside_table.borrow();
                if records.is_empty() {
                    return None;
                }
                records.get(&node).copied()
            })?;
        // SAFETY: This run registered the record, and it stays on the stack until this run returns.
        Some(unsafe { record.as_ref() })
    }

    pub(crate) fn root(&self) -> Node {
        self.root
    }

    pub(crate) fn store_table_inline_layout(&self, table: Node, layout: table_formatting_context::TableInlineLayout) {
        self.table_inline_layouts.borrow_mut().insert(table, layout);
    }

    pub(crate) fn take_table_inline_layout(&self, table: Node) -> Option<table_formatting_context::TableInlineLayout> {
        self.table_inline_layouts.borrow_mut().remove(&table)
    }
}

impl Drop for RunRecords<'_> {
    fn drop(&mut self) {
        // SAFETY: Only the innermost run registers records, so every record above this run's start is its own.
        //         References to them borrow this run, which is being dropped.
        unsafe { self.scratch.run_record_stack.truncate(self.stack_length_at_start) };
        self.scratch.end_run(self.nonce);
    }
}

const RECORDS_PER_STACK_CHUNK: usize = 64;

#[derive(Default)]
struct RunRecordStack {
    chunks: RefCell<Vec<NonNull<UsedValues>>>,
    length: Cell<usize>,
}

// SAFETY: The stack owns its chunks as a Vec owns its buffer, and a record lives only while the run
// that registered it borrows the scratch, so a stack that moves between threads holds no record.
unsafe impl Send for RunRecordStack {}

impl RunRecordStack {
    fn length(&self) -> usize {
        self.length.get()
    }

    /// The record at `index`, which a run in progress registered.
    fn get(&self, index: u32) -> NonNull<UsedValues> {
        let index = index as usize;
        assert!(index < self.length.get(), "a layout run record outlived its run");
        // SAFETY: The slot lies inside a chunk and holds a record, since it is below the length.
        unsafe { self.chunks.borrow()[index / RECORDS_PER_STACK_CHUNK].add(index % RECORDS_PER_STACK_CHUNK) }
    }

    fn push(&self, record: UsedValues) -> (u32, NonNull<UsedValues>) {
        let index = self.length.get();
        let chunk_index = index / RECORDS_PER_STACK_CHUNK;
        let mut chunks = self.chunks.borrow_mut();
        if chunk_index == chunks.len() {
            let chunk = Box::<[UsedValues]>::new_uninit_slice(RECORDS_PER_STACK_CHUNK);
            chunks.push(NonNull::from(Box::leak(chunk)).cast());
        }
        // SAFETY: The slot lies inside the chunk and holds no record, since it is at the stack's length.
        let slot = unsafe {
            let slot = chunks[chunk_index].add(index % RECORDS_PER_STACK_CHUNK);
            slot.write(record);
            slot
        };
        self.length.set(index + 1);
        (u32::try_from(index).expect("layout run records fit a u32 index"), slot)
    }

    /// # Safety
    ///
    /// No reference to a record at or above `length` may be live.
    unsafe fn truncate(&self, length: usize) {
        let chunks = self.chunks.borrow();
        while self.length.get() > length {
            let index = self.length.get() - 1;
            self.length.set(index);
            // SAFETY: The slot holds a record that nothing references, per this function's contract.
            unsafe {
                chunks[index / RECORDS_PER_STACK_CHUNK]
                    .add(index % RECORDS_PER_STACK_CHUNK)
                    .drop_in_place();
            }
        }
    }

    fn release_spare_chunks(&self) {
        assert_eq!(self.length.get(), 0, "layout run records outlived their pass");
        let mut chunks = self.chunks.borrow_mut();
        while chunks.len() > 1 {
            let chunk = chunks.pop().expect("the stack holds more than one chunk");
            // SAFETY: The stack is empty, so the chunk holds no records.
            unsafe { free_stack_chunk(chunk) };
        }
    }
}

impl Drop for RunRecordStack {
    fn drop(&mut self) {
        // SAFETY: Records borrow the runs that registered them, and no run outlives the arena.
        unsafe { self.truncate(0) };
        for chunk in self.chunks.get_mut().drain(..) {
            // SAFETY: The stack is empty, so the chunk holds no records.
            unsafe { free_stack_chunk(chunk) };
        }
    }
}

/// # Safety
///
/// `chunk` must come from `RunRecordStack::push` and hold no records.
unsafe fn free_stack_chunk(chunk: NonNull<UsedValues>) {
    let slots = std::ptr::slice_from_raw_parts_mut(
        chunk.as_ptr().cast::<MaybeUninit<UsedValues>>(),
        RECORDS_PER_STACK_CHUNK,
    );
    // SAFETY: The slots were allocated as a boxed slice of this length, per this function's contract.
    drop(unsafe { Box::from_raw(slots) });
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn nested_runs_leave_their_parents_records_in_place() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        let nested_used = UsedValues::default();
        let parent_nonce = RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            &root_used,
            |parent, _| {
                let child_used: *const UsedValues = parent.register(child, UsedValues::default());
                RunRecords::with_root(
                    LayoutPass::for_test(&arena, &scratch),
                    child,
                    root,
                    &nested_used,
                    |nested, _| {
                        assert!(std::ptr::eq(parent.used_values(child), child_used));
                        assert!(std::ptr::eq(parent.used_values(root), &raw const root_used));
                        assert!(std::ptr::eq(nested.used_values(child), &raw const nested_used));
                        assert!(nested.used_values_if_owned(root).is_none());
                        RunRecords::with_unrooted(
                            LayoutPass::for_test(&arena, &scratch),
                            child,
                            root,
                            |measurement, _| {
                                let measured: *const UsedValues = measurement.register(child, UsedValues::default());
                                assert!(std::ptr::eq(measurement.used_values(child), measured));
                                assert!(std::ptr::eq(parent.used_values(child), child_used));
                                assert!(std::ptr::eq(nested.used_values(child), &raw const nested_used));
                            },
                        );
                        assert!(std::ptr::eq(parent.used_values(child), child_used));
                    },
                );
                assert!(std::ptr::eq(parent.used_values(child), child_used));
                parent.nonce
            },
        );
        assert!(scratch.run_record(root.slot_index(), parent_nonce).is_none());
        assert_eq!(scratch.run_record_stack.length(), 0);
    }

    #[test]
    fn returning_runs_release_their_records_and_reuse_the_storage() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let lines = std::sync::Arc::new(crate::layout::inline_content::InlineContent::default());
        let weak_lines = std::sync::Arc::downgrade(&lines);
        let root_used = UsedValues::default();
        let first_record = RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            &root_used,
            |records, _| {
                let used = records.register(child, UsedValues::default());
                used.set_finished_line_data(lines);
                std::ptr::from_ref(used)
            },
        );
        assert!(weak_lines.upgrade().is_none());
        let second_record = RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            &root_used,
            |records, _| std::ptr::from_ref(records.register(child, UsedValues::default())),
        );
        assert_eq!(first_record, second_record);

        let nodes: Vec<_> = (0..RECORDS_PER_STACK_CHUNK * 2 + 1)
            .map(|_| arena.allocate_for_test().slot)
            .collect();
        RunRecords::with_unrooted(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            |records, _| {
                for node in nodes {
                    records.register(node, UsedValues::default());
                }
            },
        );
        assert_eq!(scratch.run_record_stack.chunks.borrow().len(), 3);
        scratch.end_layout_pass();
        assert_eq!(scratch.run_record_stack.chunks.borrow().len(), 1);
    }

    #[test]
    fn a_box_registered_by_a_returned_run_can_be_freed() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            &root_used,
            |records, _| {
                records.register(child, UsedValues::default());
            },
        );
        let _ = arena.free_subtree(child);
    }

    #[test]
    #[should_panic(expected = "registered twice")]
    fn registering_the_root_of_a_rooted_run_panics() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            &root_used,
            |records, _| {
                records.register(root, UsedValues::default());
            },
        );
    }

    #[test]
    #[should_panic(expected = "only the innermost layout run may register records")]
    fn an_enclosing_run_cannot_register_while_a_nested_run_is_in_progress() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        RunRecords::with_unrooted(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            |parent, _| {
                RunRecords::with_root(
                    LayoutPass::for_test(&arena, &scratch),
                    root,
                    NodeSlotId::INVALID,
                    &root_used,
                    |_, _| {
                        parent.register(child, UsedValues::default());
                    },
                );
            },
        );
    }

    #[test]
    fn unwinding_a_nested_run_restores_its_parent() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let root_used = UsedValues::default();
        let nested_used = UsedValues::default();
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            &root_used,
            |parent, _| {
                let child_used: *const UsedValues = parent.register(child, UsedValues::default());
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    RunRecords::with_root(
                        LayoutPass::for_test(&arena, &scratch),
                        child,
                        root,
                        &nested_used,
                        |nested, _| {
                            nested.register(root, UsedValues::default());
                            panic!("abort the nested measurement");
                        },
                    );
                }));
                assert!(result.is_err());
                assert!(std::ptr::eq(parent.used_values(root), &raw const root_used));
                assert!(std::ptr::eq(parent.used_values(child), child_used));
            },
        );
    }
}
