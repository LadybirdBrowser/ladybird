/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

/// What the layout stage keeps for itself besides what each of its runs keeps: what the runs of a layout pass share. It
/// is the stage's scratch rather than arena columns: nothing outside a running pass reads it, but for the memos it
/// keeps for later passes.
pub(crate) struct LayoutScratch {
    /// The intrinsic sizes passes measured, kept for the passes after them.
    pub(super) intrinsic_size_caches: super::layout_node_arena::IntrinsicSizeCaches,
    /// The inline items a block container generated, kept for its later runs in the same pass. The
    /// items borrow fonts for the current pass, so the stashes are cleared when the pass ends.
    inline_item_stashes: RefCell<HashMap<NodeSlotId, super::inline_level_iterator::StashedInlineItems>>,
    /// What the pass found out for the host outside any run, which its driver reports itself.
    pub(super) host_findings_outside_runs: RefCell<HostFindings>,
    /// The results of formatting context runs, kept for the runs after them in this pass and the passes after it.
    fc_run_cache_store: super::fc_run_cache::FcRunCacheStore,
    /// The layouts of in-flow blocks, kept for the passes after the one that laid them out.
    in_flow_block_cache_store: super::in_flow_block_cache::InFlowBlockCacheStore,
    /// The document's layout trace while a stage lays the document out, which the stage takes from the arena and hands
    /// back once it is done.
    pub(super) layout_trace: super::trace::LayoutTrace,
    /// The chunks and grapheme segmenters of the text rows the passes laid out.
    text_memos: super::rendered_text::TextMemos,
}

/// What layout found out that the host hears of once the layout that found it is committed: what the
/// container-relative lengths it resolved depend on, and the natural size it negotiated for the box of an `<svg>`
/// document element. A run hands them to its parent with its result, so a discarded result tells the host nothing.
#[derive(Clone, Debug, Default, PartialEq)]
pub(crate) struct HostFindings {
    pub(crate) messages: Vec<super::commit::FfiCommitMessage>,
    pub(crate) document_svg_root_natural_size: Option<(NodeSlotId, crate::css::style::NaturalSize)>,
}

impl HostFindings {
    pub(crate) fn is_empty(&self) -> bool {
        self.messages.is_empty() && self.document_svg_root_natural_size.is_none()
    }

    pub(crate) fn note_container_length_dependencies(
        &mut self,
        element: crate::css::style::tree::StyleNodeID,
        bases: &crate::css::style::ContainerUnitBases,
    ) {
        use super::commit::{FfiCommitMessage, FfiCommitMessageKind};
        self.note_message(FfiCommitMessage::new(
            element.raw(),
            FfiCommitMessageKind::StyleDependsOnSizeContainerQuery,
        ));
        for basis in bases.width_and_height() {
            let Some(container) = basis.container() else {
                continue;
            };
            self.note_message(FfiCommitMessage::new(
                container.raw(),
                FfiCommitMessageKind::SizeQueryContainerResolvedAgainst,
            ));
            if basis.container_has_no_box() {
                self.note_message(FfiCommitMessage::new(
                    container.raw(),
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
        for message in other.messages {
            self.note_message(message);
        }
        if other.document_svg_root_natural_size.is_some() {
            self.document_svg_root_natural_size = other.document_svg_root_natural_size;
        }
    }
}

// The scratch can move to the thread a layout stage runs on.
const _: () = {
    const fn assert_send<T: Send>() {}
    assert_send::<LayoutScratch>();
};

impl Default for LayoutScratch {
    fn default() -> Self {
        Self {
            intrinsic_size_caches: Default::default(),
            inline_item_stashes: RefCell::new(HashMap::default()),
            host_findings_outside_runs: RefCell::new(HostFindings::default()),
            fc_run_cache_store: Default::default(),
            in_flow_block_cache_store: Default::default(),
            layout_trace: Default::default(),
            text_memos: Default::default(),
        }
    }
}

impl LayoutScratch {
    /// The scratch of a fork of the arena, which keeps the run memos the arena's entries were stored for and starts
    /// the rest anew.
    pub(super) fn fork(&self) -> Self {
        Self {
            fc_run_cache_store: self.fc_run_cache_store.clone(),
            in_flow_block_cache_store: self.in_flow_block_cache_store.clone(),
            text_memos: self.text_memos.fork(),
            ..Self::default()
        }
    }

    pub(super) fn fc_run_cache_store(&self) -> &super::fc_run_cache::FcRunCacheStore {
        &self.fc_run_cache_store
    }

    pub(super) fn in_flow_block_cache_store(&self) -> &super::in_flow_block_cache::InFlowBlockCacheStore {
        &self.in_flow_block_cache_store
    }

    pub(super) fn text_memos(&self) -> &super::rendered_text::TextMemos {
        &self.text_memos
    }

    /// Brings the run memos up to date with what the arena's writes since the last call did to their boxes.
    pub(super) fn apply_memo_invalidations(&self, arena: &mut LayoutNodeArena) {
        arena.take_memo_invalidations().apply_to(
            &self.fc_run_cache_store,
            &self.in_flow_block_cache_store,
            &self.text_memos,
        );
    }

    pub(super) fn store_inline_item_stash(
        &self,
        block_container: NodeSlotId,
        stash: super::inline_level_iterator::StashedInlineItems,
    ) {
        self.inline_item_stashes.borrow_mut().insert(block_container, stash);
    }

    pub(super) fn inline_item_stash(
        &self,
        block_container: NodeSlotId,
    ) -> Option<super::inline_level_iterator::StashedInlineItems> {
        self.inline_item_stashes.borrow().get(&block_container).cloned()
    }

    /// What the pass found out outside any run, which its driver reports itself.
    pub(crate) fn take_host_findings_outside_runs(&self) -> HostFindings {
        std::mem::take(&mut self.host_findings_outside_runs.borrow_mut())
    }

    /// Lets go of what the pass that just committed into `arena` kept for itself, and sweeps the run cache.
    pub(crate) fn end_layout_pass(&self, arena: &mut LayoutNodeArena) {
        self.apply_memo_invalidations(arena);
        arena.sweep_stale_fc_run_cache_entries(&self.fc_run_cache_store);
        self.inline_item_stashes.borrow_mut().clear();
    }
}

/// What a run keeps while it is in progress: its root, the percentage block size dependencies it resolves, the
/// anchor-size() resolutions of its root and what it found out for the host. It keeps no metrics of any box: the
/// formatting context or call that lays out a box holds the box's cells, or hands the box to a run of its own. The
/// scoped constructors lend this to the run, so callers cannot extend its lifetime.
pub(crate) struct RunState {
    root: Node,
    content_sizing: ContentSizing,
    /// What the run found so far about the percentage block size dependency of its root and of the boxes whose
    /// layout in it is in progress, the innermost last. Only these boxes have their dependency resolved.
    percentage_block_size_dependencies_in_progress: PercentageBlockSizeDependenciesInProgress,
    /// The anchor-size() resolutions of an absolutely positioned root, a run input that is constant for the run.
    root_anchor_resolved_values: Option<Box<style_values::AnchorResolvedValues>>,
    /// What the run found out for the host so far, which the pass the run is handed notes into.
    host_findings: RefCell<HostFindings>,
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
    /// A child as the run of its parent left it in `cells`, with the `baselines` of its contents, where that run
    /// sizes the child itself instead of handing it to a run of its own.
    pub(crate) fn sized_here(cells: used_values::RootInputCells, baselines: DerivedBaselines) -> Self {
        Self {
            cells,
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
pub(crate) struct PercentageBlockSizeDependencyTracking<'state> {
    dependencies_in_progress: &'state PercentageBlockSizeDependenciesInProgress,
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

impl RunState {
    /// Runs `run` with what a run rooted at `root` keeps, and the pass as that run sees it. The style reads of an
    /// absolutely positioned root see `root_anchor_resolved_values`.
    pub(crate) fn with_root<R>(
        callbacks: LayoutPass<'_>,
        root: Node,
        root_containing_block: Node,
        root_anchor_resolved_values: Option<Box<style_values::AnchorResolvedValues>>,
        run: impl for<'run> FnOnce(&'run Self, LayoutPass<'run>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            root_anchor_resolved_values,
            ContentSizing::Layout,
            run,
        )
    }

    pub(crate) fn with_content_sizing_root<R>(
        callbacks: LayoutPass<'_>,
        root: Node,
        root_containing_block: Node,
        run: impl for<'run> FnOnce(&'run Self, LayoutPass<'run>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            None,
            ContentSizing::RootContributions,
            run,
        )
    }

    pub(crate) fn with_content_inline_sizes_root<R>(
        callbacks: LayoutPass<'_>,
        root: Node,
        root_containing_block: Node,
        run: impl for<'run> FnOnce(&'run Self, LayoutPass<'run>) -> R,
    ) -> R {
        Self::enter(
            callbacks,
            root,
            root_containing_block,
            None,
            ContentSizing::RootContentInlineSizes,
            run,
        )
    }

    fn enter<R>(
        callbacks: LayoutPass<'_>,
        root: Node,
        root_containing_block: Node,
        root_anchor_resolved_values: Option<Box<style_values::AnchorResolvedValues>>,
        content_sizing: ContentSizing,
        run: impl for<'run> FnOnce(&'run Self, LayoutPass<'run>) -> R,
    ) -> R {
        debug_assert!(
            !callbacks.arena().has_intrinsic_size_cache_drops(),
            "the layout stage drops released intrinsic size cache entries before its runs read a cache"
        );
        let state = Self {
            root,
            content_sizing,
            percentage_block_size_dependencies_in_progress: RefCell::new(smallvec::smallvec![(
                root,
                PercentageBlockSizeDependency::default()
            )]),
            root_anchor_resolved_values,
            host_findings: RefCell::new(HostFindings::default()),
        };
        run(
            &state,
            callbacks.for_run(root, root_containing_block, &state.host_findings),
        )
    }

    /// What this run found out for the host, which its outputs carry to the run that uses its result.
    pub(crate) fn take_host_findings(&self) -> Option<Box<HostFindings>> {
        let findings = std::mem::take(&mut *self.host_findings.borrow_mut());
        (!findings.is_empty()).then(|| Box::new(findings))
    }

    pub(crate) fn is_content_sizing(&self) -> bool {
        self.content_sizing != ContentSizing::Layout
    }

    pub(crate) fn measures_content_inline_sizes_of(&self, node: Node) -> bool {
        self.content_sizing == ContentSizing::RootContentInlineSizes && node == self.root
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

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_nested_run_keeps_its_own_root_and_host_findings() {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let root = arena.allocate_for_test().slot;
        let child = arena.allocate_for_test().slot;
        RunState::with_root(
            LayoutPass::for_test(&arena, &scratch),
            root,
            NodeSlotId::INVALID,
            None,
            |parent, parent_callbacks| {
                assert_eq!(parent.root(), root);
                let nested_findings = RunState::with_root(parent_callbacks, child, root, None, |nested, callbacks| {
                    assert_eq!(nested.root(), child);
                    assert!(nested.resolves_percentage_block_size_dependency_of(child));
                    assert!(!nested.resolves_percentage_block_size_dependency_of(root));
                    assert!(!parent.resolves_percentage_block_size_dependency_of(child));
                    assert!(nested.anchor_resolved_values_of(child).is_none());
                    callbacks.note_host_findings(|findings| {
                        findings.document_svg_root_natural_size =
                            Some((child, crate::css::style::NaturalSize::default()));
                    });
                    nested.take_host_findings()
                });
                assert_eq!(parent.root(), root);
                assert!(parent.take_host_findings().is_none());
                assert_eq!(
                    nested_findings.and_then(|findings| findings.document_svg_root_natural_size),
                    Some((child, crate::css::style::NaturalSize::default()))
                );
            },
        );
        assert!(scratch.take_host_findings_outside_runs().is_empty());
    }
}
