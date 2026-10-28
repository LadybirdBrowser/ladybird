/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::mem::MaybeUninit;
use std::ptr::NonNull;

/// What the layout stage keeps for itself: the slot-indexed table the runs of a layout pass find
/// the boxes they hold through, the stack of what they keep for those boxes, and the stack their
/// records live on. They are the stage's scratch rather than arena columns: nothing outside a
/// running pass reads them, and every run releases what it keeps before it ends, so between passes
/// they hold no live record.
pub(crate) struct LayoutScratch {
    run_kept_boxes: RefCell<Vec<RunKeptBoxSlot>>,
    next_run_nonce: Cell<u64>,
    live_run_nonces: RefCell<Vec<u64>>,
    kept_boxes: RefCell<Vec<KeptBox>>,
    run_record_stack: RunRecordStack,
    /// The intrinsic sizes passes measured, kept for the passes after them.
    pub(crate) intrinsic_size_caches: super::layout_node_arena::IntrinsicSizeCaches,
    /// The inline items a block container generated, kept for its later runs in the same pass. The
    /// items borrow fonts for the current pass, so the stashes are cleared when the pass ends.
    inline_item_stashes: RefCell<HashMap<NodeSlotId, super::inline_level_iterator::StashedInlineItems>>,
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

// Between passes the scratch holds no record, so it can move to the thread a layout stage runs on.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<LayoutScratch>();
};

/// Which run holds a slot's box, and where on the kept box stack it keeps it. The records stay on
/// the record stack, so the table holds no reference.
#[derive(Clone, Copy, Default)]
struct RunKeptBoxSlot {
    nonce: u64, // 0 = vacant
    kept_box: u32,
}

/// What a run keeps for a box it holds: what the box's parent decided about it, and how the run
/// holds the box.
struct KeptBox {
    facts_decided_by_parent: FactsDecidedByParent,
    holding: KeptBoxHolding,
}

enum KeptBoxHolding {
    /// The box's record on the record stack, and what the run of the box handed back once it
    /// returned, for a child laid out by a run of its own.
    Record {
        record: u32,
        handed_back_by_its_run: Option<ChildLaidOutByItsOwnRun>,
    },
}

/// Whether a run checks the boxes it keeps for its children against their records, which it keeps as well for now.
fn checks_kept_boxes_against_records() -> bool {
    cfg!(debug_assertions) || super::run_repeatability::verification_is_enabled()
}

impl Default for LayoutScratch {
    fn default() -> Self {
        Self {
            run_kept_boxes: RefCell::new(Vec::new()),
            next_run_nonce: Cell::new(1),
            live_run_nonces: RefCell::new(Vec::new()),
            kept_boxes: RefCell::new(Vec::new()),
            run_record_stack: RunRecordStack::default(),
            intrinsic_size_caches: Default::default(),
            inline_item_stashes: RefCell::new(HashMap::default()),
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
    /// none: the run cache, the in-flow block cache, inline item stashes and intrinsic sizes.
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

    /// Lets go of what the pass that just committed kept for itself.
    pub(crate) fn end_layout_pass(&self) {
        self.inline_item_stashes.borrow_mut().clear();
        assert!(
            self.kept_boxes.borrow().is_empty(),
            "boxes kept by layout runs outlived their pass"
        );
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

    fn run_kept_box(&self, slot_index: u32, run_nonce: u64) -> Option<u32> {
        let slots = self.run_kept_boxes.borrow();
        let slot = slots.get(slot_index as usize)?;
        (slot.nonce == run_nonce).then_some(slot.kept_box)
    }

    fn claim_run_kept_box(&self, slot_index: u32, run_nonce: u64, kept_box: u32) -> RunKeptBoxClaim {
        let mut slots = self.run_kept_boxes.borrow_mut();
        // The table grows with the slot space: nearly every slot gets a run record each layout
        // pass, and the table outlives the pass, so this resizes rarely.
        if slots.len() <= slot_index as usize {
            slots.resize_with(slot_index as usize + 1, RunKeptBoxSlot::default);
        }
        let slot = &mut slots[slot_index as usize];
        if slot.nonce == run_nonce {
            return RunKeptBoxClaim::AlreadyClaimed;
        }
        // Runs nest, so the nonces of the runs in progress ascend. An entry left by a run that
        // returned is free.
        if slot.nonce != 0 && self.live_run_nonces.borrow().binary_search(&slot.nonce).is_ok() {
            return RunKeptBoxClaim::HeldByEnclosingRun;
        }
        *slot = RunKeptBoxSlot {
            nonce: run_nonce,
            kept_box,
        };
        RunKeptBoxClaim::Claimed
    }

    fn keep_box(&self, holding: KeptBoxHolding) -> u32 {
        let mut kept_boxes = self.kept_boxes.borrow_mut();
        let index = u32::try_from(kept_boxes.len()).expect("boxes kept by layout runs fit a u32 index");
        kept_boxes.push(KeptBox {
            facts_decided_by_parent: FactsDecidedByParent::default(),
            holding,
        });
        index
    }
}

/// The per-run registry of the boxes a run holds, found through the slot-indexed side table in the
/// layout stage's scratch. A run owns its root's record, built from the input cells its caller
/// hands it, and keeps every other box it holds on the scratch's kept box stack, with its record on
/// the record stack; both release them when the run returns. The scoped constructors lend records
/// to the run, so callers cannot extend the registry's lifetime.
pub(crate) struct RunRecords<'arena> {
    root: Node,
    root_used: Option<UsedValues>,
    scratch: &'arena LayoutScratch,
    nonce: u64,
    stack_length_at_start: usize,
    kept_box_stack_length_at_start: usize,
    kept_boxes_outside_table: RefCell<HashMap<Node, u32>>,
    /// What the parent of the root of a run that owns its root record decides about it, which the kept box stack keeps
    /// for every other box.
    root_facts_decided_by_parent: Cell<FactsDecidedByParent>,
    // Wrapper sizing hands this state to the wrapper's child run, which consumes it while laying
    // out the table box. Keeping it on the run prevents measurement state from escaping a pass.
    table_inline_layouts: RefCell<HashMap<Node, table_formatting_context::TableInlineLayout>>,
    content_sizing: ContentSizing,
    /// What the run found so far about the percentage block size dependency of its root and of the boxes whose
    /// layout in it is in progress, the innermost last. Only these boxes have their dependency resolved.
    percentage_block_size_dependencies_in_progress: PercentageBlockSizeDependenciesInProgress,
}

/// Where a box is placed: the offset of its content box in the containing block it is placed in.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct BoxPlacement {
    pub(crate) content_offset_in_containing_block: FfiCssPixelPoint,
    pub(crate) containing_block: Node,
}

/// What a box's fragment shows of the layout of its parent, which decides it.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct FragmentFactsDecidedByParent {
    /// For table cells and table-column(-group) boxes: the first grid column the box occupies and the number of grid
    /// columns it spans, so painting can find the cells that originate in a column (CSS 2.2 §17.5.1).
    pub(crate) table_column_index: u32,
    pub(crate) table_column_span: u32,
    /// For table cells: whether every column the cell spans has 'visibility: collapse', which removes the cell from
    /// the display along with the columns (CSS 2.2 §17.5.5).
    pub(crate) hidden_by_collapsed_columns: bool,
    /// Whether the box lies after the clamp point of a line-clamp container, which hides it.
    pub(crate) is_invisible_for_line_clamp: bool,
    /// Whether fragmentation broke the box, so that this fragment continues one in an earlier fragmentainer, or
    /// continues in a later one.
    pub(crate) continues_from_previous_fragment: bool,
    pub(crate) continues_in_next_fragment: bool,
}

/// What the parent of a box decides about the box, kept beside the box's record.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct FactsDecidedByParent {
    pub(crate) placement: Option<BoxPlacement>,
    pub(crate) used_insets: UsedInsets,
    /// The scroll shift the anchor positioning of the box's insets compensates for.
    pub(crate) default_scroll_shift: Option<abspos_inputs::DefaultScrollShift>,
    pub(crate) fragment_facts: FragmentFactsDecidedByParent,
}

/// What the parent of a box laid out by a run of its own keeps of the box once that run returns: its sizes, size
/// constraints and box sides in the parent's frame, and its baselines.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct LaidOutChildBox {
    pub(crate) cells: used_values::RootInputCells,
    pub(crate) baselines: DerivedBaselines,
    /// For a box that establishes an orthogonal flow, its baselines along the horizontal axis of the parent's frame.
    pub(crate) orthogonal_flow_baselines: Option<DerivedBaselines>,
}

/// What the parent of a box laid out by a run of its own keeps of it once that run returns: the box, the lines the run
/// laid out in it, and the frame the box's metrics are in.
#[derive(Clone)]
pub(crate) struct ChildLaidOutByItsOwnRun {
    pub(crate) laid_out: LaidOutChildBox,
    pub(crate) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    pub(crate) frame: geometry::Frame,
}

/// The sizes, size constraints, box sides and baselines of a box as a run sees them: kept as a value for a child laid
/// out by a run of its own, or read from the record of a box the run lays out itself.
#[derive(Clone, Copy)]
pub(crate) enum BoxMetrics<'records> {
    LaidOutByItsOwnRun(LaidOutChildBox),
    LaidOutByThisRun(&'records UsedValues),
    /// A child this run sizes in cells of its own before laying it out by a run of its own.
    BeingSizedByItsParent(used_values::RootInputCells),
}

impl BoxMetrics<'_> {
    pub(crate) fn input_cells(&self) -> used_values::RootInputCells {
        match self {
            Self::LaidOutByItsOwnRun(laid_out) => laid_out.cells,
            Self::LaidOutByThisRun(record) => used_values::RootInputCells::capture(record),
            Self::BeingSizedByItsParent(cells) => *cells,
        }
    }

    pub(crate) fn content_baselines(&self) -> DerivedBaselines {
        match self {
            Self::LaidOutByItsOwnRun(laid_out) => laid_out.baselines,
            Self::LaidOutByThisRun(record) => record.content_baselines_from_cells(),
            Self::BeingSizedByItsParent(_) => DerivedBaselines::default(),
        }
    }

    pub(crate) fn orthogonal_flow_baselines(&self) -> Option<DerivedBaselines> {
        match self {
            Self::LaidOutByItsOwnRun(laid_out) => laid_out.orthogonal_flow_baselines,
            // NB: A box that establishes an orthogonal flow is laid out by a run of its own.
            Self::LaidOutByThisRun(_) | Self::BeingSizedByItsParent(_) => None,
        }
    }
}

macro_rules! box_metrics_getters {
    ($($name:ident: $value:ty,)+) => {
        impl BoxMetricCells for BoxMetrics<'_> {
            $(
                #[inline]
                fn $name(&self) -> $value {
                    match self {
                        Self::LaidOutByItsOwnRun(laid_out) => laid_out.cells.$name(),
                        Self::LaidOutByThisRun(record) => record.$name(),
                        Self::BeingSizedByItsParent(cells) => cells.$name(),
                    }
                }
            )+
        }
    };
}

box_metrics_getters! {
    content_inline_size: CssPixels,
    content_block_size: CssPixels,
    margin_left: CssPixels,
    margin_right: CssPixels,
    margin_top: CssPixels,
    margin_bottom: CssPixels,
    border_left: CssPixels,
    border_right: CssPixels,
    border_top: CssPixels,
    border_bottom: CssPixels,
    padding_left: CssPixels,
    padding_right: CssPixels,
    padding_top: CssPixels,
    padding_bottom: CssPixels,
    has_definite_inline_size: bool,
    has_definite_block_size: bool,
    has_definite_block_size_only_for_button_content_alignment: bool,
    uses_collapsing_borders_model: bool,
    is_collapsed_borders_table_box: bool,
}

/// Where a run keeps a box it holds.
#[derive(Clone, Copy)]
enum HeldBox {
    RootWithRecord,
    Kept(u32),
}

/// The used values of a box's inset properties.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct UsedInsets {
    pub(crate) left: CssPixels,
    pub(crate) right: CssPixels,
    pub(crate) top: CssPixels,
    pub(crate) bottom: CssPixels,
}

/// Whether a box depends on the block size its percentages resolve against, and whether a box it contains does.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct PercentageBlockSizeDependency {
    pub(crate) depends_on_percentage_block_size: bool,
    pub(crate) has_descendant_that_depends_on_percentage_block_size: bool,
}

type PercentageBlockSizeDependenciesInProgress =
    RefCell<smallvec::SmallVec<[(Node, PercentageBlockSizeDependency); 8]>>;

/// Keeps tracking the percentage block size dependency of a box while its layout is in progress.
pub(crate) struct PercentageBlockSizeDependencyTracking<'records> {
    dependencies_in_progress: &'records PercentageBlockSizeDependenciesInProgress,
    node: Node,
}

impl Drop for PercentageBlockSizeDependencyTracking<'_> {
    fn drop(&mut self) {
        let finished = self.dependencies_in_progress.borrow_mut().pop();
        if !std::thread::panicking() {
            assert_eq!(
                finished.map(|(node, _)| node),
                Some(self.node),
                "the layouts of boxes in progress finished out of order"
            );
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq)]
enum ContentSizing {
    Layout,
    RootContributions,
    RootContentInlineSizes,
}

enum RunKeptBoxClaim {
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
        root_cells: used_values::RootInputCells,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some(root_cells),
            ContentSizing::Layout,
            run,
        )
    }

    pub(crate) fn with_content_sizing_root<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_cells: used_values::RootInputCells,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some(root_cells),
            ContentSizing::RootContributions,
            run,
        )
    }

    pub(crate) fn with_content_inline_sizes_root<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_cells: used_values::RootInputCells,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some(root_cells),
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
        root_cells: Option<used_values::RootInputCells>,
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
            root_used: root_cells.map(|cells| cells.materialize_record()),
            scratch,
            nonce: scratch.begin_run(),
            stack_length_at_start: scratch.run_record_stack.length(),
            kept_box_stack_length_at_start: scratch.kept_boxes.borrow().len(),
            kept_boxes_outside_table: RefCell::new(HashMap::default()),
            root_facts_decided_by_parent: Cell::new(FactsDecidedByParent::default()),
            table_inline_layouts: RefCell::new(HashMap::default()),
            content_sizing,
            percentage_block_size_dependencies_in_progress: RefCell::new(smallvec::smallvec![(
                root,
                PercentageBlockSizeDependency::default()
            )]),
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
        let kept_box = self.scratch.keep_box(KeptBoxHolding::Record {
            record: index,
            handed_back_by_its_run: None,
        });
        match self.scratch.claim_run_kept_box(slot_index, self.nonce, kept_box) {
            RunKeptBoxClaim::Claimed => {}
            RunKeptBoxClaim::AlreadyClaimed => registered_twice(),
            RunKeptBoxClaim::HeldByEnclosingRun => {
                if self
                    .kept_boxes_outside_table
                    .borrow_mut()
                    .insert(node, kept_box)
                    .is_some()
                {
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
        match self.held_box(node)? {
            HeldBox::RootWithRecord => self.root_used.as_ref(),
            HeldBox::Kept(index) => match self.scratch.kept_boxes.borrow()[index as usize].holding {
                // SAFETY: This run registered the record, and it stays on the stack until this run returns.
                KeptBoxHolding::Record { record, .. } => {
                    Some(unsafe { self.scratch.run_record_stack.get(record).as_ref() })
                }
            },
        }
    }

    /// Whether this run holds `node`: lays it out itself, or keeps a child laid out by a run of its own.
    pub(crate) fn holds_box(&self, node: Node) -> bool {
        self.held_box(node).is_some()
    }

    #[track_caller]
    fn panic_for_box_not_held(&self, node: Node) -> ! {
        panic!(
            "the run rooted at slot {} does not hold slot {} (read at {})",
            self.root.slot_index(),
            node.slot_index(),
            std::panic::Location::caller(),
        )
    }

    fn held_box(&self, node: Node) -> Option<HeldBox> {
        if node == self.root && self.root_used.is_some() {
            return Some(HeldBox::RootWithRecord);
        }
        self.scratch
            .run_kept_box(node.slot_index(), self.nonce)
            .or_else(|| {
                let kept_boxes = self.kept_boxes_outside_table.borrow();
                if kept_boxes.is_empty() {
                    return None;
                }
                kept_boxes.get(&node).copied()
            })
            .map(HeldBox::Kept)
    }

    fn kept_facts_decided_by_parent_of(&self, node: Node) -> Option<FactsDecidedByParent> {
        Some(match self.held_box(node)? {
            HeldBox::RootWithRecord => self.root_facts_decided_by_parent.get(),
            HeldBox::Kept(index) => self.scratch.kept_boxes.borrow()[index as usize].facts_decided_by_parent,
        })
    }

    #[track_caller]
    fn keep_facts_decided_by_parent_of(&self, node: Node, facts: FactsDecidedByParent) {
        match self.held_box(node) {
            Some(HeldBox::RootWithRecord) => self.root_facts_decided_by_parent.set(facts),
            Some(HeldBox::Kept(index)) => {
                self.scratch.kept_boxes.borrow_mut()[index as usize].facts_decided_by_parent = facts;
            }
            None => self.panic_for_box_not_held(node),
        }
    }

    pub(crate) fn root(&self) -> Node {
        self.root
    }

    /// Places `node` in `placement`, which is final: the box's metrics are sealed with it.
    #[track_caller]
    pub(crate) fn place_box(&self, node: Node, placement: BoxPlacement) {
        let facts = self.facts_decided_by_parent_of(node);
        assert!(facts.placement.is_none(), "a box is placed once");
        self.keep_facts_decided_by_parent_of(
            node,
            FactsDecidedByParent {
                placement: Some(placement),
                ..facts
            },
        );
        if let Some(used) = self.used_values_if_owned(node) {
            used.seal_own_metrics();
        }
    }

    /// Adopts for `node` what its parent decided about it in the layout that committed `facts`, placing the box there.
    #[track_caller]
    pub(crate) fn adopt_committed_facts_decided_by_parent(&self, node: Node, facts: FactsDecidedByParent) {
        assert!(
            facts.placement.is_some() && self.facts_decided_by_parent_of(node).placement.is_none(),
            "a box adopts the placement it committed once"
        );
        self.keep_facts_decided_by_parent_of(node, facts);
        if let Some(used) = self.used_values_if_owned(node) {
            used.seal_own_metrics();
        }
    }

    #[track_caller]
    pub(crate) fn set_default_scroll_shift(&self, node: Node, shift: Option<abspos_inputs::DefaultScrollShift>) {
        let facts = self.facts_decided_by_parent_of(node);
        self.keep_facts_decided_by_parent_of(
            node,
            FactsDecidedByParent {
                default_scroll_shift: shift,
                ..facts
            },
        );
    }

    /// What the parent of `node` has decided about it so far.
    #[track_caller]
    pub(crate) fn facts_decided_by_parent_of(&self, node: Node) -> FactsDecidedByParent {
        match self.kept_facts_decided_by_parent_of(node) {
            Some(facts) => facts,
            None => self.panic_for_box_not_held(node),
        }
    }

    #[track_caller]
    fn update_fragment_facts_decided_by_parent_of(
        &self,
        node: Node,
        update: impl FnOnce(&mut FragmentFactsDecidedByParent),
    ) {
        assert!(
            !matches!(self.held_box(node), Some(HeldBox::RootWithRecord)),
            "what a box's fragment shows of its parent's layout is decided by the run of that parent"
        );
        let mut facts = self.facts_decided_by_parent_of(node);
        update(&mut facts.fragment_facts);
        self.keep_facts_decided_by_parent_of(node, facts);
    }

    /// Places `node` in the table grid columns starting at `first_column_index`.
    #[track_caller]
    pub(crate) fn place_in_table_columns(&self, node: Node, first_column_index: u32, column_span: u32) {
        self.update_fragment_facts_decided_by_parent_of(node, |facts| {
            facts.table_column_index = first_column_index;
            facts.table_column_span = column_span;
        });
    }

    #[track_caller]
    pub(crate) fn set_hidden_by_collapsed_columns(&self, node: Node, hidden: bool) {
        self.update_fragment_facts_decided_by_parent_of(node, |facts| facts.hidden_by_collapsed_columns = hidden);
    }

    #[track_caller]
    pub(crate) fn hide_for_line_clamp(&self, node: Node) {
        self.update_fragment_facts_decided_by_parent_of(node, |facts| facts.is_invisible_for_line_clamp = true);
    }

    #[track_caller]
    pub(crate) fn is_invisible_for_line_clamp(&self, node: Node) -> bool {
        self.facts_decided_by_parent_of(node)
            .fragment_facts
            .is_invisible_for_line_clamp
    }

    #[track_caller]
    pub(crate) fn note_continues_from_previous_fragment(&self, node: Node) {
        self.update_fragment_facts_decided_by_parent_of(node, |facts| facts.continues_from_previous_fragment = true);
    }

    #[track_caller]
    pub(crate) fn note_continues_in_next_fragment(&self, node: Node) {
        self.update_fragment_facts_decided_by_parent_of(node, |facts| facts.continues_in_next_fragment = true);
    }

    #[track_caller]
    pub(crate) fn continues_in_next_fragment(&self, node: Node) -> bool {
        self.facts_decided_by_parent_of(node)
            .fragment_facts
            .continues_in_next_fragment
    }

    /// Where `node` is placed, if this run placed it.
    pub(crate) fn placement_of(&self, node: Node) -> Option<BoxPlacement> {
        self.kept_facts_decided_by_parent_of(node)?.placement
    }

    pub(crate) fn is_placed(&self, node: Node) -> bool {
        self.placement_of(node).is_some()
    }

    /// The offset of the content box of `node` in its containing block, or the origin while the box is not placed.
    #[track_caller]
    pub(crate) fn content_offset_or_origin(&self, node: Node) -> FfiCssPixelPoint {
        self.facts_decided_by_parent_of(node)
            .placement
            .map_or(FfiCssPixelPoint::default(), |placement| {
                placement.content_offset_in_containing_block
            })
    }

    #[track_caller]
    pub(crate) fn used_insets_of(&self, node: Node) -> UsedInsets {
        self.facts_decided_by_parent_of(node).used_insets
    }

    /// Sets the used insets of `node`, which cannot change once the box is placed.
    #[track_caller]
    pub(crate) fn set_used_insets(&self, node: Node, insets: UsedInsets) {
        let facts = self.kept_facts_decided_by_parent_of(node).unwrap_or_default();
        assert!(facts.placement.is_none(), "the used insets of a placed box are final");
        self.keep_facts_decided_by_parent_of(
            node,
            FactsDecidedByParent {
                used_insets: insets,
                ..facts
            },
        );
    }

    /// Tracks the percentage block size dependency of `node` until the returned tracking is dropped, which the layout
    /// of `node` holds while it is in progress.
    pub(crate) fn track_percentage_block_size_dependency_while_laying_out(
        &self,
        node: Node,
    ) -> PercentageBlockSizeDependencyTracking<'_> {
        self.percentage_block_size_dependencies_in_progress
            .borrow_mut()
            .push((node, PercentageBlockSizeDependency::default()));
        PercentageBlockSizeDependencyTracking {
            dependencies_in_progress: &self.percentage_block_size_dependencies_in_progress,
            node,
        }
    }

    /// The percentage block size dependency found so far for the run's root or a box whose layout is in progress.
    #[track_caller]
    pub(crate) fn percentage_block_size_dependency_of(&self, node: Node) -> PercentageBlockSizeDependency {
        self.percentage_block_size_dependencies_in_progress
            .borrow()
            .iter()
            .rev()
            .find(|(tracked, _)| *tracked == node)
            .map(|(_, dependency)| *dependency)
            .expect("only the run root and boxes whose layout is in progress have their dependency resolved")
    }

    /// Notes that `node` depends on the block size its percentages resolve against. Nothing resolves the dependency of
    /// a box that is neither the run root nor in progress, so the note is dropped for such a box.
    pub(crate) fn note_box_depends_on_percentage_block_size(&self, node: Node) {
        self.update_percentage_block_size_dependency_of(node, |dependency| {
            dependency.depends_on_percentage_block_size = true;
        });
    }

    /// Notes that a box `containing_block` contains depends on the block size its percentages resolve against.
    /// Nothing resolves the dependency of a box that is neither the run root nor in progress, so the note is dropped for
    /// such a box.
    pub(crate) fn note_descendant_depends_on_percentage_block_size(&self, containing_block: Node) {
        self.update_percentage_block_size_dependency_of(containing_block, |dependency| {
            dependency.has_descendant_that_depends_on_percentage_block_size = true;
        });
    }

    fn update_percentage_block_size_dependency_of(
        &self,
        node: Node,
        update: impl FnOnce(&mut PercentageBlockSizeDependency),
    ) {
        if let Some((_, dependency)) = self
            .percentage_block_size_dependencies_in_progress
            .borrow_mut()
            .iter_mut()
            .rev()
            .find(|(tracked, _)| *tracked == node)
        {
            update(dependency);
        }
    }

    /// Keeps what the run of `child`, a box this run holds the record of, handed back once it returned.
    #[track_caller]
    pub(crate) fn receive_laid_out_child_box(&self, child: Node, handed_back: ChildLaidOutByItsOwnRun) {
        let Some(HeldBox::Kept(index)) = self.held_box(child) else {
            self.panic_for_box_not_held(child)
        };
        let KeptBoxHolding::Record {
            handed_back_by_its_run, ..
        } = &mut self.scratch.kept_boxes.borrow_mut()[index as usize].holding;
        *handed_back_by_its_run = Some(handed_back);
    }

    /// The sizes, size constraints, box sides and baselines of `node`, a box this run lays out or a child of one laid
    /// out by a run of its own.
    #[track_caller]
    pub(crate) fn box_metrics_of(&self, node: Node) -> BoxMetrics<'_> {
        match self.box_metrics_of_if_owned(node) {
            Some(metrics) => metrics,
            None => self.panic_for_box_not_held(node),
        }
    }

    /// The sizes, size constraints, box sides and baselines of `node`, if this run lays it out or holds a child it laid
    /// out by a run of its own.
    pub(crate) fn box_metrics_of_if_owned(&self, node: Node) -> Option<BoxMetrics<'_>> {
        let index = match self.held_box(node)? {
            HeldBox::RootWithRecord => return self.root_used.as_ref().map(BoxMetrics::LaidOutByThisRun),
            HeldBox::Kept(index) => index,
        };
        let (record, laid_out) = match &self.scratch.kept_boxes.borrow()[index as usize].holding {
            KeptBoxHolding::Record {
                record,
                handed_back_by_its_run,
            } => (
                *record,
                handed_back_by_its_run.as_ref().map(|handed_back| handed_back.laid_out),
            ),
        };
        // SAFETY: This run registered the record, and it stays on the stack until this run returns.
        let record = unsafe { self.scratch.run_record_stack.get(record).as_ref() };
        let Some(laid_out) = laid_out else {
            return Some(BoxMetrics::LaidOutByThisRun(record));
        };
        if checks_kept_boxes_against_records() {
            assert_eq!(
                laid_out.cells,
                used_values::RootInputCells::capture(record),
                "the parent's record of {node:?} changed after its run handed the box back"
            );
        }
        Some(BoxMetrics::LaidOutByItsOwnRun(laid_out))
    }

    /// Moves the sizes, size constraints and box sides this run holds for `node`, if it holds the box, from frame `from`
    /// to frame `to`.
    pub(crate) fn move_box_metrics_to_frame(&self, node: Node, from: geometry::Frame, to: geometry::Frame) {
        let index = match self.held_box(node) {
            None => return,
            Some(HeldBox::RootWithRecord) => {
                if let Some(root_used) = &self.root_used {
                    root_used.move_box_metrics_to_frame(from, to);
                }
                return;
            }
            Some(HeldBox::Kept(index)) => index,
        };
        let mut kept_boxes = self.scratch.kept_boxes.borrow_mut();
        let KeptBoxHolding::Record {
            record,
            handed_back_by_its_run,
        } = &mut kept_boxes[index as usize].holding;
        // SAFETY: This run registered the record, and it stays on the stack until this run returns.
        unsafe { self.scratch.run_record_stack.get(*record).as_ref() }.move_box_metrics_to_frame(from, to);
        if let Some(ChildLaidOutByItsOwnRun { laid_out, frame, .. }) = handed_back_by_its_run {
            debug_assert_eq!(*frame, from, "a kept box moves out of the frame it is in");
            // NB: The baselines stay those of the parent's frame, which every reader of them is in.
            laid_out.cells = used_values::UsedValuesCellState {
                inputs: laid_out.cells,
                baselines: laid_out.baselines,
            }
            .in_frame(from, to)
            .inputs;
            *frame = to;
        }
    }

    /// What the fragment of `node` shows of the box's own layout, which finishes the lines this run laid out in it.
    #[track_caller]
    pub(crate) fn fragment_geometry_of(
        &self,
        node: Node,
        callbacks: &LayoutPass<'_>,
    ) -> fragment_tree::FragmentGeometry {
        let used = self.used_values(node);
        let line_data = used.finish_line_data(callbacks);
        let geometry_of_record = fragment_tree::FragmentGeometry {
            cells: used_values::RootInputCells::capture(used),
            line_data,
        };
        let Some(HeldBox::Kept(index)) = self.held_box(node) else {
            return geometry_of_record;
        };
        let handed_back = match &self.scratch.kept_boxes.borrow()[index as usize].holding {
            KeptBoxHolding::Record {
                handed_back_by_its_run, ..
            } => handed_back_by_its_run.clone(),
        };
        let Some(handed_back) = handed_back else {
            return geometry_of_record;
        };
        if checks_kept_boxes_against_records() {
            assert_eq!(
                handed_back.frame, callbacks.frame,
                "the fragment of {node:?} is built in another frame than the one its box is kept in"
            );
            assert!(
                handed_back.laid_out.cells == geometry_of_record.cells
                    && match (&handed_back.line_data, &geometry_of_record.line_data) {
                        (None, None) => true,
                        (Some(kept), Some(recorded)) => std::sync::Arc::ptr_eq(kept, recorded),
                        _ => false,
                    },
                "the parent's record of {node:?} changed after its run handed the box back"
            );
        }
        fragment_tree::FragmentGeometry {
            cells: handed_back.laid_out.cells,
            line_data: handed_back.line_data,
        }
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
        self.scratch
            .kept_boxes
            .borrow_mut()
            .truncate(self.kept_box_stack_length_at_start);
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

    fn default_root_cells() -> used_values::RootInputCells {
        used_values::RootInputCells::capture(&UsedValues::default())
    }

    #[test]
    fn nested_runs_leave_their_parents_records_in_place() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let parent_nonce = RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            default_root_cells(),
            |parent, _| {
                let root_used: *const UsedValues = parent.used_values(root);
                let child_used: *const UsedValues = parent.register(child, UsedValues::default());
                RunRecords::with_root(
                    LayoutPass::for_test(&arena, &scratch),
                    child,
                    root,
                    default_root_cells(),
                    |nested, _| {
                        let nested_used: *const UsedValues = nested.used_values(child);
                        assert!(!std::ptr::eq(nested_used, child_used));
                        assert!(std::ptr::eq(parent.used_values(child), child_used));
                        assert!(std::ptr::eq(parent.used_values(root), root_used));
                        assert!(nested.used_values_if_owned(root).is_none());
                        RunRecords::with_unrooted(
                            LayoutPass::for_test(&arena, &scratch),
                            child,
                            root,
                            |measurement, _| {
                                let measured: *const UsedValues = measurement.register(child, UsedValues::default());
                                assert!(std::ptr::eq(measurement.used_values(child), measured));
                                assert!(std::ptr::eq(parent.used_values(child), child_used));
                                assert!(std::ptr::eq(nested.used_values(child), nested_used));
                            },
                        );
                        assert!(std::ptr::eq(parent.used_values(child), child_used));
                    },
                );
                assert!(std::ptr::eq(parent.used_values(child), child_used));
                parent.nonce
            },
        );
        assert!(scratch.run_kept_box(root.slot_index(), parent_nonce).is_none());
        assert_eq!(scratch.run_record_stack.length(), 0);
        assert!(scratch.kept_boxes.borrow().is_empty());
    }

    #[test]
    fn returning_runs_release_their_records_and_reuse_the_storage() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let lines = std::sync::Arc::new(crate::layout::inline_content::InlineContent::default());
        let weak_lines = std::sync::Arc::downgrade(&lines);
        let first_record = RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            default_root_cells(),
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
            default_root_cells(),
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
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            default_root_cells(),
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
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            default_root_cells(),
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
        RunRecords::with_unrooted(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            |parent, _| {
                RunRecords::with_root(
                    LayoutPass::for_test(&arena, &scratch),
                    root,
                    NodeSlotId::INVALID,
                    default_root_cells(),
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
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            default_root_cells(),
            |parent, _| {
                let root_used: *const UsedValues = parent.used_values(root);
                let child_used: *const UsedValues = parent.register(child, UsedValues::default());
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    RunRecords::with_root(
                        LayoutPass::for_test(&arena, &scratch),
                        child,
                        root,
                        default_root_cells(),
                        |nested, _| {
                            nested.register(root, UsedValues::default());
                            panic!("abort the nested measurement");
                        },
                    );
                }));
                assert!(result.is_err());
                assert!(std::ptr::eq(parent.used_values(root), root_used));
                assert!(std::ptr::eq(parent.used_values(child), child_used));
            },
        );
    }
}
