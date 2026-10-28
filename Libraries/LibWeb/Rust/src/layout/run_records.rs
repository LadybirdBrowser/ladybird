/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;
use std::ptr::NonNull;

/// What the layout stage keeps for itself besides what each of its runs keeps: what the runs of a layout pass share and
/// what the runs in progress found out for the host. It is the stage's scratch rather than arena columns: nothing
/// outside a running pass reads it, but for the memos it keeps for later passes, and it holds no record of a box.
pub(crate) struct LayoutScratch {
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

// The scratch holds no record, so it can move to the thread a layout stage runs on.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<LayoutScratch>();
};

/// What a run keeps for a box it holds: what the box's parent decided about it, and how the run
/// holds the box.
struct KeptBox {
    facts_decided_by_parent: FactsDecidedByParent,
    holding: KeptBoxHolding,
}

enum KeptBoxHolding {
    /// A box this run lays out itself, in a record the run keeps for it until the run is dropped.
    LaidOutByThisRun { record: NonNull<UsedValues> },
    /// A box this run lays out itself, in a record that the layout of the box owns: the call laying it out, or the
    /// formatting context it lays the box out for.
    LaidOutInRecordItsLayoutOwns,
    /// A child this run keeps for a layout of its own, a run of its own or a record the call laying it out owns,
    /// with the cells it starts out with, until that layout returns.
    ChildNotYetLaidOut {
        cells_before_its_run: used_values::RootInputCells,
    },
    /// A child whose own layout handed the box back to the formatting context of this run, which keeps what it reads
    /// of the box.
    ChildHandedBack,
}

impl Default for LayoutScratch {
    fn default() -> Self {
        Self {
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
    }

    /// Starts a run inside the runs in progress, and returns how many runs are in progress with it.
    fn begin_run(&self) -> usize {
        let mut findings_of_runs_in_progress = self.host_findings_of_runs_in_progress.borrow_mut();
        findings_of_runs_in_progress.push(HostFindings::default());
        findings_of_runs_in_progress.len()
    }

    fn end_run(&self, runs_in_progress_with_it: usize) {
        let mut findings_of_runs_in_progress = self.host_findings_of_runs_in_progress.borrow_mut();
        assert_eq!(
            findings_of_runs_in_progress.len(),
            runs_in_progress_with_it,
            "layout runs ended out of order"
        );
        findings_of_runs_in_progress.pop();
    }

    fn runs_in_progress(&self) -> usize {
        self.host_findings_of_runs_in_progress.borrow().len()
    }
}

/// The per-run registry of the boxes a run holds. A run owns its root's record, built from the input cells its caller
/// hands it, and keeps what it holds of every other box: what the box's parent decides about it, with the record of a
/// box it lays out in a record it keeps, or with the cells a child it keeps for a layout of its own starts out with,
/// until that layout hands the box back to the run's formatting context. A box whose layout owns its record keeps it
/// there. The run releases what it keeps when it returns. The scoped constructors lend records to the run, so callers cannot extend
/// the registry's lifetime.
pub(crate) struct RunRecords<'arena> {
    root: Node,
    root_used: Option<UsedValues>,
    scratch: &'arena LayoutScratch,
    /// How many runs are in progress with this one, which only the innermost of them may register boxes with.
    runs_in_progress_with_it: usize,
    /// What the run keeps of each box it holds but its root.
    held_boxes: RefCell<HashMap<Node, KeptBox>>,
    /// What the parent of the root of a run that owns its root record decides about it, which the run keeps with every
    /// other box it holds.
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

/// What the parent of a box decides about the box, kept with the box by the run that holds it.
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

impl LaidOutChildBox {
    /// A child as the run of its parent left it in `record`, where that run sizes the child itself instead of handing
    /// it to a run of its own.
    pub(crate) fn sized_in_parent_run(record: &UsedValues) -> Self {
        Self {
            cells: used_values::RecordInFrame::in_its_own_frame(record).cells(),
            baselines: record.content_baselines_from_cells(),
            // NB: A box that establishes an orthogonal flow is laid out by a run of its own.
            orthogonal_flow_baselines: None,
        }
    }
}

/// What the parent of a box laid out by a run of its own keeps of it once that run returns: the box, the lines the run
/// laid out in it, and the frame the box's metrics are in.
#[derive(Clone)]
pub(crate) struct ChildLaidOutByItsOwnRun {
    pub(crate) laid_out: LaidOutChildBox,
    pub(crate) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    pub(crate) frame: geometry::Frame,
}

impl ChildLaidOutByItsOwnRun {
    /// What placing the child takes of the box its run handed back.
    pub(crate) fn placed_child(&self) -> formatting_context::PlacedChild {
        formatting_context::PlacedChild::of_laid_out_box(&self.laid_out, self.line_data.clone(), self.frame)
    }
}

/// The sizes, size constraints, box sides and baselines of a box as a run sees them: kept as a value for a child
/// awaiting the run of its own, or read from the record of a box the run lays out itself.
#[derive(Clone, Copy)]
pub(crate) enum BoxMetrics<'records> {
    LaidOutByThisRun(used_values::RecordInFrame<'records>),
    /// A child this run keeps for a run of its own that has not laid it out yet, with the cells it is to be handed to
    /// that run with.
    NotYetLaidOut(used_values::RootInputCells),
}

impl BoxMetrics<'_> {
    pub(crate) fn input_cells(&self) -> used_values::RootInputCells {
        match self {
            Self::LaidOutByThisRun(record) => record.cells(),
            Self::NotYetLaidOut(cells) => *cells,
        }
    }

    pub(crate) fn content_baselines(&self) -> DerivedBaselines {
        match self {
            Self::LaidOutByThisRun(record) => record.record().content_baselines_from_cells(),
            Self::NotYetLaidOut(_) => DerivedBaselines::default(),
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
                        Self::LaidOutByThisRun(record) => record.$name(),
                        Self::NotYetLaidOut(cells) => cells.$name(),
                    }
                }
            )+

            fn record_viewed(&self) -> Option<&UsedValues> {
                match self {
                    Self::LaidOutByThisRun(record) => Some(record.record()),
                    _ => None,
                }
            }
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

impl<'arena> RunRecords<'arena> {
    /// Runs `run` with the records of a run rooted at `root`, and the pass as that run sees it. The root's record holds
    /// `root_cells`, which hold its metrics in `root_frame`, the frame it lays its contents out in.
    pub(crate) fn with_root<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_cells: used_values::RootInputCells,
        root_frame: geometry::Frame,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some((root_cells, root_frame)),
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
            Some((root_cells, callbacks.frame)),
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
            Some((root_cells, callbacks.frame)),
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
        root_cells: Option<(used_values::RootInputCells, geometry::Frame)>,
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
            root_used: root_cells.map(|(cells, frame)| cells.materialize_record(frame)),
            scratch,
            runs_in_progress_with_it: scratch.begin_run(),
            held_boxes: RefCell::new(HashMap::default()),
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
        assert!(
            self.is_innermost_run(),
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
        self.assert_registers_in_innermost_run(node);
        let record = NonNull::from(Box::leak(Box::new(used)));
        self.keep(node, KeptBoxHolding::LaidOutByThisRun { record });
        // SAFETY: The run keeps the record until it is dropped, which ends this borrow of the run.
        unsafe { record.as_ref() }
    }

    /// Keeps `node`, a box this run lays out in a record that the layout of the box owns, for what the box's parent
    /// decides about it.
    pub(crate) fn keep_box_laid_out_in_record_its_layout_owns(&self, node: Node) {
        self.assert_registers_in_innermost_run(node);
        self.keep(node, KeptBoxHolding::LaidOutInRecordItsLayoutOwns);
    }

    /// Keeps `child`, which this run hands to a run of its own, with the cells it starts out with, which it returns.
    pub(crate) fn keep_child_for_its_run(
        &self,
        callbacks: &LayoutPass<'_>,
        child: Node,
        constraints: ContainingBlockConstraints,
    ) -> used_values::RootInputCells {
        let cells = used_values::RootInputCells::of_box_before_sizing(callbacks, child, constraints);
        self.keep_child_for_its_run_with_cells(child, cells);
        cells
    }

    /// Keeps `child`, which this run hands to a run of its own, with the cells `cells` it starts out with.
    pub(crate) fn keep_child_for_its_run_with_cells(&self, child: Node, cells: used_values::RootInputCells) {
        self.assert_registers_in_innermost_run(child);
        self.keep(
            child,
            KeptBoxHolding::ChildNotYetLaidOut {
                cells_before_its_run: cells,
            },
        );
    }

    /// Whether this run keeps `child` without a record, for runs of its own: before the first of them returns, or once
    /// the last one handed it back.
    pub(crate) fn keeps_child_without_record(&self, child: Node) -> bool {
        if self.is_root_with_record(child) {
            return false;
        }
        self.held_boxes.borrow().get(&child).is_some_and(|kept| {
            !matches!(
                kept.holding,
                KeptBoxHolding::LaidOutByThisRun { .. } | KeptBoxHolding::LaidOutInRecordItsLayoutOwns
            )
        })
    }

    /// Gives `child`, a box this run keeps without a record, a record with the cells `cells`, for this run to lay the
    /// box out itself.
    #[track_caller]
    pub(crate) fn lay_out_child_in_this_run(
        &self,
        child: Node,
        cells: used_values::RootInputCells,
        frame: geometry::Frame,
    ) -> &UsedValues {
        assert!(
            self.is_innermost_run(),
            "only the innermost layout run may register records"
        );
        if self.is_root_with_record(child) {
            self.panic_for_box_not_held(child)
        }
        let mut held_boxes = self.held_boxes.borrow_mut();
        let Some(kept) = held_boxes.get_mut(&child) else {
            self.panic_for_box_not_held(child)
        };
        assert!(
            !matches!(
                kept.holding,
                KeptBoxHolding::LaidOutByThisRun { .. } | KeptBoxHolding::LaidOutInRecordItsLayoutOwns
            ),
            "slot {} already has a record in the run rooted at slot {}",
            child.slot_index(),
            self.root.slot_index()
        );
        let record = NonNull::from(Box::leak(Box::new(cells.materialize_record(frame))));
        kept.holding = KeptBoxHolding::LaidOutByThisRun { record };
        // SAFETY: The run keeps the record until it is dropped, which ends this borrow of the run.
        unsafe { record.as_ref() }
    }

    fn assert_registers_in_innermost_run(&self, node: Node) {
        if self.is_root_with_record(node) {
            self.panic_for_box_registered_twice(node);
        }
        assert!(
            self.is_innermost_run(),
            "only the innermost layout run may register records"
        );
    }

    fn is_innermost_run(&self) -> bool {
        self.scratch.runs_in_progress() == self.runs_in_progress_with_it
    }

    fn is_root_with_record(&self, node: Node) -> bool {
        node == self.root && self.root_used.is_some()
    }

    fn panic_for_box_registered_twice(&self, node: Node) -> ! {
        panic!(
            "slot {} registered twice in the run rooted at slot {}",
            node.slot_index(),
            self.root.slot_index()
        )
    }

    fn keep(&self, node: Node, holding: KeptBoxHolding) {
        let mut held_boxes = self.held_boxes.borrow_mut();
        if held_boxes.contains_key(&node) {
            self.panic_for_box_registered_twice(node);
        }
        held_boxes.insert(
            node,
            KeptBox {
                facts_decided_by_parent: FactsDecidedByParent::default(),
                holding,
            },
        );
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
        if self.is_root_with_record(node) {
            return self.root_used.as_ref();
        }
        let record = self.record_this_run_keeps(node)?;
        // SAFETY: The run keeps the record until it is dropped, which ends this borrow of the run.
        Some(unsafe { record.as_ref() })
    }

    /// The record this run keeps for `node`, a box it holds but its root, where it keeps one.
    fn record_this_run_keeps(&self, node: Node) -> Option<NonNull<UsedValues>> {
        match self.held_boxes.borrow().get(&node)?.holding {
            KeptBoxHolding::LaidOutByThisRun { record } => Some(record),
            KeptBoxHolding::LaidOutInRecordItsLayoutOwns
            | KeptBoxHolding::ChildNotYetLaidOut { .. }
            | KeptBoxHolding::ChildHandedBack => None,
        }
    }

    /// Whether this run holds `node`: lays it out itself, or keeps a child laid out by a run of its own.
    pub(crate) fn holds_box(&self, node: Node) -> bool {
        self.is_root_with_record(node) || self.held_boxes.borrow().contains_key(&node)
    }

    #[track_caller]
    fn panic_for_child_read_after_its_run_handed_it_back(&self, node: Node) -> ! {
        panic!(
            "the run rooted at slot {} reads slot {}, which its parent keeps once its layout handed it back (read at {})",
            self.root.slot_index(),
            node.slot_index(),
            std::panic::Location::caller(),
        )
    }

    #[track_caller]
    fn panic_for_box_read_outside_the_layout_owning_its_record(&self, node: Node) -> ! {
        panic!(
            "the run rooted at slot {} reads slot {}, whose layout keeps it in a record of its own (read at {})",
            self.root.slot_index(),
            node.slot_index(),
            std::panic::Location::caller(),
        )
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

    fn kept_facts_decided_by_parent_of(&self, node: Node) -> Option<FactsDecidedByParent> {
        if self.is_root_with_record(node) {
            return Some(self.root_facts_decided_by_parent.get());
        }
        Some(self.held_boxes.borrow().get(&node)?.facts_decided_by_parent)
    }

    #[track_caller]
    fn keep_facts_decided_by_parent_of(&self, node: Node, facts: FactsDecidedByParent) {
        if self.is_root_with_record(node) {
            return self.root_facts_decided_by_parent.set(facts);
        }
        match self.held_boxes.borrow_mut().get_mut(&node) {
            Some(kept) => kept.facts_decided_by_parent = facts,
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
            !self.is_root_with_record(node),
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

    /// Keeps for `node`, an in-flow box, what its insets resolved to.
    #[track_caller]
    pub(crate) fn keep_in_flow_box_insets(&self, node: Node, insets: abspos_engine::InFlowBoxInsets) {
        if let Some(default_scroll_shift) = insets.default_scroll_shift {
            self.set_default_scroll_shift(node, Some(default_scroll_shift));
        }
        if let Some(used_insets) = insets.used_insets {
            self.set_used_insets(node, used_insets);
        }
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

    /// Notes that the layout of `child`, a box this run keeps for a layout of its own, handed the box back once it
    /// returned.
    #[track_caller]
    pub(crate) fn note_child_handed_back(&self, child: Node) {
        if self.is_root_with_record(child) {
            self.panic_for_box_not_held(child)
        }
        let mut held_boxes = self.held_boxes.borrow_mut();
        let Some(kept) = held_boxes.get_mut(&child) else {
            self.panic_for_box_not_held(child)
        };
        assert!(
            !matches!(
                kept.holding,
                KeptBoxHolding::LaidOutByThisRun { .. } | KeptBoxHolding::LaidOutInRecordItsLayoutOwns
            ),
            "{child:?} is handed back by a run of its own to a parent that lays it out itself"
        );
        kept.holding = KeptBoxHolding::ChildHandedBack;
    }

    /// The sizes, size constraints, box sides and baselines of `node`, a box this run lays out or a child it keeps for
    /// a run of its own that has not laid it out yet.
    #[track_caller]
    pub(crate) fn box_metrics_of(&self, node: Node) -> BoxMetrics<'_> {
        match self.box_metrics_of_if_owned(node) {
            Some(metrics) => metrics,
            None => self.panic_for_box_not_held(node),
        }
    }

    /// The sizes, size constraints, box sides and baselines of `node`, if this run lays it out or keeps a child for a
    /// run of its own that has not laid it out yet. A box its layout handed back, or laid out in a record it owns, is
    /// read from that layout's result instead, so reading it here panics.
    #[track_caller]
    pub(crate) fn box_metrics_of_if_owned(&self, node: Node) -> Option<BoxMetrics<'_>> {
        if self.is_root_with_record(node) {
            return self
                .root_used
                .as_ref()
                .map(|record| BoxMetrics::LaidOutByThisRun(used_values::RecordInFrame::in_its_own_frame(record)));
        }
        let record = match &self.held_boxes.borrow().get(&node)?.holding {
            KeptBoxHolding::LaidOutByThisRun { record } => *record,
            KeptBoxHolding::ChildNotYetLaidOut { cells_before_its_run } => {
                return Some(BoxMetrics::NotYetLaidOut(*cells_before_its_run));
            }
            KeptBoxHolding::ChildHandedBack => self.panic_for_child_read_after_its_run_handed_it_back(node),
            KeptBoxHolding::LaidOutInRecordItsLayoutOwns => {
                self.panic_for_box_read_outside_the_layout_owning_its_record(node)
            }
        };
        // SAFETY: The run keeps the record until it is dropped, which ends this borrow of the run.
        Some(BoxMetrics::LaidOutByThisRun(
            used_values::RecordInFrame::in_its_own_frame(unsafe { record.as_ref() }),
        ))
    }

    /// The sizes, size constraints, box sides and baselines of `node` as box_metrics_of() has them, with the record of a
    /// box this run lays out read in frame `reader`.
    #[track_caller]
    pub(crate) fn box_metrics_in_frame(&self, node: Node, reader: geometry::Frame) -> BoxMetrics<'_> {
        match self.box_metrics_in_frame_if_owned(node, reader) {
            Some(metrics) => metrics,
            None => self.panic_for_box_not_held(node),
        }
    }

    /// The sizes, size constraints, box sides and baselines of `node` as box_metrics_of_if_owned() has them, with the
    /// record of a box this run lays out read in frame `reader`.
    #[track_caller]
    pub(crate) fn box_metrics_in_frame_if_owned(&self, node: Node, reader: geometry::Frame) -> Option<BoxMetrics<'_>> {
        Some(match self.box_metrics_of_if_owned(node)? {
            BoxMetrics::LaidOutByThisRun(record) => {
                BoxMetrics::LaidOutByThisRun(used_values::RecordInFrame::new(record.record(), reader))
            }
            metrics_kept_as_a_value => metrics_kept_as_a_value,
        })
    }

    /// What the fragment of `node` shows of the box's own layout, which finishes the lines this run laid out in it.
    #[track_caller]
    pub(crate) fn fragment_geometry_of(
        &self,
        node: Node,
        callbacks: &LayoutPass<'_>,
    ) -> fragment_tree::FragmentGeometry {
        let geometry_of_record = |used: &UsedValues| {
            let line_data = used.finish_line_data(callbacks);
            fragment_tree::FragmentGeometry {
                cells: used_values::RecordInFrame::new(used, callbacks.frame).cells(),
                line_data,
            }
        };
        if self.is_root_with_record(node) {
            return geometry_of_record(self.used_values(node));
        }
        let holding = match self.held_boxes.borrow().get(&node) {
            Some(kept) => match &kept.holding {
                KeptBoxHolding::LaidOutByThisRun { record } => Ok(*record),
                KeptBoxHolding::ChildNotYetLaidOut { .. } | KeptBoxHolding::ChildHandedBack => Err(true),
                KeptBoxHolding::LaidOutInRecordItsLayoutOwns => Err(false),
            },
            None => self.panic_for_box_not_held(node),
        };
        let record = match holding {
            Ok(record) => record,
            Err(true) => panic!("the fragment of {node:?} is built from the box its run hands back to its parent"),
            Err(false) => self.panic_for_box_read_outside_the_layout_owning_its_record(node),
        };
        // SAFETY: The run keeps the record until it is dropped, which ends this borrow of the run.
        geometry_of_record(unsafe { record.as_ref() })
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
        for kept in self.held_boxes.get_mut().values() {
            if let KeptBoxHolding::LaidOutByThisRun { record } = kept.holding {
                // SAFETY: The run leaked the record into its keeping, and nothing borrows it once the run is dropped.
                drop(unsafe { Box::from_raw(record.as_ptr()) });
            }
        }
        self.scratch.end_run(self.runs_in_progress_with_it);
    }
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
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            default_root_cells(),
            geometry::Frame::PHYSICAL,
            |parent, _| {
                let root_used: *const UsedValues = parent.used_values(root);
                let child_used: *const UsedValues = parent.register(child, UsedValues::default());
                RunRecords::with_root(
                    LayoutPass::for_test(&arena, &scratch),
                    child,
                    root,
                    default_root_cells(),
                    geometry::Frame::PHYSICAL,
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
            },
        );
        assert_eq!(scratch.runs_in_progress(), 0);
    }

    #[test]
    fn returning_runs_release_their_records() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        let lines = std::sync::Arc::new(crate::layout::inline_content::InlineContent::default());
        let weak_lines = std::sync::Arc::downgrade(&lines);
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            default_root_cells(),
            geometry::Frame::PHYSICAL,
            |records, _| {
                let used = records.register(child, UsedValues::default());
                used.set_finished_line_data(lines);
            },
        );
        assert!(weak_lines.upgrade().is_none());
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
            geometry::Frame::PHYSICAL,
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
            geometry::Frame::PHYSICAL,
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
                    geometry::Frame::PHYSICAL,
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
            geometry::Frame::PHYSICAL,
            |parent, _| {
                let root_used: *const UsedValues = parent.used_values(root);
                let child_used: *const UsedValues = parent.register(child, UsedValues::default());
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    RunRecords::with_root(
                        LayoutPass::for_test(&arena, &scratch),
                        child,
                        root,
                        default_root_cells(),
                        geometry::Frame::PHYSICAL,
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
