/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

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

/// What a run keeps while it is in progress: the record of its root, built from the input cells its caller hands it, the
/// anchor-size() resolutions of its root, and the percentage block size dependencies it resolves. Every other box the
/// run lays out is laid out in a record the call laying it out owns, or handed to a run of its own. The scoped
/// constructors lend the records to the run, so callers cannot extend their lifetime.
pub(crate) struct RunRecords<'arena> {
    root: Node,
    root_used: Option<UsedValues>,
    scratch: &'arena LayoutScratch,
    /// How many runs are in progress with this one, of which only the innermost hands over what it found out.
    runs_in_progress_with_it: usize,
    content_sizing: ContentSizing,
    /// What the run found so far about the percentage block size dependency of its root and of the boxes whose
    /// layout in it is in progress, the innermost last. Only these boxes have their dependency resolved.
    percentage_block_size_dependencies_in_progress: PercentageBlockSizeDependenciesInProgress,
    /// The anchor-size() resolutions of an absolutely positioned root, a run input that is constant for the run.
    root_anchor_resolved_values: Option<Box<style_values::AnchorResolvedValues>>,
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
/// constraints and box sides in the parent's frame, its baselines, and the lines the run laid out in it.
#[derive(Clone)]
pub(crate) struct LaidOutChildBox {
    pub(crate) cells: used_values::RootInputCells,
    pub(crate) baselines: DerivedBaselines,
    /// For a box that establishes an orthogonal flow, its baselines along the horizontal axis of the parent's frame.
    pub(crate) orthogonal_flow_baselines: Option<DerivedBaselines>,
    pub(crate) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
}

impl LaidOutChildBox {
    /// A child as the run of its parent left it in `record`, with the `baselines` of its contents, where that run
    /// sizes the child itself instead of handing it to a run of its own.
    pub(crate) fn sized_in_parent_run(record: &UsedValues, baselines: DerivedBaselines) -> Self {
        Self {
            cells: used_values::RootInputCells::capture(record),
            baselines,
            // NB: A box that establishes an orthogonal flow is laid out by a run of its own.
            orthogonal_flow_baselines: None,
            line_data: None,
        }
    }
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
    /// `root_cells`, which hold its metrics in `root_frame`, the frame it lays its contents out in. The style reads of
    /// an absolutely positioned root see `root_anchor_resolved_values`.
    pub(crate) fn with_root<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_cells: used_values::RootInputCells,
        root_frame: geometry::Frame,
        root_anchor_resolved_values: Option<Box<style_values::AnchorResolvedValues>>,
        run: impl FnOnce(&Self, LayoutPass<'arena>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            Some((root_cells, root_frame)),
            root_anchor_resolved_values,
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
            None,
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
            None,
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
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            None,
            None,
            ContentSizing::Layout,
            run,
        )
    }

    fn enter<R>(
        callbacks: LayoutPass<'arena>,
        root: Node,
        root_containing_block: Node,
        root_cells: Option<(used_values::RootInputCells, geometry::Frame)>,
        root_anchor_resolved_values: Option<Box<style_values::AnchorResolvedValues>>,
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
            content_sizing,
            percentage_block_size_dependencies_in_progress: RefCell::new(smallvec::smallvec![(
                root,
                PercentageBlockSizeDependency::default()
            )]),
            root_anchor_resolved_values,
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

    fn is_innermost_run(&self) -> bool {
        self.scratch.runs_in_progress() == self.runs_in_progress_with_it
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

    /// The record of `node` where it is the run's root, the only box a run keeps a record of.
    pub(crate) fn used_values_if_owned(&self, node: Node) -> Option<&UsedValues> {
        if node != self.root {
            return None;
        }
        self.root_used.as_ref()
    }

    pub(crate) fn root(&self) -> Node {
        self.root
    }

    /// The anchor-size() resolutions the style reads of `node` see, which only an absolutely positioned run root has.
    pub(crate) fn anchor_resolved_values_of(&self, node: Node) -> Option<&style_values::AnchorResolvedValues> {
        if node != self.root {
            return None;
        }
        self.root_anchor_resolved_values.as_deref()
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

    /// Whether this run resolves the percentage block size dependency of `node`: the run root, or a box whose layout is
    /// in progress.
    pub(crate) fn resolves_percentage_block_size_dependency_of(&self, node: Node) -> bool {
        self.percentage_block_size_dependencies_in_progress
            .borrow()
            .iter()
            .any(|(tracked, _)| *tracked == node)
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
}

impl Drop for RunRecords<'_> {
    fn drop(&mut self) {
        self.scratch.end_run(self.runs_in_progress_with_it);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_nested_run_keeps_its_own_root_record() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        RunRecords::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            used_values::RootInputCells::default(),
            geometry::Frame::PHYSICAL,
            None,
            |parent, _| {
                let root_used: *const UsedValues = parent.used_values(root);
                RunRecords::with_root(
                    LayoutPass::for_test(&arena, &scratch),
                    child,
                    root,
                    used_values::RootInputCells::default(),
                    geometry::Frame::PHYSICAL,
                    None,
                    |nested, _| {
                        assert!(nested.used_values_if_owned(root).is_none());
                        assert!(nested.used_values_if_owned(child).is_some());
                        assert!(parent.used_values_if_owned(child).is_none());
                        RunRecords::with_unrooted(
                            LayoutPass::for_test(&arena, &scratch),
                            child,
                            root,
                            |unrooted, _| {
                                assert!(unrooted.used_values_if_owned(child).is_none());
                            },
                        );
                    },
                );
                assert!(std::ptr::eq(parent.used_values(root), root_used));
            },
        );
        assert_eq!(scratch.runs_in_progress(), 0);
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
            used_values::RootInputCells::default(),
            geometry::Frame::PHYSICAL,
            None,
            |parent, _| {
                let root_used: *const UsedValues = parent.used_values(root);
                let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
                    RunRecords::with_root(
                        LayoutPass::for_test(&arena, &scratch),
                        child,
                        root,
                        used_values::RootInputCells::default(),
                        geometry::Frame::PHYSICAL,
                        None,
                        |_, _| panic!("abort the nested measurement"),
                    );
                }));
                assert!(result.is_err());
                assert!(std::ptr::eq(parent.used_values(root), root_used));
                assert_eq!(scratch.runs_in_progress(), 1);
            },
        );
    }
}
