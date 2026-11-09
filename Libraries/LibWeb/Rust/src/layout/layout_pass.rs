/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::node_facts::ReplacedContentFacts;
use super::*;

/// Arena inputs borrowed while computing a layout result. The pass ends before commit takes a
/// mutable arena borrow, so its text and style views cannot survive commit.
///
/// A layout run reads only the subtree it lays out. Whatever it needs from above arrives through
/// its layout input, its root's used values and its run state, or was derived onto the subtree
/// before the pass, like ancestor facts. That is what lets partial relayout, the formatting context
/// run cache and the intrinsic size caches reuse a subtree's layout without looking outside it.
///
/// A pass holds no part of the host. Every host call takes the main thread token, which a pass is
/// not handed, so a pass has no way to call into the document.
///
/// Each run is handed a copy of its own, through which the code inside the run reaches what the run
/// keeps for it, so no state that the runs share tells which run is in progress.
#[derive(Clone, Copy)]
pub(crate) struct LayoutPass<'arena> {
    arena: &'arena LayoutNodeArena,
    scratch: &'arena super::run_state::LayoutScratch,
    pub(crate) initial_containing_block_inline_size: CssPixels,
    pub(crate) initial_containing_block_block_size: CssPixels,
    pub(crate) document_in_quirks_mode: bool,
    /// The frame of the formatting context the pass is laying out, in which style reads see sizes and box sides.
    pub(crate) frame: super::geometry::Frame,
    /// The root of the run this pass view was handed to and that root's containing block, where containing block
    /// searches from inside the run stop. Invalid outside any run.
    innermost_run: (Node, Node),
    /// What the run this pass view was handed to found out for the host so far, or the pass outside any run.
    host_findings: &'arena super::run_state::HostFindingsSink,
    /// The layout memos the runs this pass view reaches read and keep.
    memos: MemoScope<'arena>,
    /// The innermost of the position options the boxes this pass view lays out are laid out with.
    position_option_styles: Option<&'arena PositionOptionStyle<'arena>>,
    records_trace: bool,
    forks_runs: bool,
    worker_scratch: WorkerScratchOfThisThread,
}

const _: () = {
    const fn assert_send_and_sync<T: Send + Sync>() {}
    assert_send_and_sync::<LayoutPass<'static>>();
};

#[derive(Default)]
pub(crate) struct WorkerScratch {
    pub(super) spare_fragment_builder_buffers: std::cell::RefCell<Vec<super::fragment_tree::FragmentBuilderBuffers>>,
    pub(super) reserved_unique_identities: std::cell::Cell<std::ops::Range<u64>>,
    inline_content_preparation_buffers:
        std::cell::Cell<super::prepared_inline_content::InlineContentPreparationBuffers>,
}

thread_local! {
    static WORKER_SCRATCH_OF_THIS_THREAD: WorkerScratch = WorkerScratch::default();
}

#[derive(Clone, Copy)]
struct WorkerScratchOfThisThread {
    scratch: std::ptr::NonNull<WorkerScratch>,
    #[cfg(debug_assertions)]
    thread: std::thread::ThreadId,
}

// SAFETY: A pass view reaches the worker scratch of the thread it was pointed at only on that thread: a pass view
// handed to a task on another thread is first pointed at the scratch of that thread (debug builds check the thread on
// every use).
unsafe impl Send for WorkerScratchOfThisThread {}
// SAFETY: As above.
unsafe impl Sync for WorkerScratchOfThisThread {}

impl WorkerScratchOfThisThread {
    fn get() -> Self {
        WORKER_SCRATCH_OF_THIS_THREAD.with(|scratch| Self {
            scratch: std::ptr::NonNull::from(scratch),
            #[cfg(debug_assertions)]
            thread: std::thread::current().id(),
        })
    }

    #[inline]
    fn scratch(&self) -> &WorkerScratch {
        #[cfg(debug_assertions)]
        assert_eq!(
            self.thread,
            std::thread::current().id(),
            "a pass view reaches the worker scratch of another thread"
        );
        // SAFETY: The scratch lives as long as its thread, on which alone it is reached (see the Send impl).
        unsafe { self.scratch.as_ref() }
    }
}

/// Which layout memos a pass view reads and keeps: the run cache, the in-flow block cache, prepared inline content and
/// intrinsic sizes.
#[derive(Clone, Copy)]
enum MemoScope<'arena> {
    /// Those kept for the runs and passes after the ones that made them.
    Kept,
    /// None from before a run whose result is discarded, which keeps none after it either. It measures intrinsic sizes
    /// into caches of its own.
    Discarded(&'arena super::layout_node_arena::IntrinsicSizeCaches),
}

/// The style a box is laid out with in place of the style of its row: that of one of its position options. It links to
/// the position option the box is laid out inside of, if any.
/// https://drafts.csswg.org/css-anchor-position-1/#apply-a-position-option
struct PositionOptionStyle<'pass> {
    node: Node,
    style: super::node_data::LayoutStyleRef,
    enclosing: Option<&'pass PositionOptionStyle<'pass>>,
}

impl<'arena> LayoutPass<'arena> {
    /// A pass over `arena` and `scratch`, which notes what it finds out for the host outside any run in
    /// `host_findings_outside_runs`.
    pub(crate) fn new(
        arena: &'arena LayoutNodeArena,
        scratch: &'arena super::run_state::LayoutScratch,
        host_findings_outside_runs: &'arena super::run_state::HostFindingsSink,
        stage: super::formatting_context::LayoutStageFacts,
    ) -> Self {
        Self {
            arena,
            scratch,
            initial_containing_block_inline_size: stage.viewport_inline_size,
            initial_containing_block_block_size: stage.viewport_block_size,
            document_in_quirks_mode: stage.document_in_quirks_mode,
            frame: super::geometry::Frame::PHYSICAL,
            innermost_run: (NodeSlotId::INVALID, NodeSlotId::INVALID),
            host_findings: host_findings_outside_runs,
            memos: MemoScope::Kept,
            position_option_styles: None,
            records_trace: scratch.layout_trace.is_recording(),
            forks_runs: false,
            worker_scratch: WorkerScratchOfThisThread::get(),
        }
    }

    pub(super) fn on_this_thread(self) -> Self {
        Self {
            worker_scratch: WorkerScratchOfThisThread::get(),
            ..self
        }
    }

    #[inline]
    pub(super) fn worker_scratch(&self) -> &WorkerScratch {
        self.worker_scratch.scratch()
    }

    pub(crate) fn forking_runs(self) -> Self {
        Self {
            forks_runs: true,
            ..self
        }
    }

    pub(super) fn forks_runs(&self) -> bool {
        self.forks_runs && !self.is_discarded_run() && !self.records_trace
    }

    pub(super) fn noting_host_findings_in<'findings>(
        self,
        host_findings: &'findings super::run_state::HostFindingsSink,
    ) -> LayoutPass<'findings>
    where
        'arena: 'findings,
    {
        let pass: LayoutPass<'findings> = self;
        LayoutPass { host_findings, ..pass }
    }

    /// A pass over `arena` and `scratch` with no viewport, in no-quirks mode.
    #[cfg(test)]
    pub(crate) fn for_test(
        arena: &'arena LayoutNodeArena,
        scratch: &'arena super::run_state::LayoutScratch,
        host_findings_outside_runs: &'arena super::run_state::HostFindingsSink,
    ) -> Self {
        Self::new(
            arena,
            scratch,
            host_findings_outside_runs,
            super::formatting_context::LayoutStageFacts {
                viewport_inline_size: CssPixels::default(),
                viewport_block_size: CssPixels::default(),
                document_in_quirks_mode: false,
            },
        )
    }

    /// This pass, as the run rooted at `root` sees it, which notes what it finds out for the host in `host_findings`.
    #[inline]
    pub(super) fn for_run<'run>(
        self,
        root: Node,
        root_containing_block: Node,
        host_findings: &'run super::run_state::HostFindingsSink,
    ) -> LayoutPass<'run>
    where
        'arena: 'run,
    {
        let pass: LayoutPass<'run> = self;
        LayoutPass {
            innermost_run: (root, root_containing_block),
            host_findings,
            ..pass
        }
    }

    /// This pass, laying out in the frame of `writing_mode`.
    #[inline]
    pub(crate) fn in_frame_of(mut self, writing_mode: u8) -> Self {
        self.frame = super::geometry::Frame::of(writing_mode);
        self
    }

    #[inline]
    pub(crate) fn arena(&self) -> &'arena LayoutNodeArena {
        self.arena
    }

    /// Lays out `run`, whose result is discarded, with this pass as that run sees it: it reads no memoized layout from
    /// before it and keeps none, so that it computes everything afresh and leaves nothing for the runs after it. It
    /// measures intrinsic sizes into caches of its own, which start empty and are dropped when it returns.
    pub(crate) fn lay_out_discarded_run<R>(self, run: impl for<'discarded> FnOnce(LayoutPass<'discarded>) -> R) -> R {
        let mut intrinsic_size_caches = super::layout_node_arena::IntrinsicSizeCaches::default();
        intrinsic_size_caches.cover_slots(self.arena.slot_capacity());
        let pass: LayoutPass<'_> = self;
        run(LayoutPass {
            memos: MemoScope::Discarded(&intrinsic_size_caches),
            ..pass
        })
    }

    pub(crate) fn lays_out_with_position_option(&self) -> bool {
        self.position_option_styles.is_some()
    }

    /// Runs `run` with this pass as it lays out `node` with `style`, the style of one of its position options, which
    /// the style reads of `node` see in place of the style of its row. The payloads `style` names must stay alive
    /// until `run` returns.
    pub(crate) fn with_position_option_style<R>(
        self,
        node: Node,
        style: super::node_data::LayoutStyleRef,
        run: impl for<'option> FnOnce(LayoutPass<'option>) -> R,
    ) -> R {
        let position_option_style = PositionOptionStyle {
            node,
            style,
            enclosing: self.position_option_styles,
        };
        let pass: LayoutPass<'_> = self;
        run(LayoutPass {
            position_option_styles: Some(&position_option_style),
            ..pass
        })
    }

    /// The style the style reads of `node` see in place of the style of its row, if any.
    #[inline]
    fn position_option_style(&self, node: Node) -> Option<super::node_data::LayoutStyleRef> {
        let mut position_option_style = self.position_option_styles;
        while let Some(option) = position_option_style {
            if option.node == node {
                return Some(option.style);
            }
            position_option_style = option.enclosing;
        }
        None
    }

    /// Where this pass view records what its runs do while the document traces its layout: the document's layout trace,
    /// or nowhere inside a run whose result is discarded.
    pub(crate) fn layout_trace(&self) -> super::trace::LayoutTraceTarget<'arena> {
        if self.is_discarded_run() || !self.records_trace {
            return super::trace::LayoutTraceTarget::recording_nothing();
        }
        super::trace::LayoutTraceTarget::recording_into(&self.scratch.layout_trace)
    }

    /// The results of formatting context runs, kept for the runs and passes after them, unless this pass view is inside
    /// a run whose result is discarded.
    pub(crate) fn fc_run_cache_store(&self) -> Option<&'arena super::fc_run_cache::FcRunCacheStore> {
        self.kept_memos().then(|| self.scratch.fc_run_cache_store())
    }

    pub(crate) fn fragment_builder_scratch(&self) -> &'arena super::fragment_tree::FragmentBuilderScratch {
        self.scratch.fragment_builder_scratch()
    }

    /// The layouts of in-flow blocks, kept for the passes after the one that laid them out, unless this pass view is
    /// inside a run whose result is discarded.
    pub(crate) fn in_flow_block_cache_store(
        &self,
    ) -> Option<&'arena super::in_flow_block_cache::InFlowBlockCacheStore> {
        self.kept_memos().then(|| self.scratch.in_flow_block_cache_store())
    }

    /// Whether this pass view is inside a run whose result is discarded, which reads no memoized layout from before it
    /// and keeps none.
    pub(crate) fn is_discarded_run(&self) -> bool {
        matches!(self.memos, MemoScope::Discarded(_))
    }

    fn kept_memos(&self) -> bool {
        matches!(self.memos, MemoScope::Kept)
    }

    /// Notes what the run this pass view was handed to found out for the host, or the pass outside any run.
    pub(crate) fn note_host_findings(&self, note: impl FnOnce(&mut super::run_state::HostFindings)) {
        self.host_findings.note(note);
    }

    pub(crate) fn host_findings_noted_so_far(&self) -> u64 {
        self.host_findings.notes_so_far()
    }

    /// Hands what a finished run found out besides its result to the run this pass view was handed to, which takes in
    /// its result.
    pub(crate) fn receive_residue(&self, residue: Option<Box<super::run_state::RunResidue>>) {
        if let Some(residue) = residue {
            self.note_host_findings(|into| into.receive(residue.host_findings));
        }
    }

    /// Hands what a finished run found out to the run this pass view was handed to, which used its result.
    pub(crate) fn receive_host_findings(&self, findings: Option<Box<super::run_state::HostFindings>>) {
        if let Some(findings) = findings {
            self.note_host_findings(|into| into.receive(*findings));
        }
    }

    /// The intrinsic sizes passes measured, kept for the passes after them, or inside a run whose result is discarded,
    /// that run's own.
    pub(crate) fn intrinsic_size_caches(&self) -> &'arena super::layout_node_arena::IntrinsicSizeCaches {
        match self.memos {
            MemoScope::Kept => &self.scratch.intrinsic_size_caches,
            MemoScope::Discarded(intrinsic_size_caches) => intrinsic_size_caches,
        }
    }

    #[inline]
    pub(crate) fn node_data(&self, node: Node) -> &'arena NodeData {
        self.arena().data(node)
    }

    pub(crate) fn text_content(&self, node: Node) -> &'arena super::rendered_text::TextContent {
        self.arena
            .text_content(node)
            .expect("text node content must be synced to the arena before layout")
    }

    /// The chunks of the text row `node` for `key`, made by `compute` where the last run that chunked the row's text did
    /// so for another key.
    pub(crate) fn text_chunks(
        &self,
        node: Node,
        key: &super::rendered_text::TextChunkCacheKey,
        compute: impl FnOnce() -> super::text_chunker::ChunkedText,
    ) -> std::sync::Arc<super::rendered_text::CachedTextChunks> {
        super::parallel_layout::check_row_reached_by_forked_task(node.slot_index() as usize, true);
        self.scratch.text_memos().text_chunks(
            self.text_state_index(node),
            self.text_content(node).rendered(),
            key,
            compute,
        )
    }

    /// The inline content prepared for block containers, kept for the runs after the one that prepared it, unless this
    /// pass view is inside a run whose result is discarded.
    pub(crate) fn prepared_inline_content_memos(
        &self,
    ) -> Option<&'arena super::prepared_inline_content::PreparedInlineContentMemos> {
        self.kept_memos().then(|| self.scratch.prepared_inline_content_memos())
    }

    pub(crate) fn take_inline_content_preparation_buffers(
        &self,
    ) -> super::prepared_inline_content::InlineContentPreparationBuffers {
        self.worker_scratch().inline_content_preparation_buffers.take()
    }

    pub(crate) fn return_inline_content_preparation_buffers(
        &self,
        buffers: super::prepared_inline_content::InlineContentPreparationBuffers,
    ) {
        self.worker_scratch().inline_content_preparation_buffers.set(buffers);
    }

    /// Runs `read` with the grapheme segmenter of the text row `node`.
    pub(crate) fn with_grapheme_segmenter<R>(
        &self,
        node: Node,
        read: impl FnOnce(&super::text_chunker::GraphemeSegmenter) -> R,
    ) -> R {
        super::parallel_layout::check_row_reached_by_forked_task(node.slot_index() as usize, true);
        self.scratch.text_memos().with_grapheme_segmenter(
            self.text_state_index(node),
            self.text_content(node).rendered(),
            read,
        )
    }

    fn text_state_index(&self, node: Node) -> u32 {
        self.arena
            .text_state_index(node)
            .expect("a text row holds its text in a text state")
    }

    #[inline]
    pub(crate) fn style_view(&self, node: Node) -> ComputedValuesView<'arena> {
        self.computed_values_view_if_styled(node)
            .expect("styled node must publish its style container before layout")
    }

    pub(crate) fn replaced_content_facts(&self, node: Node) -> Option<ReplacedContentFacts> {
        self.arena.replaced_content_facts(node)
    }

    #[inline]
    pub(crate) fn has_replaced_content_facts(&self, node: Node) -> bool {
        self.arena.has_replaced_content_facts(node)
    }

    /// The style of `node`: that of its row, or the position option it is being laid out with.
    #[inline]
    pub(crate) fn computed_values_view_if_styled(&self, node: Node) -> Option<ComputedValuesView<'arena>> {
        self.row_and_style(node).1
    }

    /// The row of `node` together with its style as [`Self::computed_values_view_if_styled`] answers it, from one
    /// lookup of the row.
    #[inline]
    pub(crate) fn row_and_style(&self, node: Node) -> (&'arena NodeData, Option<ComputedValuesView<'arena>>) {
        let data = self.node_data(node);
        if let Some(style) = self.position_option_style(node) {
            // SAFETY: A position option's style stays alive while a box is laid out with it.
            return (data, Some(unsafe { style.view() }));
        }
        (data, node_facts::node_style_view(data))
    }

    pub(crate) fn can_skip_is_anonymous_text_run(&self, node: Node) -> bool {
        let data = self.node_data(node);
        if !node_facts::has_flag(data, NodeFlag::Anonymous) || data.generated_for.get() != 0 {
            return false;
        }

        let mut child = data.first_child.get();
        while !child.is_invalid() {
            let data = self.node_data(child);
            if !node_facts::kind_is_text(data.kind.get())
                || !self.text_content(child).untransformed_text_is_ascii_whitespace
            {
                return false;
            }
            child = data.next_sibling.get();
        }
        true
    }

    pub(crate) fn is_before(&self, node: Node, other: Node) -> bool {
        self.arena().is_before(node, other)
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, node: Node) -> Option<abspos_inputs::AbsposLayoutInputs> {
        let data = self.node_data(node);
        assert!(node_facts::kind_is_box(data.kind.get()));
        self.arena().saved_abspos_layout_inputs(node)
    }

    pub(crate) fn committed_fragment_link(&self, node: Node) -> Option<FragmentLink> {
        self.arena().committed_fragment_link(node)
    }

    #[inline]
    pub(crate) fn has_committed_fragment_link(&self, node: Node) -> bool {
        self.node_data(node).flags.get() & NodeFlag::HasCommittedFragmentLink as u32 != 0
    }

    #[inline]
    pub(crate) fn parent(&self, node: Node) -> Node {
        self.node_data(node).parent.get()
    }

    #[inline]
    pub(crate) fn first_child(&self, node: Node) -> Node {
        self.node_data(node).first_child.get()
    }

    #[inline]
    pub(crate) fn last_child(&self, node: Node) -> Node {
        self.node_data(node).last_child.get()
    }

    #[inline]
    pub(crate) fn next_sibling(&self, node: Node) -> Node {
        self.node_data(node).next_sibling.get()
    }

    #[inline]
    pub(crate) fn previous_sibling(&self, node: Node) -> Node {
        self.node_data(node).previous_sibling.get()
    }

    /// The children of `node`, in tree order.
    pub(crate) fn children(self, node: Node) -> impl Iterator<Item = Node> + 'arena {
        std::iter::successors(link(self.first_child(node)), move |&child| {
            link(self.next_sibling(child))
        })
    }

    pub(crate) fn siblings_from(self, first: Node, last: Node) -> impl Iterator<Item = Node> + 'arena {
        std::iter::successors(link(first), move |&child| {
            (child != last).then(|| link(self.next_sibling(child))).flatten()
        })
    }

    /// The children of `node`, last first.
    pub(crate) fn children_rev(self, node: Node) -> impl Iterator<Item = Node> + 'arena {
        std::iter::successors(link(self.last_child(node)), move |&child| {
            link(self.previous_sibling(child))
        })
    }

    pub(crate) fn in_flow_containing_block(&self, node: Node) -> Node {
        let (innermost_root, innermost_root_containing_block) = self.innermost_run;
        if node == innermost_root {
            return innermost_root_containing_block;
        }
        let mut ancestor = self.parent(node);
        while !ancestor.is_invalid() {
            let data = self.node_data(ancestor);
            if node_facts::node_forms_containing_block_for_children(data, node_facts::node_style_view(data)) {
                return ancestor;
            }
            if ancestor == innermost_root {
                return innermost_root_containing_block;
            }
            ancestor = data.parent.get();
        }
        NodeSlotId::INVALID
    }

    pub(crate) fn containing_block_for_child_run(
        &self,
        child: Node,
        participation: &ParticipationInParentFormattingContext,
    ) -> Node {
        match participation {
            ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs) => inputs.containing_block,
            _ => self.in_flow_containing_block(child),
        }
    }

    /// Whether `ancestor` is `node` or one of its ancestors. The walk stops at `root`, so `node` must be
    /// in `root`'s subtree and `ancestor` must not be above `root`.
    pub(crate) fn is_ancestor(&self, ancestor: Node, mut node: Node, root: Node) -> bool {
        while !node.is_invalid() {
            if node == ancestor {
                return true;
            }
            if node == root {
                return false;
            }
            node = self.node_data(node).parent.get();
        }
        false
    }
}

/// A node's link to another, which is none for an invalid slot.
fn link(node: Node) -> Option<Node> {
    (!node.is_invalid()).then_some(node)
}
