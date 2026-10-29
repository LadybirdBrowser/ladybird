/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(super) const CALC_NUMERIC_KIND_LENGTH: u8 = 4;

/// The anchor() inset resolutions of one positioned box, produced by the
/// abspos engine's resolve pass; sides without anchor functions stay
/// unresolved and read from style.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ResolvedAnchorInsets {
    pub(crate) top: Option<style_values::ResolvedInsetOverride>,
    pub(crate) right: Option<style_values::ResolvedInsetOverride>,
    pub(crate) bottom: Option<style_values::ResolvedInsetOverride>,
    pub(crate) left: Option<style_values::ResolvedInsetOverride>,
}

impl ResolvedAnchorInsets {
    pub(crate) fn override_for(&self, side: geometry::Side) -> Option<style_values::ResolvedInsetOverride> {
        match side {
            geometry::Side::Top => self.top,
            geometry::Side::Right => self.right,
            geometry::Side::Bottom => self.bottom,
            geometry::Side::Left => self.left,
        }
    }
}

pub(super) fn point_add(left: FfiCssPixelPoint, right: FfiCssPixelPoint) -> FfiCssPixelPoint {
    FfiCssPixelPoint {
        x: left.x + right.x,
        y: left.y + right.y,
    }
}

pub(super) fn point_sub(left: FfiCssPixelPoint, right: FfiCssPixelPoint) -> FfiCssPixelPoint {
    FfiCssPixelPoint {
        x: left.x - right.x,
        y: left.y - right.y,
    }
}

pub(crate) fn translate_static_position_rect(
    mut rect: abspos_inputs::StaticPositionRect,
    offset: FfiCssPixelPoint,
) -> abspos_inputs::StaticPositionRect {
    rect.rect.offset.inline_offset += offset.x;
    rect.rect.offset.block_offset += offset.y;
    rect
}

pub(crate) type Node = NodeSlotId;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CyclicPercentageIntrinsicContribution {
    NotCyclic,
    ResolveAsZero,
    TreatAsInitialValue,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum CyclicPercentageSizeProperty {
    PreferredOrMaxSize,
    MinSize,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct PixelFraction {
    pub(crate) numerator: CssPixels,
    pub(crate) denominator: CssPixels,
}

/// A replaced box's natural size and aspect ratio, any of which it can lack.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ReplacedIntrinsicSize {
    pub(crate) width: Option<CssPixels>,
    pub(crate) height: Option<CssPixels>,
    pub(crate) aspect_ratio: Option<PixelFraction>,
}

#[derive(Clone, Copy, Debug, Default)]
pub(super) struct ReplacedMaxContentSizeLimits {
    pub(super) definite_size_in_ratio_determining_axis: Option<CssPixels>,
    pub(super) minimum_inline_size: Option<CssPixels>,
    pub(super) minimum_block_size: Option<CssPixels>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(super) enum SizeDimension {
    Inline,
    Block,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TableWrapperInlineSizeMode {
    ClampToAvailableInlineSize,
    UseTableUsedInlineSizeIfNotAuto,
}

pub(crate) fn run_builds_fragments(input: &LayoutInput, parent_collects_fragments: bool) -> bool {
    parent_collects_fragments || input.participation != ParticipationInParentFormattingContext::Root
}

pub(crate) struct RootLaidOutWithoutParentFragments {
    pub(crate) result: ChildLayoutResult,
    pub(crate) root_cells_left_by_run: used_values::RootInputCells,
}

pub(crate) fn run_root_without_parent_fragments(
    callbacks: LayoutPass<'_>,
    node: Node,
    root_cells: used_values::RootInputCells,
    input: LayoutInput,
) -> RootLaidOutWithoutParentFragments {
    debug_assert_ne!(
        input.participation,
        ParticipationInParentFormattingContext::Item,
        "an item run leaves its root's input cells to its parent, so they would come back unchanged"
    );
    let fc_type = independent_formatting_context_type(node, &callbacks);
    let root_containing_block = callbacks.containing_block_for_child_run(node, &input.participation);
    let ChildRunOutcome {
        result,
        laid_out_box,
        fragment_arrival,
        ..
    } = run_formatting_context(
        None,
        root_cells,
        node,
        root_containing_block,
        None,
        fc_type,
        false,
        callbacks,
        input,
        None,
    );
    debug_assert!(
        fragment_arrival.is_none(),
        "a run without parent fragments hands its placement nothing"
    );
    RootLaidOutWithoutParentFragments {
        result,
        root_cells_left_by_run: laid_out_box.cells,
    }
}

pub(crate) fn run_fragmentainer(
    outer: &FormattingContextRun,
    outer_root_cells: used_values::RootInputCells,
    builds_fragments: bool,
    input: LayoutInput,
    fragmentainer: std::rc::Rc<fragmentation::FragmentainerContext>,
) -> RunOutputs {
    debug_assert_eq!(input.participation, ParticipationInParentFormattingContext::Root);
    let box_ = outer.box_;
    let root_containing_block = outer
        .callbacks
        .containing_block_for_child_run(box_, &input.participation);
    execute_formatting_context_run(
        builds_fragments,
        outer_root_cells,
        box_,
        root_containing_block,
        None,
        FormattingContextType::Block,
        outer.should_collect_devtools_layout_data,
        outer.callbacks,
        input,
        None,
        None,
        Some(fragmentainer),
    )
}

pub(super) fn cache_key(
    measured_at_inline_size: Option<CssPixels>,
    measured_at_block_size: Option<CssPixels>,
    constraints: ContainingBlockConstraints,
) -> IntrinsicSizeCacheKey {
    IntrinsicSizeCacheKey {
        measured_at_inline_size,
        measured_at_block_size,
        percentage_basis_inline_size: constraints.percentage_basis_inline_size,
        percentage_basis_block_size: constraints.percentage_basis_block_size,
        quirks_mode_percentage_basis_block_size: constraints.quirks_mode_percentage_basis_block_size,
        measured_in_transposed_frame: false,
    }
}

impl PixelFraction {
    pub(crate) fn new(numerator: CssPixels, denominator: CssPixels) -> Self {
        assert_ne!(denominator, CssPixels::default());
        Self { numerator, denominator }
    }

    pub(crate) fn zero() -> Self {
        Self::new(CssPixels::default(), CssPixels::from_integer(1))
    }

    pub(crate) fn multiply(self, value: CssPixels) -> CssPixels {
        if self.denominator == CssPixels::default() {
            return CssPixels::default();
        }
        let wide = value.raw_value() as i64 * self.numerator.raw_value() as i64;
        CssPixels::from_raw((wide / self.denominator.raw_value() as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32)
    }

    pub(crate) fn divide(self, value: CssPixels) -> CssPixels {
        if self.numerator == CssPixels::default() {
            return CssPixels::default();
        }
        let wide = value.raw_value() as i64 * self.denominator.raw_value() as i64;
        CssPixels::from_raw((wide / self.numerator.raw_value() as i64).clamp(i32::MIN as i64, i32::MAX as i64) as i32)
    }

    pub(crate) fn max(self, other: Self) -> Self {
        let left = self.numerator.raw_value() as i64 * other.denominator.raw_value() as i64;
        let right = other.numerator.raw_value() as i64 * self.denominator.raw_value() as i64;
        if left >= right { self } else { other }
    }
}

pub(crate) fn cyclic_percentage_intrinsic_contribution(
    is_replaced_box: bool,
    size_contains_percentage: bool,
    available_size: AvailableSize,
    size_property: CyclicPercentageSizeProperty,
) -> CyclicPercentageIntrinsicContribution {
    if !size_contains_percentage {
        return CyclicPercentageIntrinsicContribution::NotCyclic;
    }
    // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
    // For the min size properties, as well as for margins and paddings (and gutters), a cyclic percentage is resolved
    // against zero for determining intrinsic size contributions.
    if size_property == CyclicPercentageSizeProperty::MinSize && available_size.is_intrinsic_sizing_constraint() {
        return CyclicPercentageIntrinsicContribution::ResolveAsZero;
    }
    // If the box is non-replaced, then the entire value of any max size property or preferred size property
    // ('width'/'max-width'/'height'/'max-height') specified as an expression containing a percentage (such as '10%' or
    // 'calc(10px + 0%)') that is cyclic is treated for the purpose of calculating the box's intrinsic size contributions
    // only as that property's initial value.
    if available_size == AvailableSize::MinContent {
        if is_replaced_box {
            // If the box is replaced, a cyclic percentage in the value of any max size property or preferred size property
            // ('width'/'max-width'/'height'/'max-height'), is resolved against zero when calculating the min-content
            // contribution in the corresponding axis.
            return CyclicPercentageIntrinsicContribution::ResolveAsZero;
        }
        return CyclicPercentageIntrinsicContribution::TreatAsInitialValue;
    }
    if available_size == AvailableSize::MaxContent {
        // Likewise, if the box is replaced, then the entire value of any max size property or preferred size property
        // specified as an expression containing a percentage that is cyclic is treated for the purpose of calculating
        // the box's max-content contributions only as that property's initial value.
        return CyclicPercentageIntrinsicContribution::TreatAsInitialValue;
    }
    CyclicPercentageIntrinsicContribution::NotCyclic
}

pub(crate) fn subtract_border_box_adjustment(
    value: CssPixels,
    before_border: CssPixels,
    before_padding: CssPixels,
    after_border: CssPixels,
    after_padding: CssPixels,
) -> CssPixels {
    (value - before_border - before_padding - after_border - after_padding).max(CssPixels::default())
}

pub(crate) fn content_block_size_from_aspect_ratio_values(
    content_inline_size: CssPixels,
    ratio: PixelFraction,
    use_border_box: bool,
    inline_before: CssPixels,
    inline_after: CssPixels,
    block_before: CssPixels,
    block_after: CssPixels,
) -> CssPixels {
    // NB: Intrinsic grid sizing can transfer an aspect ratio before block-axis border metrics are copied into the layout
    //     state. Border widths are already definite at computed-value time, while padding remains resolved in the state.
    if ratio.numerator == CssPixels::default() {
        return CssPixels::default();
    }
    if use_border_box {
        return (ratio.divide(content_inline_size + inline_before + inline_after) - block_before - block_after)
            .max(CssPixels::default());
    }
    ratio.divide(content_inline_size)
}

pub(crate) fn content_inline_size_from_aspect_ratio_values(
    content_block_size: CssPixels,
    ratio: PixelFraction,
    use_border_box: bool,
    inline_before: CssPixels,
    inline_after: CssPixels,
    block_before: CssPixels,
    block_after: CssPixels,
) -> CssPixels {
    if ratio.numerator == CssPixels::default() {
        return CssPixels::default();
    }
    if use_border_box {
        return (ratio.multiply(content_block_size + block_before + block_after) - inline_before - inline_after)
            .max(CssPixels::default());
    }
    ratio.multiply(content_block_size)
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum BaselineSet {
    First,
    Last,
}

/// Lays out absolutely positioned boxes whose containing blocks are placed, each in the frame of its containing
/// block's writing mode, and returns where each was placed.
pub(crate) fn lay_out_pending_abspos_children(
    run: &FormattingContextRun,
    batch: Vec<abspos_inputs::PendingAbsposChild>,
) -> Vec<fragment_tree::PlacedAbsposBox> {
    let engine = abspos_engine::AbsposEngine::for_run(run);
    let mut placed = Vec::with_capacity(batch.len());
    for entry in batch {
        let child_box = entry.child_box;
        let containing_block = entry.containing_block();
        let containing_block_frame = frame_established_by(&run.callbacks, containing_block);
        let border_box_block_end = if containing_block_frame == run.callbacks.frame {
            engine.layout_pending_child(run, entry)
        } else {
            let framed_run = run.in_frame_of(containing_block_frame);
            abspos_engine::AbsposEngine::for_run(&framed_run).layout_pending_child(&framed_run, entry)
        };
        placed.push(fragment_tree::PlacedAbsposBox {
            box_: child_box,
            border_box_block_end,
        });
    }
    placed
}

/// What placing a child takes from its layout and from its parent: what the child's fragment shows of the box's own
/// layout, in the frame of the run placing it, and what the parent decided about the child besides where it goes.
#[derive(Clone)]
pub(crate) struct PlacedChild {
    pub(crate) geometry: fragment_tree::FragmentGeometry,
    /// What the parent decided about the child before placing it, which placing it adds the placement to.
    pub(crate) facts_decided_by_parent: run_state::FactsDecidedByParent,
    /// The offset the relative positioning of the fragmented inline boxes a block-level child interrupts gives the
    /// child's committed fragment.
    pub(crate) offset_from_relatively_positioned_inline_ancestors: FfiCssPixelPoint,
    /// For an absolutely positioned child, the inputs it was laid out with, which name the containing block it is
    /// placed in.
    pub(crate) abspos_layout_inputs: Option<abspos_inputs::AbsposLayoutInputs>,
}

impl PlacedChild {
    /// A child whose fragment shows `geometry` before its parent decided anything about it.
    pub(crate) fn new(geometry: fragment_tree::FragmentGeometry) -> Self {
        Self {
            geometry,
            facts_decided_by_parent: run_state::FactsDecidedByParent::default(),
            offset_from_relatively_positioned_inline_ancestors: FfiCssPixelPoint::default(),
            abspos_layout_inputs: None,
        }
    }

    /// A child whose run handed back the box `laid_out`, before its parent decided anything about it.
    pub(crate) fn of_laid_out_box(laid_out: &run_state::LaidOutChildBox) -> Self {
        Self::new(fragment_tree::FragmentGeometry {
            cells: laid_out.cells,
            line_data: laid_out.line_data.clone(),
        })
    }

    /// A box that `run` lays out in `cells`, which hold it in the run's frame, with `lines`, before its parent decided
    /// anything about it.
    pub(crate) fn laid_out_here(
        cells: used_values::RootInputCells,
        lines: Option<used_values::LineData>,
        run: &FormattingContextRun,
    ) -> Self {
        let callbacks = &run.callbacks;
        Self::new(fragment_tree::FragmentGeometry {
            cells,
            // NB: Only a run that builds fragments shows the lines it laid out in the boxes it places.
            line_data: run
                .fragments
                .is_some()
                .then(|| {
                    lines.map(|lines| {
                        std::sync::Arc::new(inline_content::InlineContent::finish(
                            lines,
                            callbacks.arena(),
                            cells.content_inline_size,
                            callbacks.frame.writing_mode(),
                        ))
                    })
                })
                .flatten(),
        })
    }

    /// The child with the insets its parent resolved for it as an in-flow box.
    pub(crate) fn with_in_flow_box_insets(mut self, insets: abspos_engine::InFlowBoxInsets) -> Self {
        if let Some(used_insets) = insets.used_insets {
            self.facts_decided_by_parent.used_insets = used_insets;
        }
        if let Some(default_scroll_shift) = insets.default_scroll_shift {
            self.facts_decided_by_parent.default_scroll_shift = Some(default_scroll_shift);
        }
        self
    }

    /// The child with what its parent decided its fragment shows of the parent's layout.
    pub(crate) fn with_fragment_facts(mut self, fragment_facts: run_state::FragmentFactsDecidedByParent) -> Self {
        self.facts_decided_by_parent.fragment_facts = fragment_facts;
        self
    }

    /// The child, a block-level box, with the offset the relative positioning of the inline boxes it interrupts gives
    /// it.
    pub(crate) fn with_offset_from_relatively_positioned_inline_ancestors(mut self, offset: FfiCssPixelPoint) -> Self {
        self.offset_from_relatively_positioned_inline_ancestors = offset;
        self
    }
}

/// Places `node` at `offset` in its containing block, taking in what the layout of the box handed its placement. An
/// absolutely positioned child is placed in the containing block its layout inputs name, and hands its link those
/// inputs.
pub(crate) fn place_child(
    run: &FormattingContextRun,
    node: Node,
    placed_child: PlacedChild,
    offset: FfiCssPixelPoint,
    containing_line_box_fragment: Option<used_values::LineBoxFragmentCoordinate>,
    fragment_arrival: Option<fragment_tree::ChildFragmentArrival>,
) -> Option<fragment_tree::PlacedFragment> {
    let abspos_layout_inputs = placed_child.abspos_layout_inputs;
    let containing_block = abspos_layout_inputs.map_or_else(
        || run.callbacks.in_flow_containing_block(node),
        |inputs| inputs.containing_block,
    );
    let callbacks = &run.callbacks;
    let placement = run_state::BoxPlacement {
        content_offset_in_containing_block: offset,
        containing_block,
    };
    let Some(fragments) = run.fragments.as_deref() else {
        debug_assert!(
            fragment_arrival.is_none(),
            "a run that builds no fragments is handed none"
        );
        return None;
    };
    fragments.note_placed_box(
        node,
        placement,
        cells_kept_for_contained_abspos(run, node, || placed_child.geometry.cells),
    );
    let PlacedChild {
        geometry,
        facts_decided_by_parent,
        offset_from_relatively_positioned_inline_ancestors,
        ..
    } = placed_child;
    debug_assert!(
        facts_decided_by_parent.placement.is_none(),
        "a child is handed to placement before it is placed"
    );
    let facts_decided_by_parent = run_state::FactsDecidedByParent {
        placement: Some(placement),
        ..facts_decided_by_parent
    };
    debug_assert!(
        fragment_arrival
            .as_ref()
            .is_none_or(|arrival| arrival.node() == node && !fragments.left_child_unplaced(node)),
        "a box is placed with fragments handed to it for another box, or after it was left unplaced"
    );
    let mut child_run_root = None;
    let committed_fragment = match fragment_arrival {
        Some(fragment_tree::ChildFragmentArrival::CommittedFragment {
            fragment,
            reuses_subtree,
        }) => Some((
            fragment_tree::committed_fragment_showing_box(
                callbacks,
                node,
                || geometry.clone(),
                facts_decided_by_parent.fragment_facts,
                fragment,
            ),
            reuses_subtree,
        )),
        Some(fragment_tree::ChildFragmentArrival::RunRoot {
            root,
            reuses_committed_subtree,
        }) => {
            child_run_root = Some((root, reuses_committed_subtree));
            None
        }
        None => None,
    };
    if let Some((fragment, reuses_subtree)) = committed_fragment {
        fragments.attach_committed_fragment(
            node,
            fragment.clone(),
            reuses_subtree,
            (!containing_block.is_invalid()).then_some(containing_block),
            facts_decided_by_parent,
            abspos_layout_inputs,
            resolve_containing_line_box_index(callbacks, node, containing_line_box_fragment),
            point_add(
                offset,
                committed_offset_delta_at_placement(
                    callbacks,
                    node,
                    facts_decided_by_parent.used_insets,
                    offset_from_relatively_positioned_inline_ancestors,
                ),
            ),
            callbacks.frame,
        );
        return Some(fragment_tree::PlacedFragment {
            fragment,
            exports_payloads: false,
            abspos_boxes_placed_with_it: Vec::new(),
        });
    }
    match child_run_root {
        Some((root, reuses_committed_subtree)) => {
            fragments.take_in_child_root_at_placement(node, root, reuses_committed_subtree, callbacks);
        }
        None => fragments.open_fragment_of_placed_box(node),
    }
    let mut abspos_boxes_placed_with_it = Vec::new();
    loop {
        let batch = fragments.take_drainable_abspos(node, callbacks);
        if batch.is_empty() {
            break;
        }
        abspos_boxes_placed_with_it.extend(lay_out_pending_abspos_children(run, batch));
    }
    let node_facts = NodeFacts::new(callbacks, node);
    let own_anchor_candidate_border_box_rect = (node_facts.is_box() && node_facts.has_anchor_names()).then(|| {
        let metrics = &geometry.cells;
        let collapsed = metrics.uses_collapsing_borders_model();
        CssPixelRect {
            x: offset.x - metrics.border_box_left(collapsed),
            y: offset.y - metrics.border_box_top(collapsed),
            width: metrics.border_box_inline_size(collapsed),
            height: metrics.border_box_block_size(collapsed),
        }
    });
    let placed = fragments.build_fragment_for_placed_box(
        callbacks,
        node,
        (!containing_block.is_invalid()).then_some(containing_block),
        geometry,
        facts_decided_by_parent,
        abspos_layout_inputs,
        resolve_containing_line_box_index(callbacks, node, containing_line_box_fragment),
        point_add(
            offset,
            committed_offset_delta_at_placement(
                callbacks,
                node,
                facts_decided_by_parent.used_insets,
                offset_from_relatively_positioned_inline_ancestors,
            ),
        ),
        own_anchor_candidate_border_box_rect,
    );
    Some(fragment_tree::PlacedFragment {
        abspos_boxes_placed_with_it,
        ..placed
    })
}

/// The sizes and box sides `cells_in_run_frame` of `node`, which `run` is placing, where the box may be the containing
/// block of absolutely positioned boxes the run lays out at its placement or later.
fn cells_kept_for_contained_abspos(
    run: &FormattingContextRun,
    node: Node,
    cells_in_run_frame: impl FnOnce() -> used_values::RootInputCells,
) -> Option<fragment_tree::CellsInFrame> {
    let data = run.callbacks.node_data(node);
    let may_contain_abspos = node_facts::has_flag(data, NodeFlag::EstablishesAbsolutePositionContainingBlock)
        || node_facts::has_flag(data, NodeFlag::EstablishesFixedPositionContainingBlock)
        || data.parent.get().is_invalid();
    may_contain_abspos.then(|| fragment_tree::CellsInFrame {
        cells: cells_in_run_frame(),
        frame: run.callbacks.frame,
    })
}

/// The index of the line box that `node` is placed on. Both callers that place a box on a line name one that exists:
/// atomic inlines are placed while walking the laid-out lines, and an interrupting block gets its line before it is
/// laid out.
fn resolve_containing_line_box_index(
    callbacks: &LayoutPass<'_>,
    node: Node,
    coordinate: Option<used_values::LineBoxFragmentCoordinate>,
) -> Option<usize> {
    let coordinate = coordinate?;
    NodeFacts::new(callbacks, node)
        .is_non_fragmented_box()
        .then_some(coordinate.line_box_index)
}

fn committed_offset_delta_at_placement(
    callbacks: &LayoutPass<'_>,
    node: Node,
    used_insets: run_state::UsedInsets,
    offset_from_relatively_positioned_inline_ancestors: FfiCssPixelPoint,
) -> FfiCssPixelPoint {
    let mut delta = FfiCssPixelPoint::default();
    let facts = NodeFacts::new(callbacks, node);
    if !facts.is_non_fragmented_box() {
        return delta;
    }
    if facts.is_relatively_positioned() {
        delta.x += used_insets.left;
        delta.y += used_insets.top;
    }
    delta.x += offset_from_relatively_positioned_inline_ancestors.x;
    delta.y += offset_from_relatively_positioned_inline_ancestors.y;
    delta
}

pub(crate) fn register_contained_abspos_child(
    callbacks: &LayoutPass<'_>,
    fragments: Option<&fragment_tree::RunFragmentBuilder>,
    coordinate_space_box: Node,
    child: Node,
    static_position_rect: abspos_inputs::StaticPositionRect,
    containing_block_info_override: Option<abspos_inputs::AbsposContainingBlockInfo>,
) {
    let Some(fragments) = fragments else {
        return;
    };
    let mut entry = abspos_inputs::PendingAbsposChild {
        child_box: child,
        coordinate_space_box,
        static_position_rect,
        containing_block_info_override,
        containing_block_search: abspos_inputs::ContainingBlockSearch::starting_at(
            child,
            NodeFacts::new(callbacks, child).is_fixed_position(),
        ),
    };
    resolve_pending_abspos_containing_block(callbacks, &mut entry, fragments.root_node());
    fragments.register_pending_abspos(coordinate_space_box, entry);
}

/// The containing block `node` is laid out in, within the run rooted at `run_root`.
pub(crate) fn containing_block_laid_out_in(callbacks: &LayoutPass<'_>, node: Node, run_root: Node) -> Node {
    let facts = NodeFacts::new(callbacks, node);
    if !facts.is_absolutely_positioned() {
        return callbacks.in_flow_containing_block(node);
    }
    let mut search = abspos_inputs::ContainingBlockSearch::starting_at(node, facts.is_fixed_position());
    callbacks
        .arena()
        .continue_containing_block_search(&mut search, run_root);
    search.containing_block
}

pub(crate) fn resolve_pending_abspos_containing_block(
    callbacks: &LayoutPass<'_>,
    entry: &mut abspos_inputs::PendingAbsposChild,
    limit: Node,
) {
    if !entry.containing_block().is_invalid() {
        return;
    }
    callbacks
        .arena()
        .continue_containing_block_search(&mut entry.containing_block_search, limit);
}

pub(crate) fn box_baseline_with_content_baselines(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    metrics: &impl BoxMetricCells,
    baseline_set: BaselineSet,
    content_baselines: DerivedBaselines,
) -> CssPixels {
    let facts = NodeFacts::new(callbacks, box_);
    let style = StyleValues::for_node(callbacks, box_);
    let collapsed = metrics.uses_collapsing_borders_model();

    // https://drafts.csswg.org/css2/#propdef-vertical-align
    if facts.vertical_align_applies() && style.vertical_align_is_keyword() {
        match style.vertical_align_keyword() {
            vertical_align::TOP => {
                // Top: Align the top of the aligned subtree with the top of the line box.
                return metrics.border_box_top(collapsed);
            }
            vertical_align::MIDDLE => {
                // Middle: Align the vertical midpoint of the box with the baseline of the parent box plus half the x-height of the parent.
                let containing_block = callbacks.in_flow_containing_block(box_);
                assert!(!containing_block.is_invalid());
                let containing_style = StyleValues::for_node(callbacks, containing_block);
                return metrics.margin_box_block_size(collapsed) / 2
                    + CssPixels::nearest_value_for_f32(containing_style.font_x_height() / 2.0);
            }
            vertical_align::_LIBWEB_BASELINE_MIDDLE => {
                // NB: The vertical midpoint of the box is aligned with the baseline of the parent box.
                return metrics.margin_box_block_size(collapsed) / 2;
            }
            vertical_align::BOTTOM => {
                // Bottom: Align the bottom of the aligned subtree with the bottom of the line box.
                return metrics.content_block_size() + metrics.margin_box_top(collapsed);
            }
            vertical_align::TEXT_TOP => {
                // TextTop: Align the top of the box with the top of the parent's content area (see 10.6.1).
                return style.font_size();
            }
            vertical_align::TEXT_BOTTOM => {
                // TextBottom: Align the bottom of the box with the bottom of the parent's content area (see 10.6.1).
                let containing_block = callbacks.in_flow_containing_block(box_);
                assert!(!containing_block.is_invalid());
                let containing_style = StyleValues::for_node(callbacks, containing_block);
                return metrics.margin_box_block_size(collapsed)
                    - CssPixels::nearest_value_for_f32(containing_style.font_descent());
            }
            _ => {}
        }
    }

    // INTEROP: A range input's baseline is its bottom border edge in Chrome, whatever its internal layout.
    if facts.is_range_input_box() {
        return metrics.margin_box_top(collapsed) + metrics.content_block_size() + metrics.border_box_bottom(collapsed);
    }

    let display = facts.display();
    let is_flex_or_grid_container = display.is_flex_inside() || display.is_grid_inside();

    // https://drafts.csswg.org/css2/#propdef-vertical-align
    // The baseline of an 'inline-block' is the baseline of its last line box in the normal flow, unless it has either
    // no in-flow line boxes or if its 'overflow' property has a computed value other than 'visible', in which case the
    // baseline is the bottom margin edge.
    // https://drafts.csswg.org/css-align-3/#baseline-rules
    // CSS Align restates this overflow exception as only applying to the last baseline set: "for legacy reasons if its
    // baseline-source is auto (the initial value) a block-level or inline-level block container that is a scroll
    // container always has a last baseline set, whose baselines all correspond to its block-end margin edge". First
    // baseline sets always derive from content; so do flex and grid containers, which are not block containers.
    // FIXME: Per CSS Align, a scroll container's content-derived baseline position should be clamped to its border
    //        edge.
    let derive_baseline_from_content = baseline_set == BaselineSet::First
        || style.baseline_source() != crate::css::css_enums::baseline_source::AUTO
        || is_flex_or_grid_container
        || !facts.is_scroll_container();

    // AD-HOC: We also use the content-derived baseline for <input> elements with block children, even when their
    //         overflow makes them scroll containers. The internal shadow tree baseline should determine the control's
    //         baseline for proper alignment with adjacent text.
    //         https://html.spec.whatwg.org/multipage/rendering.html#form-controls
    let input_derives_from_children = facts.is_html_input_element() && !facts.children_are_inline();

    let content_baseline = match baseline_set {
        BaselineSet::First => content_baselines.first,
        BaselineSet::Last => content_baselines.last,
    };
    if let Some(content_baseline) = content_baseline
        && (derive_baseline_from_content || input_derives_from_children)
    {
        return metrics.margin_box_top(collapsed) + content_baseline;
    }

    // AD-HOC: Inline buttons without a content baseline synthesize one from their content-box bottom edge in
    //         Chromium and Firefox, rather than the margin-box edge used by ordinary inline-blocks.
    if facts.uses_button_layout() && display.is_inline_outside() && content_baseline.is_none() {
        return metrics.margin_box_top(collapsed) + metrics.content_block_size();
    }

    // If the box has no baseline set, the bottom margin edge of the box is metrics.
    metrics.margin_box_block_size(collapsed)
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum BaselinePurpose {
    Container,
    InlineAlignment,
}

pub(crate) fn inline_box_baseline(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    metrics: &impl BoxMetricCells,
    content_baselines: DerivedBaselines,
) -> CssPixels {
    box_baseline_for_purpose(
        callbacks,
        box_,
        metrics,
        BaselineSet::Last,
        content_baselines,
        BaselinePurpose::InlineAlignment,
    )
}

fn box_baseline_for_purpose(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    metrics: &impl BoxMetricCells,
    mut baseline_set: BaselineSet,
    mut content_baselines: DerivedBaselines,
    purpose: BaselinePurpose,
) -> CssPixels {
    if purpose == BaselinePurpose::InlineAlignment {
        let facts = NodeFacts::new(callbacks, box_);
        let display = facts.display();
        // https://drafts.csswg.org/css-inline-3/#baseline-source
        // auto: Specifies last-baseline alignment for inline-block, first-baseline alignment for everything else.
        // INTEROP: Flex and grid containers contribute their first baseline through inline-block ancestors too.
        // NB: A button's anonymous content wrapper preserves the last baseline of multiline button content.
        let flex_or_grid_baseline =
            (display.is_flex_inside() || display.is_grid_inside()) && !facts.is_anonymous_button_content_wrapper();
        // https://www.w3.org/TR/CSS22/tables.html#height-layout
        // The baseline of an 'inline-table' is the baseline of the first row of the table.
        let inline_table_baseline = display.is_inline_outside() && facts.is_table_wrapper();
        use crate::css::css_enums::baseline_source;
        match StyleValues::for_node(callbacks, box_).baseline_source() {
            // https://drafts.csswg.org/css-inline-3/#baseline-source
            // Specifies first-baseline alignment.
            baseline_source::FIRST => baseline_set = BaselineSet::First,
            // Specifies last-baseline alignment.
            baseline_source::LAST => baseline_set = BaselineSet::Last,
            _ if flex_or_grid_baseline || inline_table_baseline => baseline_set = BaselineSet::First,
            _ => content_baselines.last = content_baselines.inline_alignment.or(content_baselines.last),
        }
    }
    box_baseline_with_content_baselines(callbacks, box_, metrics, baseline_set, content_baselines)
}

/// First and last baseline sets derived for one box, relative to its content
/// box. Laying out a box's contents returns them. A formatting context records
/// its own root box's baselines on the instance. The side that ran the context
/// or laid the box out hands them back with the box.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct DerivedBaselines {
    pub(crate) first: Option<CssPixels>,
    pub(crate) last: Option<CssPixels>,
    // NB: Inline alignment can export a descendant flex/grid first baseline through block containers while
    //     ordinary container derivation must retain the distinct last baseline set.
    pub(crate) inline_alignment: Option<CssPixels>,
}

/// The first, last and inline-alignment baselines of a line, relative to the content box of its block container.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) struct LineBaselines {
    pub(crate) first: CssPixels,
    pub(crate) last: CssPixels,
    pub(crate) inline_alignment: CssPixels,
}

/// The baselines a line gives its block container where it holds `box_`, a block-level box with `cells` and
/// `content_baselines` whose content box sits `content_block_offset` into the container's content box.
pub(crate) fn baselines_of_block_level_box_on_line(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    cells: &used_values::RootInputCells,
    content_baselines: DerivedBaselines,
    content_block_offset: CssPixels,
) -> LineBaselines {
    let offset_from_margin_edge = content_block_offset - cells.margin_box_top(cells.uses_collapsing_borders_model);
    let baseline = |baseline_set, purpose| {
        offset_from_margin_edge
            + box_baseline_for_purpose(callbacks, box_, cells, baseline_set, content_baselines, purpose)
    };
    LineBaselines {
        first: baseline(BaselineSet::First, BaselinePurpose::Container),
        last: baseline(BaselineSet::Last, BaselinePurpose::Container),
        inline_alignment: baseline(BaselineSet::Last, BaselinePurpose::InlineAlignment),
    }
}

/// The baselines of `lines`, relative to the content box of the block container that holds them.
pub(crate) fn derive_baselines_from_lines(lines: &[line_box::LineBoxData]) -> DerivedBaselines {
    let mut remaining_lines = lines.iter();
    let first = remaining_lines.find(|line| !line.is_empty()).or_else(|| lines.first());
    if let Some(first) = first {
        let last = remaining_lines.rfind(|line| !line.is_empty()).unwrap_or(first);
        let baselines_of_line = |line: &line_box::LineBoxData| {
            if !line.has_block_level_box {
                let baseline = line.physical_vertical_end() - line.block_length + line.baseline;
                return LineBaselines {
                    first: baseline,
                    last: baseline,
                    inline_alignment: baseline,
                };
            }
            assert_eq!(line.fragments.len(), 1);
            line.block_level_box
                .expect("a line interrupted for a block-level box keeps the box")
                .baselines
        };
        return DerivedBaselines {
            first: Some(baselines_of_line(first).first),
            last: Some(baselines_of_line(last).last),
            inline_alignment: Some(baselines_of_line(last).inline_alignment),
        };
    }

    DerivedBaselines::default()
}

impl DerivedBaselines {
    /// Folds in the baselines a child laid out after the children folded in so far gives its container. A block
    /// container with block-level children takes its first baseline from the first in-flow child that has one, and its
    /// last baselines from the last.
    pub(crate) fn fold_in_later_child(&mut self, child: DerivedBaselines) {
        self.first = self.first.or(child.first);
        self.last = child.last.or(self.last);
        self.inline_alignment = child.inline_alignment.or(self.inline_alignment);
    }
}

/// Whether `container`, a block container with block-level children, derives its baselines from those of `child`.
pub(crate) fn child_gives_container_baselines(callbacks: &LayoutPass<'_>, container: Node, child: Node) -> bool {
    let facts = NodeFacts::new(callbacks, container);
    let child_facts = NodeFacts::new(callbacks, child);
    // NB: Flex and grid containers derive their baselines from their items instead.
    child_facts.is_flow_layout_participant()
        && !child_facts.is_floating()
        && !(facts.is_fieldset_box() && facts.rendered_legend() == child)
        && table_formatting_context::child_participates_in_table_run(facts.display(), &child_facts)
        // A table wrapper box has the baselines of its table box, not of a caption above it: "The baseline of an
        // 'inline-table' is the baseline of the first row of the table."
        // https://www.w3.org/TR/CSS22/tables.html#height-layout
        && !(facts.is_table_wrapper() && !child_facts.display().is_table_inside())
}

/// The baselines `child` gives its container, relative to the container's content box.
pub(crate) fn baselines_laid_out_child_gives_container(
    callbacks: &LayoutPass<'_>,
    child: LaidOutChild<'_, impl BoxMetricCells>,
) -> DerivedBaselines {
    let baseline = |baseline_set, purpose| {
        baseline_of_laid_out_child_for_purpose(callbacks, child, baseline_set, ChildBaselineSource::Own, purpose)
    };
    DerivedBaselines {
        first: baseline(BaselineSet::First, BaselinePurpose::Container),
        last: baseline(BaselineSet::Last, BaselinePurpose::Container),
        inline_alignment: baseline(BaselineSet::Last, BaselinePurpose::InlineAlignment),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ChildBaselineSource {
    Own,
    OwnOrSynthesized,
    Synthesized,
}

/// A child its parent laid out: its sizes and box sides, the baselines of its contents, and the offset of its content
/// box in its containing block, or the origin while it is not placed.
pub(crate) struct LaidOutChild<'metrics, Metrics: BoxMetricCells> {
    pub(crate) node: Node,
    pub(crate) metrics: &'metrics Metrics,
    pub(crate) content_baselines: DerivedBaselines,
    pub(crate) content_offset: FfiCssPixelPoint,
}

impl<Metrics: BoxMetricCells> Clone for LaidOutChild<'_, Metrics> {
    fn clone(&self) -> Self {
        *self
    }
}

impl<Metrics: BoxMetricCells> Copy for LaidOutChild<'_, Metrics> {}

/// The baseline of `child`, relative to its container's margin edge. A synthesized baseline comes from the child's
/// border box.
pub(crate) fn baseline_of_laid_out_child_for_purpose(
    callbacks: &LayoutPass<'_>,
    child: LaidOutChild<'_, impl BoxMetricCells>,
    baseline_set: BaselineSet,
    source: ChildBaselineSource,
    purpose: BaselinePurpose,
) -> Option<CssPixels> {
    let LaidOutChild {
        node,
        metrics: child_metrics,
        content_baselines,
        content_offset,
    } = child;
    let has_baseline = match baseline_set {
        BaselineSet::First => content_baselines.first.is_some(),
        BaselineSet::Last => content_baselines.last.is_some(),
    };
    let collapsed = child_metrics.uses_collapsing_borders_model();
    if !has_baseline || source == ChildBaselineSource::Synthesized {
        if source == ChildBaselineSource::Own {
            return None;
        }
        // https://drafts.csswg.org/css-align-3/#synthesize-baseline
        // The alphabetic baseline is synthesized from the line-under edge of the border box.
        return Some(
            content_offset.y + child_metrics.content_block_size() + child_metrics.border_box_bottom(collapsed),
        );
    }
    let child_offset_from_margin_edge = content_offset.y - child_metrics.margin_box_top(collapsed);
    Some(
        child_offset_from_margin_edge
            + box_baseline_for_purpose(callbacks, node, child_metrics, baseline_set, content_baselines, purpose),
    )
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum FormattingContextType {
    Block,
    Flex,
    Grid,
    Table,
    Svg,
    Math,
    ReplacedWithChildren,
    InternalReplaced,
}

impl FormattingContextType {
    fn registers_abspos_children_against_its_final_block_size(self) -> bool {
        matches!(self, Self::Flex | Self::Grid)
    }

    fn lays_out_inside_its_root_as_definite_in_both_axes(self) -> bool {
        matches!(self, Self::Svg | Self::ReplacedWithChildren)
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BorderData {
    pub color: u32,
    pub line_style: u8,
    pub width: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ChildLayoutResult {
    pub automatic_content_block_size: CssPixels,
    pub automatic_line_clamp_max_lines: Option<usize>,
    pub baselines: DerivedBaselines,
    /// For a table wrapper, the inline size its table box gives it.
    pub table_wrapper_content_inline_size: Option<CssPixels>,
    pub table_box_in_wrapper_border_box_block_size: Option<CssPixels>,
    /// For a box with a preferred aspect ratio, the block size of its laid out content.
    pub content_block_size_for_aspect_ratio_minimum: Option<CssPixels>,
    /// For a flex or grid container with a preferred aspect ratio, the block size of the margin boxes of its in-flow
    /// children once they are placed, unless one of them is not an item.
    pub in_flow_children_margin_box_block_extent: Option<CssPixels>,
    pub depends_on_percentage_block_size: bool,
    /// For an absolutely positioned box, the used insets its run gave it.
    pub out_of_flow_root_used_insets: Option<run_state::UsedInsets>,
    /// For a box that establishes an orthogonal flow, its baselines along the horizontal axis of its parent's frame,
    /// from the left edge of its content box.
    pub orthogonal_flow_baselines: Option<DerivedBaselines>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) enum OwnBlockSizeObservation {
    #[default]
    Observed,
    Unobserved,
}

#[derive(Clone)]
pub(crate) struct RunRootOutcome {
    /// The input cells the run left its root, unless its parent owns them.
    pub(super) input_cells_left_by_run: Option<used_values::RootInputCells>,
    pub(super) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
}

impl RunRootOutcome {
    pub(super) fn parent_owns_root_input_cells(&self) -> bool {
        self.input_cells_left_by_run.is_none()
    }

    /// What the parent keeps of the root once its run returned `result`, given the input cells it handed the run.
    fn laid_out_child_box(
        &self,
        root_cells: used_values::RootInputCells,
        result: &ChildLayoutResult,
        content_block_size_adopted_after_run: Option<CssPixels>,
    ) -> run_state::LaidOutChildBox {
        let mut cells = self.input_cells_left_by_run.unwrap_or(root_cells);
        if let Some(block_size) = content_block_size_adopted_after_run {
            cells.content_block_size = clamp_to_max_dimension_value(block_size.max(CssPixels::default()));
        }
        run_state::LaidOutChildBox {
            cells,
            baselines: result.baselines,
            orthogonal_flow_baselines: result.orthogonal_flow_baselines,
            line_data: self.line_data.clone(),
        }
    }
}

#[derive(Clone)]
pub(crate) struct RunOutputs {
    pub(crate) result: ChildLayoutResult,
    pub(crate) root: Option<fragment_tree::UnplacedRootFragment>,
    pub(crate) root_outcome: RunRootOutcome,
    pub(crate) inputs_never_observed: fc_run_cache::RunInputsTheStoredRunNeverObserved,
    /// Boxed, as most runs find out nothing for the host.
    pub(crate) host_findings: Option<Box<super::run_state::HostFindings>>,
}

pub(crate) enum ChildLayoutOutcome {
    Skipped {
        depends_on_percentage_block_size: bool,
        /// For an absolutely positioned child, the used values of its inset properties.
        out_of_flow_root_used_insets: Option<run_state::UsedInsets>,
    },
    Created(ChildLayoutResult),
}

impl ChildLayoutOutcome {
    /// Whether the laid out child depends on the block size its percentages resolve against, which the box containing
    /// it hears of.
    pub(crate) fn child_depends_on_percentage_block_size(&self) -> bool {
        match self {
            Self::Skipped {
                depends_on_percentage_block_size,
                ..
            } => *depends_on_percentage_block_size,
            Self::Created(result) => result.depends_on_percentage_block_size,
        }
    }

    /// Notes for the box containing `child` whether `child`, laid out with this outcome, depends on the block size its
    /// percentages resolve against.
    pub(crate) fn report_percentage_block_size_dependency_of(&self, run: &FormattingContextRun, child: Node) {
        propagate_percentage_block_size_dependency_to_containing_block(
            run.state,
            &run.callbacks,
            child,
            self.child_depends_on_percentage_block_size(),
        );
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SizingAxis {
    Inline,
    Block,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum SizingProperty {
    Width,
    Height,
    MinWidth,
    MinHeight,
    MaxWidth,
    MaxHeight,
    FlexBasis,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FlexLayoutClampState {
    Unclamped,
    ClampedToMin,
    ClampedToMax,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum FlexLayoutGrowthState {
    Growing,
    Shrinking,
}

#[derive(Debug, PartialEq)]
pub(crate) struct FlexLayoutItem {
    /// The style node naming the item's DOM node; the devtools reader resolves it to a node id.
    pub(crate) style_node: Option<crate::css::style::tree::StyleNodeID>,
    pub(crate) rect: CssPixelRect,
    pub(crate) main_base_size: CssPixels,
    pub(crate) main_delta_size: CssPixels,
    pub(crate) main_min_size: CssPixels,
    pub(crate) main_max_size: CssPixels,
    pub(crate) cross_min_size: CssPixels,
    pub(crate) cross_max_size: CssPixels,
    pub(crate) clamp_state: FlexLayoutClampState,
    pub(crate) flex_basis: String,
    pub(crate) main_size_property: String,
    pub(crate) main_min_size_property: String,
    pub(crate) main_max_size_property: String,
    pub(crate) flex_grow: f64,
    pub(crate) flex_shrink: f64,
}

#[derive(PartialEq)]
pub(crate) struct FlexLayoutLine {
    pub(crate) growth_state: FlexLayoutGrowthState,
    pub(crate) cross_start: CssPixels,
    pub(crate) cross_size: CssPixels,
    pub(crate) items: Vec<FlexLayoutItem>,
}

#[derive(PartialEq)]
pub(crate) struct FlexLayoutData {
    pub(crate) align_content: u8,
    pub(crate) align_items: u8,
    pub(crate) flex_direction: u8,
    pub(crate) flex_wrap: u8,
    pub(crate) justify_content: u8,
    pub(crate) main_axis_direction: u8,
    pub(crate) cross_axis_direction: u8,
    pub(crate) lines: Vec<FlexLayoutLine>,
}

/// The calls the layout stage makes into the document, registered once per arena. Each callback
/// receives the registered `context`, the owning document, as its first argument. The fields are
/// private: the callbacks are reached only through the methods below, which take the main thread
/// token.
///
/// NB: The callbacks are typed as safe functions because the table only comes from the host through an export
///     whose safety contract keeps them callable, and the private fields let nothing else call them.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutHostCallbacks {
    context: *mut c_void,
    /// The commit messages a finished commit leaves for the document, in the order it produced
    /// them.
    deliver_commit_messages:
        extern "C" fn(*mut c_void, &crate::render_state::BegunRead, *const commit::FfiCommitMessage, usize),
    /// The scroll containers a finished layout tree build gave a style, each with whether it is a
    /// scroll snap container.
    take_built_scroll_containers:
        extern "C" fn(*mut c_void, &crate::render_state::BegunRead, *const FfiBuiltScrollContainer, usize),
}

/// A scroll container a layout tree build gave a style, with whether it was a scroll snap
/// container as the build finished.
#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiBuiltScrollContainer {
    pub slot: NodeSlotId,
    pub is_scroll_snap_container: bool,
}

impl FfiLayoutHostCallbacks {
    /// The layout host the document the entry was called for registered.
    pub(crate) fn of(main_thread: &crate::stage::MainThread) -> Self {
        main_thread
            .host_tables()
            .and_then(|host_tables| host_tables.layout_host.get())
            .expect("layout node arena has no layout host")
    }

    /// Delivers `messages` to the document, in `read`.
    pub(crate) fn deliver_commit_messages(
        &self,
        _: &crate::stage::MainThread,
        read: &crate::render_state::BegunRead,
        messages: &[commit::FfiCommitMessage],
    ) {
        if messages.is_empty() {
            return;
        }
        // The document registered the host with the arena and outlives this call.
        (self.deliver_commit_messages)(self.context, read, messages.as_ptr(), messages.len());
    }

    /// Hands the document the scroll containers a build gave a style, in `read`.
    pub(crate) fn take_built_scroll_containers(
        &self,
        _: &crate::stage::MainThread,
        read: &crate::render_state::BegunRead,
        built: &[FfiBuiltScrollContainer],
    ) {
        if built.is_empty() {
            return;
        }
        // The document registered the host with the arena and outlives this call.
        (self.take_built_scroll_containers)(self.context, read, built.as_ptr(), built.len());
    }
}

/// # Safety
///
/// `host` must be a live document host, on its document's thread. The callbacks must remain valid until they are
/// cleared or the host is destroyed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn document_host_set_layout_host_callbacks(
    host: &crate::render_state::DocumentHost,
    callbacks: FfiLayoutHostCallbacks,
) {
    host.host_tables().layout_host.set(Some(callbacks));
}

#[derive(Clone)]
pub(crate) struct FormattingContextRun<'pass> {
    pub(crate) state: &'pass RunState,
    pub(crate) box_: Node,
    pub(crate) callbacks: LayoutPass<'pass>,
    pub(crate) should_collect_devtools_layout_data: bool,
    pub(crate) treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    pub(crate) fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
    pub(crate) fragmentainer: Option<std::rc::Rc<fragmentation::FragmentainerContext>>,
}

impl<'pass> FormattingContextRun<'pass> {
    /// This run, laying out in `frame`.
    pub(crate) fn in_frame_of(&self, frame: geometry::Frame) -> Self {
        self.with_callbacks(self.callbacks.in_frame_of(frame.writing_mode()))
    }

    /// This run, laying out with the pass view `callbacks`.
    pub(crate) fn with_callbacks<'view>(&self, callbacks: LayoutPass<'view>) -> FormattingContextRun<'view>
    where
        'pass: 'view,
    {
        FormattingContextRun {
            state: self.state,
            box_: self.box_,
            callbacks,
            should_collect_devtools_layout_data: self.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: self
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            fragments: self.fragments.clone(),
            fragmentainer: self.fragmentainer.clone(),
        }
    }

    #[inline]
    pub(crate) fn sizing(&self) -> sizing_context::SizingContext<'pass, 'static> {
        sizing_context::SizingContext::new(self.state, self.callbacks)
    }

    /// Whether the block-axis percentage insets of the boxes whose containing block is the run's root, laid out in
    /// `root`, resolve against a definite block size.
    pub(crate) fn percentage_block_insets_of_root_children_have_definite_basis(
        &self,
        root: &impl BoxMetricCells,
    ) -> bool {
        percentage_block_insets_have_definite_basis_in(
            &self.callbacks,
            self.box_,
            || root.has_definite_block_size(),
            !self.treat_block_axis_percentage_insets_as_auto_beyond_root,
        )
    }

    /// Hands what the layout of `node` produced besides its geometry to the box's fragment, where this run builds
    /// fragments.
    pub(crate) fn update_fragment_payloads_of(
        &self,
        node: Node,
        update: impl FnOnce(&mut fragment_tree::FragmentPayloads),
    ) {
        if let Some(fragments) = self.fragments.as_deref() {
            fragments.update_fragment_payloads_of(node, update);
        }
    }

    pub(crate) fn outputs(
        &self,
        result: ChildLayoutResult,
        root: Option<fragment_tree::UnplacedRootFragment>,
        root_cells_left_by_run: used_values::RootInputCells,
        root_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
        inputs_never_observed: fc_run_cache::RunInputsTheStoredRunNeverObserved,
        parent_owns_root_input_cells: bool,
    ) -> RunOutputs {
        let root_outcome = RunRootOutcome {
            input_cells_left_by_run: (!parent_owns_root_input_cells).then_some(root_cells_left_by_run),
            line_data: root_line_data,
        };
        RunOutputs {
            result,
            root,
            root_outcome,
            inputs_never_observed,
            host_findings: self.state.take_host_findings(),
        }
    }
}

enum FormattingContextImplementation<'pass> {
    Block(Box<block_formatting_context::BlockFormattingContext<'pass>>),
    Flex(Box<flex_formatting_context::FlexFormattingContext<'pass>>),
    Grid(Box<grid_formatting_context::GridFormattingContext<'pass>>),
    Table(Box<table_formatting_context::TableFormattingContext<'pass>>),
    Svg(Box<svg_formatting_context::SvgFormattingContext<'pass>>),
    Math(Box<math_formatting_context::MathFormattingContext<'pass>>),
    ReplacedWithChildren,
    InternalReplaced,
}

pub(crate) fn formatting_context_type_created_by_node_data(
    data: &NodeData,
    style: Option<ComputedValuesView<'_>>,
    parent_is_flex_or_grid_container: bool,
    parent_writing_mode: impl FnOnce() -> Option<u8>,
) -> Option<FormattingContextType> {
    if data.kind.get() == crate::layout::node_data::NodeKind::SVGSVGBox {
        return Some(FormattingContextType::Svg);
    }
    let is_replaced_box = node_facts::kind_is_replaced_box(data.kind.get());
    let can_have_children = node_facts::node_can_have_children(data);
    if is_replaced_box && can_have_children {
        return Some(FormattingContextType::ReplacedWithChildren);
    }
    if is_replaced_box {
        return Some(FormattingContextType::InternalReplaced);
    }
    if !can_have_children {
        return None;
    }
    if node_facts::has_flag(data, NodeFlag::IsReplacedElement)
        && style.is_some_and(|style| {
            let display = style.display_before_box_type_transformation();
            display.is_table_inside() || display.is_internal_table() || display.is_table_caption()
        })
    {
        return Some(if node_facts::kind_is_block_container(data.kind.get()) {
            FormattingContextType::Block
        } else {
            FormattingContextType::InternalReplaced
        });
    }
    let display = style.map(|style| style.display());
    // NB: A flex fieldset lays out its legend and anonymous content box in a block formatting context.
    //     Flex layout applies only to the anonymous content box.
    if data.kind.get() == crate::layout::node_data::NodeKind::FieldSetBox
        && display.is_some_and(|display| display.is_flex_inside())
    {
        return Some(FormattingContextType::Block);
    }
    if display.is_some_and(|display| display.is_flex_inside()) {
        return Some(FormattingContextType::Flex);
    }
    if display.is_some_and(|display| display.is_table_inside()) {
        return Some(FormattingContextType::Table);
    }
    if display.is_some_and(|display| display.is_grid_inside()) {
        return Some(FormattingContextType::Grid);
    }
    // https://w3c.github.io/mathml-core/#new-display-math-value
    // MathML elements with a computed display value equal to block math or inline math control box generation and
    // layout according to their tag name.
    // NB: A box with inline children is a token element, or holds no MathML children, and lays them out in a block
    //     formatting context.
    if display.is_some_and(|display| display.is_math_inside()) {
        if node_facts::has_flag(data, NodeFlag::ChildrenAreInline) {
            return Some(FormattingContextType::Block);
        }
        return Some(FormattingContextType::Math);
    }
    node_facts::node_creates_block_formatting_context(
        data,
        style,
        parent_is_flex_or_grid_container,
        parent_writing_mode,
    )
    .then_some(FormattingContextType::Block)
}

pub(crate) fn formatting_context_type_created_by_box(facts: NodeFacts<'_>) -> Option<FormattingContextType> {
    let fc_type = formatting_context_type_created_by_node_data(
        facts.data(),
        facts.computed_values_view_if_styled(),
        facts.parent_is_flex_or_grid_container(),
        || facts.parent_writing_mode(),
    );
    if fc_type == Some(FormattingContextType::Math) && !facts.uses_math_layout() {
        return Some(FormattingContextType::Block);
    }
    fc_type
}

fn create_formatting_context_implementation<'pass>(
    run: &FormattingContextRun<'pass>,
    root_cells_in_own_frame: used_values::RootInputCells,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext<'pass>>,
    fc_type: FormattingContextType,
    previous_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
) -> FormattingContextImplementation<'pass> {
    match fc_type {
        FormattingContextType::Block => FormattingContextImplementation::Block(Box::new(
            block_formatting_context::BlockFormattingContext::new(run, root_cells_in_own_frame, previous_line_data),
        )),
        FormattingContextType::Flex => FormattingContextImplementation::Flex(Box::new(
            flex_formatting_context::FlexFormattingContext::new(run, root_cells_in_own_frame),
        )),
        FormattingContextType::Grid => FormattingContextImplementation::Grid(Box::new(
            grid_formatting_context::GridFormattingContext::new(run, root_cells_in_own_frame, parent_grid),
        )),
        FormattingContextType::Table => {
            FormattingContextImplementation::Table(Box::new(table_formatting_context::TableFormattingContext::new(
                run,
                root_cells_in_own_frame,
                table_formatting_context::AutoTableInlineSize::FillsAvailableInlineSize,
            )))
        }
        FormattingContextType::Svg => {
            FormattingContextImplementation::Svg(Box::new(svg_formatting_context::SvgFormattingContext::new(run)))
        }
        FormattingContextType::Math => FormattingContextImplementation::Math(Box::new(
            math_formatting_context::MathFormattingContext::new(run, root_cells_in_own_frame),
        )),
        FormattingContextType::ReplacedWithChildren => FormattingContextImplementation::ReplacedWithChildren,
        FormattingContextType::InternalReplaced => FormattingContextImplementation::InternalReplaced,
    }
}

fn register_table_abspos_descendants(run: &FormattingContextRun, parent: Node) {
    for child in run.callbacks.children(parent) {
        let facts = NodeFacts::new(&run.callbacks, child);
        if facts.is_box() {
            if facts.is_absolutely_positioned() {
                register_contained_abspos_child(
                    &run.callbacks,
                    run.fragments.as_deref(),
                    run.box_,
                    child,
                    abspos_inputs::StaticPositionRect {
                        rect: Default::default(),
                        inline_alignment: StaticPositionAlignment::Start,
                        block_alignment: StaticPositionAlignment::Start,
                        alignment_derives_from_own_computed_values: false,
                        is_known: true,
                    },
                    None,
                );
            }
            if formatting_context_type_created_by_box(facts).is_none() {
                register_table_abspos_descendants(run, child);
            }
        } else {
            register_table_abspos_descendants(run, child);
        }
    }
}

pub(crate) fn independent_root_automatic_block_size(
    sizing: &sizing_context::SizingContext<'_, '_>,
    sized: sizing_context::SizedBox<'_>,
    available_inner_space: AvailableSpace,
    constraints: ContainingBlockConstraints,
    automatic_content_block_size_of_completed_run: Option<CssPixels>,
) -> CssPixels {
    let node = sized.node;
    if let Some(automatic_content_block_size_reported_by_completed_run) = automatic_content_block_size_of_completed_run
    {
        return automatic_content_block_size_reported_by_completed_run;
    }
    let callbacks = sizing.callbacks();
    if NodeFacts::new(callbacks, node).creates_block_formatting_context() {
        return CssPixels::default();
    }
    let style = StyleValues::for_node(callbacks, node);
    // https://w3c.github.io/mathml-core/#layout-algorithms
    // After calculating the line-ascent and line-descent of a math content box, its block size is set to their sum.
    if style.display().is_math_inside() {
        return CssPixels::default();
    }
    if style.display().is_flex_inside() || style.display().is_grid_inside() || style.display().is_table_inside() {
        // The automatic block size of a flex, grid, or table container is its
        // max-content size.
        // https://drafts.csswg.org/css-flexbox-1/#algo-main-container
        // https://www.w3.org/TR/css-grid-2/#intrinsic-sizes
        return sizing.calculate_max_content_block_size(
            sized,
            available_inner_space.inline_size.to_px_or_zero(),
            constraints,
        );
    }
    debug_assert!(false, "independent formatting context root of unexpected kind");
    CssPixels::default()
}

pub(crate) fn flex_self_block_size_resolution_space(
    sizing: &sizing_context::SizingContext,
    sized: sizing_context::SizedBox<'_>,
    resolution_space: AvailableSpace,
    constraints: ContainingBlockConstraints,
) -> Option<AvailableSpace> {
    let node = sized.node;
    let style = sizing.style(node);
    if !style.display().is_flex_inside() || !crate::layout::sizing_context::is_plain_auto(style.min_height()) {
        return None;
    }
    if sizing.facts(node).document_in_quirks_mode() {
        return None;
    }
    if !sizing.should_treat_block_size_as_auto(sized, resolution_space, constraints)
        || sizing.box_is_sized_as_replaced_element(sized, resolution_space, constraints)
    {
        return None;
    }
    Some(resolution_space)
}

// Flex containers with an automatic block size are treated as max-content, so resolve it early.
fn eagerly_resolve_atomic_flex_root_auto_block_size(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
    inline_definite_space: AvailableSpace,
) {
    let node = run.box_;
    let sizing = run.sizing();
    if !StyleValues::for_node(&run.callbacks, node).display().is_flex_inside()
        || sizing.box_is_sized_as_replaced_element(
            sized_root.sized_box(),
            input.available_space,
            input.containing_block_constraints,
        )
    {
        return;
    }
    sizing.resolve_used_block_size_if_treated_as_auto(
        sized_root,
        inline_definite_space,
        input.containing_block_constraints,
        None,
        || {
            independent_root_automatic_block_size(
                &sizing,
                sized_root.sized_box(),
                sized_root
                    .cells
                    .available_inner_space_or_constraints_from(inline_definite_space),
                input.containing_block_constraints,
                None,
            )
        },
    );
}

struct RootSizingOutcome {
    body_input: LayoutInput,
    atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: Option<CssPixels>,
}

fn apply_root_sizing_directives(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
    fc_type: FormattingContextType,
    root_used_insets: &Cell<run_state::UsedInsets>,
) -> RootSizingOutcome {
    let mut atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above = None;
    let body_input = match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => {
            dimension_block_level_root(run, sized_root, input, fc_type)
        }
        ParticipationInParentFormattingContext::Float { .. } => {
            resolve_float_root_block_size_ahead_of_its_contents(run, sized_root, input);
            body_input_with_inner_available_space(sized_root.cells, input)
        }
        ParticipationInParentFormattingContext::AtomicInline => {
            let run_cache_may_reuse_this_run_at_other_available_inline_sizes =
                fc_type == FormattingContextType::Block && run.fragments.is_some();
            atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above = run.sizing().dimension_atomic_root(
                sized_root,
                input.available_space,
                input.containing_block_constraints,
                run_cache_may_reuse_this_run_at_other_available_inline_sizes,
            );
            let mut body_input = body_input_with_inner_available_space(sized_root.cells, input);
            let inline_definite_space = AvailableSpace {
                inline_size: AvailableSize::definite(sized_root.cells.content_inline_size()),
                block_size: AvailableSize::Indefinite,
            };
            let flex_self_resolution_space = flex_self_block_size_resolution_space(
                &run.sizing(),
                sized_root.sized_box(),
                inline_definite_space,
                input.containing_block_constraints,
            );
            if flex_self_resolution_space.is_none() {
                eagerly_resolve_atomic_flex_root_auto_block_size(run, sized_root, input, inline_definite_space);
            }
            body_input.sizing.flex_self_block_size_resolution_space = flex_self_resolution_space;
            body_input
        }
        ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
            abspos_engine::AbsposEngine::for_run(run).dimension_out_of_flow_root(
                abspos_engine::OutOfFlowBox::new(
                    sized_root,
                    root_used_insets,
                    run.state.anchor_resolved_values_of(run.box_),
                ),
                abspos_inputs,
            );
            body_input_with_inner_available_space(sized_root.cells, input)
        }
        ParticipationInParentFormattingContext::Item => {
            if cfg!(debug_assertions) && run.fragments.is_some() {
                debug_assert!(
                    sized_root.cells.has_definite_inline_size(),
                    "container-internal run root must arrive with a container-assigned inline size"
                );
            }
            *input
        }
        ParticipationInParentFormattingContext::Root => {
            let directives = input.sizing;
            if directives.forced_content_inline_size.is_some() || directives.forced_content_block_size.is_some() {
                let used = sized_root.cells;
                if let Some(inline_size) = directives.forced_content_inline_size {
                    used.set_content_inline_size(inline_size);
                }
                if let Some(block_size) = directives.forced_content_block_size {
                    used.set_content_block_size(block_size);
                    used.update_cells(|cells| cells.has_definite_block_size = true);
                }
            }
            *input
        }
    };
    RootSizingOutcome {
        body_input,
        atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
    }
}

fn dimension_block_level_root(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
    fc_type: FormattingContextType,
) -> LayoutInput {
    let node = run.box_;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let sizing = run.sizing();
    let style = StyleValues::for_node(&run.callbacks, node);
    let flex_self_resolution_space = size_block_level_root_ahead_of_its_contents(run, sized_root, input);
    let mut body_input = body_input_with_inner_available_space(sized_root.cells, input);
    body_input.sizing.flex_self_block_size_resolution_space = flex_self_resolution_space;
    if let Some(block_size) = input.sizing.forced_content_block_size {
        let used = sized_root.cells;
        used.set_content_block_size(block_size);
        used.update_cells(|cells| cells.has_definite_block_size = true);
        body_input.available_space.block_size = AvailableSize::definite(block_size);
        return body_input;
    }
    let mut measured_content_block_size = None;
    if sizing.should_treat_block_size_as_auto(sized_root.sized_box(), available_space, constraints)
        && !crate::layout::sizing_context::is_plain_auto(style.min_height())
        && lays_out_content_against_min_block_size(NodeFacts::new(&run.callbacks, node), fc_type)
    {
        let content_block_size = sizing.measure_automatic_content_block_size(
            sized_root.sized_box(),
            body_input.available_space,
            constraints,
        );
        measured_content_block_size = Some(content_block_size);
        let min_block_size =
            sizing.calculate_inner_block_size(sized_root.sized_box(), available_space, style.min_height(), constraints);
        if content_block_size < min_block_size {
            body_input.available_space.block_size = AvailableSize::definite(min_block_size);
        }
    }
    sizing.make_form_control_content_box_definite(
        sized_root,
        available_space,
        constraints,
        measured_content_block_size,
    );
    body_input
}

/// Gives the root of a block-level run the inline size its block formatting context resolved for it, and resolves its
/// block size where that follows without laying out its contents. Answers the space a flex root resolves its own
/// automatic block size in, if it does.
fn size_block_level_root_ahead_of_its_contents(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
) -> Option<AvailableSpace> {
    let node = run.box_;
    let root = sized_root.cells;
    let constraints = input.containing_block_constraints;
    let sizing = run.sizing();
    let facts = NodeFacts::new(&run.callbacks, node);
    if let Some(content_inline_size) = input.sizing.block_parent_resolved_content_inline_size {
        root.set_content_inline_size(content_inline_size);
    }
    let flex_self_resolution_space =
        flex_self_block_size_resolution_space(&sizing, sized_root.sized_box(), input.available_space, constraints);
    let resolution_space = sizing.available_space_for_block_size_resolution(node, input.available_space, constraints);
    sizing.resolve_used_block_size_if_not_treated_as_auto(sized_root, resolution_space, constraints);
    let block_size_is_definite_from_aspect_ratio = root.has_definite_inline_size()
        && facts.has_preferred_aspect_ratio()
        && sizing.box_is_sized_as_replaced_element(sized_root.sized_box(), resolution_space, constraints);
    if facts.has_auto_content_box_size()
        || block_size_is_definite_from_aspect_ratio
        || (StyleValues::for_node(&run.callbacks, node).display().is_flex_inside()
            && flex_self_resolution_space.is_none())
    {
        sizing.resolve_used_block_size_if_treated_as_auto(sized_root, resolution_space, constraints, None, || {
            block_formatting_context::automatic_block_size_of_independent_root_ahead_of_its_contents(
                &sizing,
                sized_root.sized_box(),
                root.available_inner_space_or_constraints_from(resolution_space),
                constraints,
            )
        });
    }
    flex_self_resolution_space
}

/// Resolves the block size of the root of a float's run where that follows without laying out its contents, in the
/// inline size its block formatting context gave it.
fn resolve_float_root_block_size_ahead_of_its_contents(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
) {
    let node = run.box_;
    let root = sized_root.cells;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let sizing = run.sizing();
    let facts = NodeFacts::new(&run.callbacks, node);
    sizing.resolve_used_block_size_if_not_treated_as_auto(sized_root, available_space, constraints);
    let block_size_is_definite_from_aspect_ratio = root.has_definite_inline_size()
        && facts.has_preferred_aspect_ratio()
        && sizing.box_is_sized_as_replaced_element(sized_root.sized_box(), available_space, constraints);
    if facts.has_auto_content_box_size()
        || block_size_is_definite_from_aspect_ratio
        || (StyleValues::for_node(&run.callbacks, node).display().is_flex_inside()
            && input.sizing.flex_self_block_size_resolution_space.is_none())
    {
        sizing.resolve_used_block_size_if_treated_as_auto(sized_root, available_space, constraints, None, || {
            block_formatting_context::automatic_block_size_of_independent_root_ahead_of_its_contents(
                &sizing,
                sized_root.sized_box(),
                root.available_inner_space_or_constraints_from(available_space),
                constraints,
            )
        });
    }
}

/// Whether a block-level root with an automatic block size lays out its content against its min-height where its
/// content is shorter, which then has to be measured first. A block container's content never reads that size: Its
/// min-height only clamps its block size once its content is laid out, so measuring first would just lay out the
/// whole subtree an extra time — on every layout. Form controls center their content within the clamped size, though.
fn lays_out_content_against_min_block_size(facts: NodeFacts<'_>, fc_type: FormattingContextType) -> bool {
    fc_type != FormattingContextType::Block
        || facts.uses_button_layout()
        || facts.lays_out_text_input_content_as_ordinary_box()
}

/// The content block size of a laid out box, for growing a block size transferred through its aspect ratio. Chrome
/// derives it from the layout at the transferred size, so percentages inside resolve against that size.
pub(crate) fn content_block_size_for_aspect_ratio_minimum(
    callbacks: &LayoutPass<'_>,
    node: Node,
    result: &ChildLayoutResult,
) -> Option<CssPixels> {
    let automatic_content_block_size = result.automatic_content_block_size;
    let facts = NodeFacts::new(callbacks, node);
    if facts.node_has_size_containment() {
        // NB: The automatic content block size of a size containment box already ignores its content.
        return Some(automatic_content_block_size);
    }
    if facts.uses_button_layout() {
        // NB: The anonymous wrapper fills the button, so its content box has to be measured.
        return None;
    }
    match formatting_context_type_created_by_box(facts) {
        Some(FormattingContextType::Block) => Some(automatic_content_block_size),
        // NB: These report their own block size, so take the extent of their laid out children instead.
        Some(FormattingContextType::Flex | FormattingContextType::Grid) => {
            result.in_flow_children_margin_box_block_extent
        }
        _ => None,
    }
}

fn finalize_block_level_root(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
    body_result: &ChildLayoutResult,
) {
    let node = run.box_;
    if let Some(inline_size) = body_result.table_wrapper_content_inline_size {
        sized_root.cells.set_content_inline_size(inline_size);
    }
    let style = StyleValues::for_node(&run.callbacks, node);
    if !style.display().is_table_inside() && input.sizing.forced_content_block_size.is_none() {
        let sizing = run.sizing();
        let resolution_space = sizing.available_space_for_block_size_resolution(
            node,
            input.available_space,
            input.containing_block_constraints,
        );
        sizing.resolve_used_block_size_if_treated_as_auto(
            sized_root,
            resolution_space,
            input.containing_block_constraints,
            Some(body_result.automatic_content_block_size),
            || unreachable!("a completed block-level run always supplies its automatic content block size"),
        );
    }
}

fn finalize_float_root(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
    body_result: &ChildLayoutResult,
) {
    if let Some(inline_size) = body_result.table_wrapper_content_inline_size {
        sized_root.cells.set_content_inline_size(inline_size);
    }
    run.sizing().resolve_used_block_size_if_treated_as_auto(
        sized_root,
        input.available_space,
        input.containing_block_constraints,
        Some(body_result.automatic_content_block_size),
        || unreachable!("a completed float run always supplies its automatic content block size"),
    );
}

/// Sizes `child` in `cells` without a run of its own. Returns the used insets of an absolutely positioned child.
fn size_skipped_independent_root(
    run: &FormattingContextRun,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    child: Node,
    cells: &Cell<used_values::RootInputCells>,
    input: &LayoutInput,
) -> Option<run_state::UsedInsets> {
    match input.participation {
        ParticipationInParentFormattingContext::Float { .. } => {
            let parent = parent_block.expect("a float requires an enclosing block formatting context");
            let sized_float = block_formatting_context::ChildSizedHere::laid_out_in(child, cells);
            parent.resolve_float_root_block_size_in_parent(sized_float, input);
            parent.resolve_used_block_size_if_treated_as_auto(
                sized_float,
                input.available_space,
                input.containing_block_constraints,
                None,
                None,
            );
            None
        }
        ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
            let engine = abspos_engine::AbsposEngine::for_run(run);
            let anchor_resolved_values = (!abspos_inputs.resolved_anchor_sizes.is_empty()).then(|| {
                style_values::AnchorResolvedValues::new(&abspos_inputs.resolved_anchor_sizes, run.callbacks.frame)
            });
            let used_insets = Cell::new(run_state::UsedInsets::default());
            let sized_child = abspos_engine::OutOfFlowBox::new(
                sizing_context::SizedCells::new(child, cells, run.callbacks.frame),
                &used_insets,
                anchor_resolved_values.as_ref(),
            );
            engine.dimension_out_of_flow_root(sized_child, abspos_inputs);
            engine.finalize_out_of_flow_root_after_inside_layout(sized_child, abspos_inputs, None);
            Some(used_insets.get())
        }
        ParticipationInParentFormattingContext::BlockLevel => {
            unreachable!("a block formatting context lays out only boxes, and every block-level box can have children")
        }
        ParticipationInParentFormattingContext::AtomicInline
        | ParticipationInParentFormattingContext::Item
        | ParticipationInParentFormattingContext::Root => None,
    }
}

fn body_input_with_inner_available_space(root: &impl BoxMetricCells, input: &LayoutInput) -> LayoutInput {
    let inner_available_space = root.available_inner_space_or_constraints_from(input.available_space);
    let mut body_input = *input;
    body_input.available_space = inner_available_space;
    body_input
}

/// What a child's run gives its parent: its result, the box as the parent keeps it, and the fragments its placement
/// takes where the parent builds fragments.
#[must_use]
pub(crate) struct ChildRunOutcome {
    pub(crate) result: ChildLayoutResult,
    pub(crate) laid_out_box: run_state::LaidOutChildBox,
    pub(crate) fragment_arrival: Option<fragment_tree::ChildFragmentArrival>,
}

impl ChildRunOutcome {
    /// What placing the child takes of the box its run handed back.
    pub(crate) fn placed_child(&self) -> PlacedChild {
        PlacedChild::of_laid_out_box(&self.laid_out_box)
    }
}

#[expect(clippy::too_many_arguments)]
/// Runs the formatting context of `box_` with the input cells `root_cells` its parent sized it in.
pub(super) fn run_formatting_context(
    parent_fragments: Option<&fragment_tree::RunFragmentBuilder>,
    root_cells: used_values::RootInputCells,
    box_: Node,
    root_containing_block: Node,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    fc_type: FormattingContextType,
    should_collect_devtools_layout_data: bool,
    callbacks: LayoutPass<'_>,
    input: LayoutInput,
    table_inline_layout: Option<table_formatting_context::TableInlineLayout>,
) -> ChildRunOutcome {
    let builds_fragments = run_builds_fragments(&input, parent_fragments.is_some());
    assert!(
        parent_fragments.is_none() || builds_fragments,
        "the run of {box_:?} builds no fragments for its parent to commit"
    );
    if callbacks.reads_no_memoized_layout() {
        let outputs = execute_formatting_context_run(
            builds_fragments,
            root_cells,
            box_,
            root_containing_block,
            parent_grid,
            fc_type,
            should_collect_devtools_layout_data,
            callbacks,
            input,
            None,
            table_inline_layout,
            None,
        );
        return child_run_outcome_from_outputs(&callbacks, parent_fragments, box_, &input, root_cells, outputs, false);
    }
    let cache_key = fc_run_cache::FcRunCacheKey::new(fc_type, input, root_cells);
    let cache_attempt = match fc_run_cache::FcRunCacheAttempt::probe(
        box_,
        parent_grid.is_some(),
        builds_fragments,
        should_collect_devtools_layout_data,
        false,
        &callbacks,
        &cache_key,
    ) {
        Ok(attempt) => attempt,
        Err(hit) => {
            if let Some(outcome) = reuse_committed_subtree_as_it_is(
                parent_fragments,
                box_,
                &input,
                root_cells,
                &callbacks,
                &cache_key,
                &hit,
            ) {
                return outcome;
            }
            let reuses_committed_subtree = hit.reuses_committed_subtree;
            let action = if !builds_fragments {
                "READ RESULT"
            } else if reuses_committed_subtree {
                "REUSE SUBTREE"
            } else {
                "REPLAY FRAGMENTS"
            };
            let _trace = callbacks.layout_trace().run(box_, fc_type, || action);
            let outputs = if builds_fragments {
                hit.entry.outputs_to_replay(reuses_committed_subtree)
            } else {
                hit.entry.outputs_without_fragments()
            };
            return child_run_outcome_from_outputs(
                &callbacks,
                parent_fragments,
                box_,
                &input,
                root_cells,
                outputs,
                reuses_committed_subtree,
            );
        }
    };
    let _trace = callbacks
        .layout_trace()
        .run(box_, fc_type, || cache_attempt.trace_action());
    let previous_line_data = cache_attempt.previous_line_data();
    let discarded_outputs = run_repeatability::lay_out_discarded_run(&callbacks, |callbacks| {
        execute_formatting_context_run(
            builds_fragments,
            root_cells,
            box_,
            root_containing_block,
            parent_grid,
            fc_type,
            should_collect_devtools_layout_data,
            callbacks,
            input,
            None,
            table_inline_layout.clone(),
            None,
        )
    });
    let outputs = execute_formatting_context_run(
        builds_fragments,
        root_cells,
        box_,
        root_containing_block,
        parent_grid,
        fc_type,
        should_collect_devtools_layout_data,
        callbacks,
        input,
        previous_line_data,
        table_inline_layout,
        None,
    );
    if let Some(discarded_outputs) = &discarded_outputs {
        run_repeatability::verify_repeated_run(&callbacks, box_, discarded_outputs, &outputs);
    }
    if cache_attempt.stores_outputs() {
        run_repeatability::verify_run_ignores_its_unobserved_own_block_size_and_block_margins(
            &callbacks,
            box_,
            root_cells,
            input,
            &outputs,
            |callbacks, other_root_cells, other_input| {
                execute_formatting_context_run(
                    builds_fragments,
                    other_root_cells,
                    box_,
                    root_containing_block,
                    parent_grid,
                    fc_type,
                    should_collect_devtools_layout_data,
                    callbacks,
                    other_input,
                    None,
                    None,
                    None,
                )
            },
        );
    }
    cache_attempt.conclude(&callbacks, box_, cache_key, &input, &outputs);
    child_run_outcome_from_outputs(&callbacks, parent_fragments, box_, &input, root_cells, outputs, false)
}

/// Takes a committing run of `box_` from the cache hit `hit` with nothing replayed: the box gets what the stored run
/// left, and the run's placement attaches the box's committed fragment as it is. That is exact where the committed
/// subtree is the one the stored run produced, the box is plain, and nothing from the stored run reaches past its root.
fn reuse_committed_subtree_as_it_is(
    parent_fragments: Option<&fragment_tree::RunFragmentBuilder>,
    box_: Node,
    input: &LayoutInput,
    root_cells: used_values::RootInputCells,
    callbacks: &LayoutPass<'_>,
    cache_key: &fc_run_cache::FcRunCacheKey,
    hit: &fc_run_cache::FcRunCacheHit,
) -> Option<ChildRunOutcome> {
    parent_fragments?;
    let entry = &*hit.entry;
    if !hit.reuses_committed_subtree
        || !cache_key.has_no_table_cell_intrinsic_block_padding()
        || entry
            .outputs
            .root
            .as_ref()
            .is_some_and(|root| !root.propagates_nothing())
        || NodeFacts::new(callbacks, box_).has_anchor_names()
    {
        return None;
    }
    let fragment = callbacks
        .arena()
        .with_current_committed_fragment(box_, |fragment| fragment.is_plain_box().then(|| fragment.clone()))
        .flatten()?;
    let content_block_size_adopted_after_run =
        content_block_size_adopted_after_run(input, entry.outputs.result.automatic_content_block_size);
    let laid_out_box = entry.outputs.root_outcome.laid_out_child_box(
        root_cells,
        &entry.outputs.result,
        content_block_size_adopted_after_run,
    );
    let geometry = fragment_tree::FragmentGeometry {
        cells: laid_out_box.cells,
        line_data: laid_out_box.line_data.clone(),
    };
    if !geometry.shows_box_of_fragment(&fragment) {
        return None;
    }
    let _trace = callbacks
        .layout_trace()
        .run(box_, cache_key.fc_type(), || "REUSE SUBTREE");
    callbacks.receive_host_findings(entry.outputs.host_findings.clone());
    Some(ChildRunOutcome {
        result: entry.outputs.result,
        laid_out_box,
        fragment_arrival: Some(fragment_tree::ChildFragmentArrival::CommittedFragment {
            fragment,
            reuses_subtree: true,
        }),
    })
}

pub(crate) fn explicit_intrinsic_inner_inline_size(facts: &NodeFacts<'_>) -> CssPixels {
    let style = facts.style();
    if style.contain_intrinsic_width_has_length() {
        CssPixels::nearest_value_for(style.contain_intrinsic_width_px())
    } else {
        CssPixels::default()
    }
}

#[expect(clippy::too_many_arguments)]
fn execute_formatting_context_run(
    builds_fragments: bool,
    root_cells: used_values::RootInputCells,
    box_: Node,
    root_containing_block: Node,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    fc_type: FormattingContextType,
    should_collect_devtools_layout_data: bool,
    callbacks: LayoutPass<'_>,
    input: LayoutInput,
    previous_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    table_inline_layout: Option<table_formatting_context::TableInlineLayout>,
    fragmentainer: Option<std::rc::Rc<fragmentation::FragmentainerContext>>,
) -> RunOutputs {
    assert!(!box_.is_invalid());
    let parent_owns_root_input_cells = parent_owns_root_input_cells(&callbacks, box_, &input);
    let checks_root_input_cells_its_parent_owns =
        parent_owns_root_input_cells && (cfg!(debug_assertions) || run_repeatability::verification_is_enabled());
    // NB: The anchor-size() resolutions apply only in the parent's frame, where the root's own box is sized.
    let root_anchor_resolved_values = match input.participation {
        ParticipationInParentFormattingContext::AbsolutelyPositioned(inputs)
            if !inputs.resolved_anchor_sizes.is_empty() =>
        {
            Some(Box::new(style_values::AnchorResolvedValues::new(
                &inputs.resolved_anchor_sizes,
                callbacks.frame,
            )))
        }
        _ => None,
    };
    let (outputs, root_cells_left_by_run) = RunState::with_root(
        callbacks,
        box_,
        root_containing_block,
        root_anchor_resolved_values,
        |state, callbacks| {
            lay_out_run_root_and_contents(
                state,
                callbacks,
                builds_fragments,
                root_cells,
                box_,
                root_containing_block,
                parent_grid,
                fc_type,
                should_collect_devtools_layout_data,
                input,
                previous_line_data,
                table_inline_layout,
                fragmentainer,
                parent_owns_root_input_cells,
            )
        },
    );
    if input.sizing.root_keeps_its_committed_geometry {
        assert_eq!(
            sizes_and_box_sides(root_cells_left_by_run),
            sizes_and_box_sides(root_cells),
            "the run of {box_:?} changed the committed geometry of its root"
        );
    }
    if checks_root_input_cells_its_parent_owns {
        let mut root_input_cells_the_run_must_leave = root_cells;
        if fc_type.lays_out_inside_its_root_as_definite_in_both_axes() {
            root_input_cells_the_run_must_leave.has_definite_inline_size = true;
            root_input_cells_the_run_must_leave.has_definite_block_size = true;
        }
        if run_reads_content_block_size_adopted_after_it(fc_type, &input)
            && content_block_size_adopted_after_run(&input, outputs.result.automatic_content_block_size).is_some()
        {
            root_input_cells_the_run_must_leave.content_block_size = root_cells_left_by_run.content_block_size;
        }
        assert_eq!(
            root_cells_left_by_run, root_input_cells_the_run_must_leave,
            "the run of {box_:?} changed root cells its parent owns"
        );
    }
    outputs
}

/// The sizes and box sides of `cells`, with every flag cleared.
fn sizes_and_box_sides(cells: used_values::RootInputCells) -> used_values::RootInputCells {
    used_values::RootInputCells {
        has_definite_inline_size: false,
        has_definite_block_size: false,
        has_definite_block_size_only_for_button_content_alignment: false,
        uses_collapsing_borders_model: false,
        is_collapsed_borders_table_box: false,
        ..cells
    }
}

#[expect(clippy::too_many_arguments)]
fn lay_out_run_root_and_contents<'pass>(
    state: &'pass RunState,
    callbacks: LayoutPass<'pass>,
    builds_fragments: bool,
    root_cells: used_values::RootInputCells,
    box_: Node,
    root_containing_block: Node,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext<'pass>>,
    fc_type: FormattingContextType,
    should_collect_devtools_layout_data: bool,
    input: LayoutInput,
    previous_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    table_inline_layout: Option<table_formatting_context::TableInlineLayout>,
    fragmentainer: Option<std::rc::Rc<fragmentation::FragmentainerContext>>,
    parent_owns_root_input_cells: bool,
) -> (RunOutputs, used_values::RootInputCells) {
    let run_is_handed_table_inline_layout_outside_its_key = table_inline_layout.is_some();
    let run = FormattingContextRun {
        state,
        box_,
        callbacks,
        should_collect_devtools_layout_data,
        treat_block_axis_percentage_insets_as_auto_beyond_root: input
            .sizing
            .treat_block_axis_percentage_insets_as_auto_beyond_root,
        fragments: builds_fragments.then(|| {
            std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new(
                box_,
                (!root_containing_block.is_invalid()).then_some(root_containing_block),
            ))
        }),
        fragmentainer,
    };
    let run = &run;
    // NB: The root's own box is sized and placed in the frame of its parent's formatting context, while
    //     its contents are laid out in the frame of the writing mode the root establishes.
    let parent_frame = callbacks.frame;
    let own_frame = frame_established_by(&callbacks, box_);
    let changes_frame = own_frame != parent_frame;
    let body_run_in_own_frame;
    let body_run = if changes_frame {
        body_run_in_own_frame = run.in_frame_of(own_frame);
        &body_run_in_own_frame
    } else {
        run
    };
    // NB: A table wrapper's run hands the inline layout of its table box to the run of that box.
    let (mut inline_layout_of_root_table_box, inline_layout_of_child_table_box) = match table_inline_layout {
        Some(table_inline_layout) if table_inline_layout.table_box() == box_ => (Some(table_inline_layout), None),
        table_inline_layout => (None, table_inline_layout),
    };
    let root = Cell::new(root_cells);
    let sized_root = sizing_context::SizedCells::new(box_, &root, parent_frame);
    let root_used_insets = Cell::new(run_state::UsedInsets::default());
    let RootSizingOutcome {
        body_input,
        atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
    } = apply_root_sizing_directives(run, sized_root, &input, fc_type, &root_used_insets);
    debug_assert!(
        !body_input.available_space.inline_size.is_intrinsic_sizing_constraint(),
        "no run lays out its contents under an intrinsic inline size"
    );

    let own_root = Cell::new(root.get().in_frame(parent_frame, own_frame));
    let body_input = if changes_frame {
        body_input_in_own_frame(
            run,
            body_run,
            sizing_context::SizedCells::new(box_, &own_root, own_frame),
            &input,
            body_input,
            parent_frame,
            own_frame,
        )
    } else {
        body_input
    };

    let mut implementation = None;
    let mut root_lines = used_values::LineDataState::Empty;
    let (mut result, own_root_after_body) = if !changes_frame
        && run.fragments.is_some()
        && matches!(
            input.participation,
            ParticipationInParentFormattingContext::AtomicInline
        )
        && fc_type == FormattingContextType::Block
        && callbacks.first_child(box_).is_invalid()
    {
        // An empty atomic block context has no body output. Root sizing and finalization still
        // run through the shared paths around this branch.
        (ChildLayoutResult::default(), own_root.get())
    } else {
        let mut context_implementation = create_formatting_context_implementation(
            body_run,
            own_root.get(),
            parent_grid,
            fc_type,
            previous_line_data,
        );
        if let (Some(layout), FormattingContextImplementation::Block(context)) =
            (inline_layout_of_child_table_box, &context_implementation)
        {
            context.keep_table_inline_layout_of(layout.table_box(), layout);
        }
        let result_and_own_root_after_body = match &mut context_implementation {
            FormattingContextImplementation::Block(context) => {
                context.run(body_run, body_input);
                let mut result = ChildLayoutResult {
                    automatic_content_block_size: context.automatic_content_block_size(),
                    automatic_line_clamp_max_lines: context.automatic_line_clamp_max_lines(),
                    baselines: context.derived_baselines_of_root_box(),
                    table_wrapper_content_inline_size: context.table_wrapper_content_inline_size(),
                    table_box_in_wrapper_border_box_block_size: context.table_box_in_wrapper_border_box_block_size(),
                    ..ChildLayoutResult::default()
                };
                let mut root_after_body = own_root.get();
                // https://w3c.github.io/mathml-core/#layout-of-operators
                // NB: An operator lays out its text in a block formatting context, unless it is drawn with a
                //     stretched or large glyph.
                if NodeFacts::new(&body_run.callbacks, box_).display().is_math_inside() {
                    math_formatting_context::layout_operator(body_run, &input, &mut result, &mut root_after_body);
                }
                root_lines = context.take_root_lines();
                (result, root_after_body)
            }
            FormattingContextImplementation::Flex(context) => {
                context.run(body_run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                let result = ChildLayoutResult {
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    in_flow_children_margin_box_block_extent: context.in_flow_items_margin_box_block_extent(),
                    ..ChildLayoutResult::default()
                };
                (result, context.container_cells())
            }
            FormattingContextImplementation::Grid(context) => {
                context.run(body_run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                let result = ChildLayoutResult {
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    in_flow_children_margin_box_block_extent: context.in_flow_items_margin_box_block_extent(),
                    ..ChildLayoutResult::default()
                };
                (result, own_root.get())
            }
            FormattingContextImplementation::Table(context) => {
                context.run(body_run, body_input, inline_layout_of_root_table_box.take());
                let baselines = context.derived_baselines_of_root_box();
                let result = ChildLayoutResult {
                    automatic_content_block_size: context.automatic_content_block_size,
                    baselines,
                    ..ChildLayoutResult::default()
                };
                (result, context.table_box_cells())
            }
            FormattingContextImplementation::Svg(context) => (
                ChildLayoutResult::default(),
                context.run(body_run, body_input, own_root.get()),
            ),
            FormattingContextImplementation::Math(context) => {
                context.run(body_run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                let result = ChildLayoutResult {
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    ..ChildLayoutResult::default()
                };
                (result, context.root_cells())
            }
            FormattingContextImplementation::ReplacedWithChildren => {
                let mut root_after_body = own_root.get();
                let result = replaced_with_children_formatting_context::layout_replaced_with_children(
                    body_run,
                    body_input,
                    &mut root_after_body,
                );
                (result, root_after_body)
            }
            FormattingContextImplementation::InternalReplaced => (ChildLayoutResult::default(), own_root.get()),
        };
        implementation = Some(context_implementation);
        result_and_own_root_after_body
    };
    // https://drafts.csswg.org/css-sizing-4/#intrinsic-size-override
    // If an element has an explicit intrinsic inner size in an axis, then after laying out the element as normal for
    // size containment, the size of the contents in that axis are instead treated as being the explicit intrinsic inner
    // size instead of what was calculated in layout, and layout is performed again if necessary.
    //
    // Every formatting context reports its content sizes through ChildLayoutResult, so overriding here covers them all
    // rather than one context's root-height path.
    let containment_facts = NodeFacts::new(&body_run.callbacks, run.box_);
    let lays_out_own_content = run.fragmentainer.is_none();
    if lays_out_own_content && containment_facts.node_has_size_containment() {
        let style = containment_facts.style();
        // https://drafts.csswg.org/css-contain-2/#containment-size
        // Giving an element size containment makes its principal box a size containment box and has the following
        // effects:
        // 1. The intrinsic sizes of the size containment box are determined as if the element had no content, following
        //    the same logic as when sizing as if empty.
        result.automatic_content_block_size = if style.contain_intrinsic_height_has_length() {
            CssPixels::nearest_value_for(style.contain_intrinsic_height_px())
        } else {
            CssPixels::default()
        };
    }

    // NB: Intrinsic size overrides use the box's own writing mode, before its sizes are projected into the
    //     parent's frame.
    if changes_frame {
        // NB: The lines are finished at the inline size the contents were laid out at, before the root's own box is
        //     sized in its parent's frame.
        root_lines.finish(
            callbacks.arena(),
            own_root_after_body.content_inline_size,
            own_frame.writing_mode(),
        );
        result = result_in_parent_frame(result, own_root_after_body, parent_frame, own_frame);
    }
    root.set(own_root_after_body.in_frame(own_frame, parent_frame));

    if containment_facts.has_preferred_aspect_ratio() {
        result.content_block_size_for_aspect_ratio_minimum =
            content_block_size_for_aspect_ratio_minimum(&run.callbacks, run.box_, &result);
    }

    match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => {
            finalize_block_level_root(run, sized_root, &input, &result);
        }
        ParticipationInParentFormattingContext::Float { .. } => {
            finalize_float_root(run, sized_root, &input, &result);
        }
        ParticipationInParentFormattingContext::AtomicInline => {
            finalize_atomic_root_block_size(run, sized_root, &input, result.automatic_content_block_size);
        }
        ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
            abspos_engine::AbsposEngine::for_run(run).finalize_out_of_flow_root_after_inside_layout(
                abspos_engine::OutOfFlowBox::new(
                    sized_root,
                    &root_used_insets,
                    run.state.anchor_resolved_values_of(run.box_),
                ),
                abspos_inputs,
                Some(abspos_engine::InsideLayoutBlockSizes {
                    automatic_content_block_size: result.automatic_content_block_size,
                    content_block_size_for_aspect_ratio_minimum: result.content_block_size_for_aspect_ratio_minimum,
                }),
            );
            result.out_of_flow_root_used_insets = Some(root_used_insets.get());
        }
        ParticipationInParentFormattingContext::Item => {
            if run_reads_content_block_size_adopted_after_it(fc_type, &input)
                && let Some(block_size) =
                    content_block_size_adopted_after_run(&input, result.automatic_content_block_size)
            {
                sized_root.cells.set_content_block_size(block_size);
            }
        }
        ParticipationInParentFormattingContext::Root => {}
    }
    if matches!(
        input.participation,
        ParticipationInParentFormattingContext::BlockLevel
            | ParticipationInParentFormattingContext::Float { .. }
            | ParticipationInParentFormattingContext::AtomicInline
    ) {
        let sizing = run.sizing();
        sizing.apply_automatic_minimum_block_size_from_aspect_ratio(
            sized_root,
            sizing.available_space_for_block_size_resolution(
                run.box_,
                input.available_space,
                input.containing_block_constraints,
            ),
            input.containing_block_constraints,
            result.content_block_size_for_aspect_ratio_minimum,
        );
    }
    let root_cells_left_by_run = root.get();
    let root_in_own_frame_left_by_run = root_cells_left_by_run.in_frame(parent_frame, own_frame);
    let root_line_data = root_lines.finish(
        callbacks.arena(),
        root_in_own_frame_left_by_run.content_inline_size,
        own_frame.writing_mode(),
    );
    result.depends_on_percentage_block_size = run.sizing().resolve_percentage_block_size_dependency(run.box_);
    let item_run_observations_are_known = !run_is_handed_table_inline_layout_outside_its_key;
    let inputs_never_observed = fc_run_cache::RunInputsTheStoredRunNeverObserved {
        percentage_block_basis_and_block_size_definiteness: !result.depends_on_percentage_block_size,
        definite_available_inline_sizes_at_or_above: atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
        percentage_inline_basis: atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above.is_some()
            || (item_run_observations_are_known
                && item_run_never_observes_percentage_inline_basis(run, fc_type, &input)),
        own_block_size: if item_run_observations_are_known {
            own_block_size_observation_of_item_run(run, fc_type, &input, &result)
        } else {
            OwnBlockSizeObservation::Observed
        },
    };

    // NB: The root's descendants are laid out in the frame the root establishes.
    let take_run_fragments = || {
        run.fragments
            .as_ref()
            .map(|fragments| fragments.take_unplaced_root(&body_run.callbacks))
    };

    let registered_abspos_children_could_never_be_laid_out = run.fragments.is_none();
    if registered_abspos_children_could_never_be_laid_out {
        let outputs = run.outputs(
            result,
            take_run_fragments(),
            root_cells_left_by_run,
            root_line_data,
            inputs_never_observed,
            parent_owns_root_input_cells,
        );
        return (outputs, root_cells_left_by_run);
    }
    if let Some(implementation) = implementation {
        match &implementation {
            FormattingContextImplementation::Block(_) => {}
            FormattingContextImplementation::Table(_) => {
                register_table_abspos_descendants(body_run, run.box_);
            }
            FormattingContextImplementation::Flex(context) => {
                context.parent_did_dimension(root_in_own_frame_left_by_run);
            }
            FormattingContextImplementation::Grid(context) => {
                context.parent_did_dimension(root_in_own_frame_left_by_run);
            }
            FormattingContextImplementation::Math(context) => {
                context.parent_did_dimension(root_in_own_frame_left_by_run);
            }
            FormattingContextImplementation::Svg(_) | FormattingContextImplementation::ReplacedWithChildren => {}
            FormattingContextImplementation::InternalReplaced => {
                let outputs = run.outputs(
                    result,
                    take_run_fragments(),
                    root_cells_left_by_run,
                    root_line_data,
                    inputs_never_observed,
                    parent_owns_root_input_cells,
                );
                return (outputs, root_cells_left_by_run);
            }
        }
    }
    let outputs = run.outputs(
        result,
        take_run_fragments(),
        root_cells_left_by_run,
        root_line_data,
        inputs_never_observed,
        parent_owns_root_input_cells,
    );
    (outputs, root_cells_left_by_run)
}

/// The frame in which `node` lays out its contents: that of its writing mode.
pub(crate) fn frame_established_by(callbacks: &LayoutPass<'_>, node: Node) -> geometry::Frame {
    callbacks
        .computed_values_view_if_styled(node)
        .map_or(callbacks.frame, |style| geometry::Frame::of(style.writing_mode()))
}

/// The input for laying out the contents of a run root whose writing mode establishes a frame other than its parent's,
/// in that frame.
fn body_input_in_own_frame(
    run: &FormattingContextRun,
    body_run: &FormattingContextRun,
    sized_root_in_own_frame: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
    body_input: LayoutInput,
    parent_frame: geometry::Frame,
    own_frame: geometry::Frame,
) -> LayoutInput {
    let node = run.box_;
    let used = sized_root_in_own_frame.cells;
    if !parent_frame.is_orthogonal_to(own_frame) {
        // NB: Sizes keep their axes between frames that are not orthogonal, so the contents are laid out with the
        //     input the root's sizing gave them, apart from the parent's block formatting context they leave.
        return body_input;
    }
    let mut constraints = input.containing_block_constraints;
    let mut available_space = input.available_space;
    let mut sizing = RootSizingDirectives {
        treat_block_axis_percentage_insets_as_auto_beyond_root: input
            .sizing
            .treat_block_axis_percentage_insets_as_auto_beyond_root,
        forced_content_inline_size: body_input.sizing.forced_content_inline_size,
        forced_content_block_size: body_input.sizing.forced_content_block_size,
        root_keeps_its_committed_geometry: input.sizing.root_keeps_its_committed_geometry,
        ..RootSizingDirectives::default()
    };
    std::mem::swap(
        &mut constraints.percentage_basis_inline_size,
        &mut constraints.percentage_basis_block_size,
    );
    constraints.quirks_mode_percentage_basis_block_size = None;
    std::mem::swap(&mut available_space.inline_size, &mut available_space.block_size);
    std::mem::swap(
        &mut sizing.forced_content_inline_size,
        &mut sizing.forced_content_block_size,
    );
    // https://drafts.csswg.org/css-writing-modes-4/#orthogonal-flows
    // In the sizing phase [...] the dimensions of the box and the containing block are mapped to the inline
    // size and block size and calculations are performed accordingly using the writing mode of the box
    // establishing the orthogonal flow.
    if !used.has_definite_inline_size() {
        // NB: A flex or grid container sizes its items in both axes before laying out their contents, even where it
        //     leaves a size indefinite for the percentages of their contents.
        let inline_size = if input.participation == ParticipationInParentFormattingContext::Item
            && NodeFacts::new(&run.callbacks, node).parent_is_flex_or_grid_container()
        {
            used.content_inline_size()
        } else {
            body_run.sizing().orthogonal_flow_inline_size(
                sized_root_in_own_frame.sized_box(),
                available_space,
                constraints,
            )
        };
        used.set_content_inline_size(inline_size);
    }
    let mut body_input = LayoutInput::new(
        used.available_inner_space_or_constraints_from(available_space),
        constraints,
        ParticipationInParentFormattingContext::Root,
    );
    body_input.sizing = sizing;
    body_input
}

/// Maps what the layout of a run root's contents in its own frame found into its parent's frame.
fn result_in_parent_frame(
    mut result: ChildLayoutResult,
    root_in_own_frame: used_values::RootInputCells,
    parent_frame: geometry::Frame,
    own_frame: geometry::Frame,
) -> ChildLayoutResult {
    if parent_frame.is_orthogonal_to(own_frame) {
        // https://drafts.csswg.org/css-writing-modes-4/#orthogonal-shrink-to-fit
        // Note that this section requires that when a child box auto-sized in its block axis establishes an
        // orthogonal flow, the used block size of the child is calculated to fit its content; and this resulting
        // content-based size is used as input to the inline-axis min-content size and max-content size of the
        // parent.
        let content_inline_size = root_in_own_frame.content_inline_size;
        // NB: The root's baselines lie across its own block axis, which is the horizontal axis of its parent's frame.
        let block_start_is_left = matches!(
            own_frame.side_in(geometry::Side::Top, parent_frame),
            geometry::Side::Left
        );
        let content_block_size = root_in_own_frame.content_block_size;
        let along_parent_horizontal_axis = |baseline: Option<CssPixels>| {
            baseline.map(|baseline| {
                if block_start_is_left {
                    baseline
                } else {
                    content_block_size - baseline
                }
            })
        };
        let orthogonal_flow_baselines = DerivedBaselines {
            inline_alignment: None,
            first: along_parent_horizontal_axis(result.baselines.first),
            last: along_parent_horizontal_axis(result.baselines.last),
        };
        result.orthogonal_flow_baselines =
            (orthogonal_flow_baselines != DerivedBaselines::default()).then_some(orthogonal_flow_baselines);
        // NB: The wrapper's content inline size in its own frame is the border box inline size of its table box.
        let table_box_border_box_inline_size = result.table_wrapper_content_inline_size;
        // NB: The inline size of a table wrapper in its parent's frame is the block size of its content in its own
        //     frame.
        if result.table_wrapper_content_inline_size.is_some() {
            result.table_wrapper_content_inline_size = Some(result.automatic_content_block_size);
        }
        result.automatic_content_block_size = content_inline_size;
        result.baselines = DerivedBaselines::default();
        result.content_block_size_for_aspect_ratio_minimum = None;
        // NB: The block size of a table box in its parent's frame is its inline size in the frame of its wrapper.
        if result.table_box_in_wrapper_border_box_block_size.is_some() {
            result.table_box_in_wrapper_border_box_block_size = table_box_border_box_inline_size;
        }
    }
    result
}

fn finalize_atomic_root_block_size(
    run: &FormattingContextRun,
    sized_root: sizing_context::SizedCells<'_>,
    input: &LayoutInput,
    automatic_content_block_size_of_completed_body_run: CssPixels,
) {
    let node = run.box_;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let sizing = run.sizing();
    if sizing.box_is_sized_as_replaced_element(sized_root.sized_box(), available_space, constraints) {
        return;
    }
    if sizing.should_treat_block_size_as_auto(sized_root.sized_box(), available_space, constraints) {
        let parent_lays_out_root_as_math_content =
            formatting_context_type_created_by_box(NodeFacts::new(&run.callbacks, run.callbacks.parent(node)))
                == Some(FormattingContextType::Math);
        let automatic_block_size = if parent_lays_out_root_as_math_content {
            automatic_content_block_size_of_completed_body_run
        } else {
            block_formatting_context::floor_list_item_automatic_block_size_by_marker_line_height(
                run.callbacks,
                node,
                automatic_content_block_size_of_completed_body_run,
            )
        };
        sizing.resolve_used_block_size_if_treated_as_auto(
            sized_root,
            available_space,
            constraints,
            Some(automatic_block_size),
            || unreachable!("a completed atomic run always supplies its automatic content block size"),
        );
    } else {
        sizing.resolve_used_block_size_if_not_treated_as_auto(sized_root, available_space, constraints);
    }
}

pub(crate) fn propagate_percentage_block_size_dependency_to_containing_block(
    state: &RunState,
    callbacks: &LayoutPass<'_>,
    child: Node,
    child_depends_on_percentage_block_size: bool,
) {
    let run_root_reports_its_dependency_through_its_run_result = child == state.root();
    if run_root_reports_its_dependency_through_its_run_result {
        return;
    }
    let facts = NodeFacts::new(callbacks, child);
    let resolves_against_containing_blocks_final_size = facts.is_absolutely_positioned();
    if resolves_against_containing_blocks_final_size {
        return;
    }
    let relative_block_insets_resolve_against_containing_block = facts.is_relatively_positioned() && {
        let style = StyleValues::for_node(callbacks, child);
        style.inset_top().contains_percentage() || style.inset_bottom().contains_percentage()
    };
    if !child_depends_on_percentage_block_size && !relative_block_insets_resolve_against_containing_block {
        return;
    }
    // NB: The nearest box at or above the containing block whose dependency this run resolves hears of it, which is
    //     the run root where the containing block lies above it.
    let mut box_resolving_the_dependency = callbacks.in_flow_containing_block(child);
    while !box_resolving_the_dependency.is_invalid()
        && !state.resolves_percentage_block_size_dependency_of(box_resolving_the_dependency)
    {
        box_resolving_the_dependency = callbacks.parent(box_resolving_the_dependency);
    }
    if box_resolving_the_dependency.is_invalid() {
        box_resolving_the_dependency = state.root();
    }
    state.note_descendant_depends_on_percentage_block_size(box_resolving_the_dependency);
}

/// The layout of a child: its outcome, the fragments its placement takes, and the child's box as its layout left it,
/// unless the parent lays out the child's contents itself.
pub(crate) struct ChildLayout {
    pub(crate) outcome: ChildLayoutOutcome,
    pub(crate) fragment_arrival: Option<fragment_tree::ChildFragmentArrival>,
    pub(crate) laid_out_box: run_state::LaidOutChildBox,
}

impl ChildLayout {
    fn of_child_sized_in_parent_run(outcome: ChildLayoutOutcome, cells: used_values::RootInputCells) -> Self {
        Self {
            outcome,
            fragment_arrival: None,
            laid_out_box: run_state::LaidOutChildBox::sized_here(cells, DerivedBaselines::default()),
        }
    }
}

/// Lays out `child` with the input cells its parent sized it to: hands them to the child's run, or sizes the child in
/// them here where it needs no run of its own.
#[allow(clippy::too_many_arguments)]
pub(crate) fn layout_inside_child_with_input_cells(
    run: &FormattingContextRun,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    child: Node,
    input: LayoutInput,
    mut child_input_cells: used_values::RootInputCells,
    force_independent_context_run: bool,
    percentage_block_insets_of_containing_block_have_definite_basis: bool,
) -> ChildLayout {
    let facts = NodeFacts::new(&run.callbacks, child);
    let child_run_builds_fragments = run_builds_fragments(&input, run.fragments.is_some());
    if let Some((padding_top, padding_bottom)) = input.sizing.table_cell_intrinsic_block_padding {
        debug_assert!(facts.is_table_cell());
        debug_assert!(matches!(
            input.participation,
            ParticipationInParentFormattingContext::Item
        ));
        child_input_cells.padding_top += padding_top;
        child_input_cells.padding_bottom += padding_bottom;
    }
    let skipped = |out_of_flow_root_used_insets| ChildLayoutOutcome::Skipped {
        depends_on_percentage_block_size: run.sizing().skipped_child_depends_on_percentage_block_size(child),
        out_of_flow_root_used_insets,
    };
    if !force_independent_context_run
        && !facts.node_has_size_containment()
        && matches!(
            input.participation,
            ParticipationInParentFormattingContext::AtomicInline
        )
        && formatting_context_type_created_by_box(facts) == Some(FormattingContextType::Block)
        && run.callbacks.first_child(child).is_invalid()
    {
        // OPTIMIZATION: An empty atomic block has no formatting-context body output. Size it in the
        // parent run, which will also construct its fragment when placing the box.
        let cells_of_this_layout = Cell::new(child_input_cells);
        let committed_fragment = run.sizing().dimension_empty_atomic_root(
            sizing_context::SizedCells::new(child, &cells_of_this_layout, run.callbacks.frame),
            input.available_space,
            input.containing_block_constraints,
            child_run_builds_fragments,
        );
        // OPTIMIZATION: A box that took its metrics from its committed fragment is placed with that fragment, where
        // it shows the box as its cells hold it.
        let fragment_arrival = committed_fragment
            .filter(|fragment| {
                run.fragments.is_some()
                    && fragment.is_plain_box()
                    && fragment_tree::FragmentGeometry {
                        cells: cells_of_this_layout.get(),
                        line_data: None,
                    }
                    .shows_box_of_fragment(fragment)
                    && !facts.has_anchor_names()
            })
            .map(|fragment| fragment_tree::ChildFragmentArrival::CommittedFragment {
                fragment,
                reuses_subtree: false,
            });
        let laid_out_box =
            run_state::LaidOutChildBox::sized_here(cells_of_this_layout.get(), DerivedBaselines::default());
        return ChildLayout {
            outcome: skipped(None),
            fragment_arrival,
            laid_out_box,
        };
    }
    let creates_replaced_context = matches!(
        formatting_context_type_created_by_box(facts),
        Some(FormattingContextType::InternalReplaced | FormattingContextType::ReplacedWithChildren)
    );
    if !facts.can_have_children() && !creates_replaced_context {
        let cells_of_this_layout = Cell::new(child_input_cells);
        let out_of_flow_root_used_insets =
            size_skipped_independent_root(run, parent_block, child, &cells_of_this_layout, &input);
        return ChildLayout::of_child_sized_in_parent_run(
            skipped(out_of_flow_root_used_insets),
            cells_of_this_layout.get(),
        );
    }

    let ChildRunPlan {
        fc_type,
        root_containing_block,
        input,
    } = child_run_plan(
        run,
        child,
        input,
        force_independent_context_run,
        percentage_block_insets_of_containing_block_have_definite_basis,
    );
    let child_run_outcome = run_formatting_context(
        run.fragments.as_deref(),
        child_input_cells,
        child,
        root_containing_block,
        parent_grid,
        fc_type,
        run.should_collect_devtools_layout_data,
        run.callbacks,
        input,
        parent_block
            .and_then(|block| block.take_table_inline_layout_of(child))
            .or_else(|| parent_grid.and_then(|grid| grid.take_table_inline_layout_of(child))),
    );
    let ChildRunOutcome {
        result,
        laid_out_box,
        fragment_arrival,
    } = child_run_outcome;
    ChildLayout {
        outcome: ChildLayoutOutcome::Created(result),
        fragment_arrival,
        laid_out_box,
    }
}

/// Hands what the layout of a child handed its placement to the run's fragment builder, where the child is laid out
/// but not placed.
pub(crate) fn leave_child_unplaced(
    run: &FormattingContextRun,
    unplaced_child: PlacedChild,
    fragment_arrival: Option<fragment_tree::ChildFragmentArrival>,
) {
    if let Some(arrival) = fragment_arrival {
        run.fragments
            .as_deref()
            .expect("only a run that builds fragments is handed a child's fragments")
            .leave_child_unplaced(arrival, unplaced_child);
    }
}

/// The run a table cell's committing run will be (see layout_inside_child_with_input_cells), but for the intrinsic
/// block padding that its table hands it once rows are sized. Laid out ahead of that padding, its outputs stay in the
/// run cache, and the committing run replays them unless they show that the run observed the root's block padding (see
/// fc_run_cache::run_observes_root_block_padding).
pub(crate) fn table_cell_run_shape_ahead_of_its_intrinsic_block_padding(
    run: &FormattingContextRun,
    table_box: &impl BoxMetricCells,
    cell: Node,
    cell_input_cells: used_values::RootInputCells,
    mut input: LayoutInput,
) -> ChildRunShape {
    debug_assert!(matches!(
        input.participation,
        ParticipationInParentFormattingContext::Item
    ));
    input.sizing.table_cell_intrinsic_block_padding = Some((CssPixels::default(), CssPixels::default()));
    // NB: A table cell is a containing block of its own to the percentage block insets of its descendants.
    let plan = child_run_plan(
        run,
        cell,
        input,
        false,
        run.percentage_block_insets_of_root_children_have_definite_basis(table_box),
    );
    ChildRunShape {
        node: cell,
        fc_type: plan.fc_type,
        input: plan.input,
        root_cells: cell_input_cells,
        root_containing_block: plan.root_containing_block,
    }
}

#[derive(Clone, Copy, Debug, PartialEq)]
pub(crate) struct ChildRunShape {
    pub(crate) node: Node,
    pub(crate) fc_type: FormattingContextType,
    pub(crate) input: LayoutInput,
    pub(crate) root_cells: used_values::RootInputCells,
    pub(crate) root_containing_block: Node,
}

impl ChildRunShape {
    pub(crate) fn lays_out_at(&self, inline_size: CssPixels, constraints: ContainingBlockConstraints) -> bool {
        self.root_cells.content_inline_size == inline_size && self.input.containing_block_constraints == constraints
    }
}

pub(crate) trait ChildRunShapes {
    fn child_run_shape_at(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> Option<ChildRunShape>;
}

pub(crate) fn item_is_block_container_whose_committing_run_can_adopt_its_measurement(
    sizing: &sizing_context::SizingContext<'_, '_>,
    item: Node,
) -> bool {
    let facts = sizing.facts(item);
    !facts.document_in_quirks_mode()
        && !facts.is_table_wrapper()
        && !facts.is_replaced_box()
        && !facts.has_preferred_aspect_ratio()
        && sizing.style(item).writing_mode() == writing_mode::HORIZONTAL_TB
        && !sizing.establishes_orthogonal_flow(item)
        && formatting_context_type_created_by_box(facts) == Some(FormattingContextType::Block)
        && !sizing.own_style_depends_on_percentage_block_size(item)
        && !block_formatting_context::root_lays_out_content_against_its_own_block_size(sizing, item)
}

impl ChildRunShape {
    pub(crate) fn of_measurement_root(
        callbacks: &LayoutPass<'_>,
        node: Node,
        root_cells: used_values::RootInputCells,
        input: LayoutInput,
    ) -> Self {
        debug_assert_eq!(input.participation, ParticipationInParentFormattingContext::Root);
        Self {
            node,
            fc_type: independent_formatting_context_type(node, callbacks),
            input,
            root_cells,
            root_containing_block: callbacks.containing_block_for_child_run(node, &input.participation),
        }
    }
}

pub(crate) struct ChildRunPlan {
    pub(crate) fc_type: FormattingContextType,
    pub(crate) root_containing_block: Node,
    pub(crate) input: LayoutInput,
}

/// How `child` is laid out by a run of its own, where the block-axis percentage insets of the boxes in the child's
/// containing block resolve against a definite block size if
/// `percentage_block_insets_of_containing_block_have_definite_basis`.
pub(crate) fn child_run_plan(
    run: &FormattingContextRun,
    child: Node,
    mut input: LayoutInput,
    force_independent_context_run: bool,
    percentage_block_insets_of_containing_block_have_definite_basis: bool,
) -> ChildRunPlan {
    let fc_type = formatting_context_type_created_by_box(NodeFacts::new(&run.callbacks, child)).unwrap_or_else(|| {
        assert!(
            force_independent_context_run,
            "{child:?} is handed to a run of its own without a formatting context of its own"
        );
        independent_formatting_context_type(child, &run.callbacks)
    });
    let root_containing_block = run
        .callbacks
        .containing_block_for_child_run(child, &input.participation);
    // NB: Percentages see through an anonymous root to the block size of its containing block.
    let child_facts = NodeFacts::new(&run.callbacks, child);
    input.sizing.treat_block_axis_percentage_insets_as_auto_beyond_root = child_facts.is_anonymous()
        && !child_facts.is_table_cell()
        && !percentage_block_insets_of_containing_block_have_definite_basis;
    ChildRunPlan {
        fc_type,
        root_containing_block,
        input,
    }
}

pub(crate) fn lay_out_measurement_root_for_its_result(
    callbacks: LayoutPass<'_>,
    node: Node,
    root_cells: used_values::RootInputCells,
    input: LayoutInput,
) -> ChildLayoutResult {
    lay_out_shape_for_its_result(
        callbacks,
        ChildRunShape::of_measurement_root(&callbacks, node, root_cells, input),
    )
}

pub(crate) fn lay_out_shape_for_its_result(callbacks: LayoutPass<'_>, shape: ChildRunShape) -> ChildLayoutResult {
    let ChildRunShape {
        node,
        fc_type,
        input,
        root_cells,
        root_containing_block,
    } = shape;
    let builds_fragments = run_builds_fragments(&input, false);
    let lay_out = |callbacks: LayoutPass<'_>, root_cells, input, previous_line_data| {
        execute_formatting_context_run(
            builds_fragments,
            root_cells,
            node,
            root_containing_block,
            None,
            fc_type,
            false,
            callbacks,
            input,
            previous_line_data,
            None,
            None,
        )
    };
    let result_with_host_findings = |outputs: &RunOutputs| {
        callbacks.receive_host_findings(outputs.host_findings.clone());
        outputs.result
    };
    if callbacks.reads_no_memoized_layout() {
        return result_with_host_findings(&lay_out(callbacks, root_cells, input, None));
    }
    let runs_ahead_of_intrinsic_block_padding =
        builds_fragments && input.sizing.table_cell_intrinsic_block_padding.is_some();
    let cache_key = fc_run_cache::FcRunCacheKey::new(fc_type, input, root_cells);
    let cache_attempt = match fc_run_cache::FcRunCacheAttempt::probe(
        node,
        false,
        builds_fragments,
        false,
        true,
        &callbacks,
        &cache_key,
    ) {
        Ok(attempt) => attempt,
        Err(hit) => {
            let action = if runs_ahead_of_intrinsic_block_padding {
                "READ RESULT AHEAD OF INTRINSIC BLOCK PADDING"
            } else {
                "READ RESULT"
            };
            let _trace = callbacks.layout_trace().run(node, fc_type, || action);
            return result_with_host_findings(&hit.entry.outputs);
        }
    };
    let action = if runs_ahead_of_intrinsic_block_padding {
        "RUN AHEAD OF INTRINSIC BLOCK PADDING"
    } else {
        cache_attempt.trace_action()
    };
    let _trace = callbacks.layout_trace().run(node, fc_type, || action);
    let previous_line_data = cache_attempt.previous_line_data();
    let discarded_outputs =
        run_repeatability::lay_out_discarded_run(&callbacks, |callbacks| lay_out(callbacks, root_cells, input, None));
    let outputs = lay_out(callbacks, root_cells, input, previous_line_data);
    if let Some(discarded_outputs) = &discarded_outputs {
        run_repeatability::verify_repeated_run(&callbacks, node, discarded_outputs, &outputs);
    }
    if cache_attempt.stores_outputs() {
        run_repeatability::verify_run_ignores_its_unobserved_own_block_size_and_block_margins(
            &callbacks,
            node,
            root_cells,
            input,
            &outputs,
            |callbacks, other_root_cells, other_input| lay_out(callbacks, other_root_cells, other_input, None),
        );
    }
    cache_attempt.conclude(&callbacks, node, cache_key, &input, &outputs);
    result_with_host_findings(&outputs)
}

// NB: An item that establishes an orthogonal flow sizes itself along its container's block axis, so its run owns its
//     root input cells.
pub(super) fn parent_owns_root_input_cells(callbacks: &LayoutPass<'_>, box_: Node, input: &LayoutInput) -> bool {
    input.participation == ParticipationInParentFormattingContext::Item
        && !frame_established_by(callbacks, box_).is_orthogonal_to(callbacks.frame)
}

fn own_block_size_observation_of_item_run(
    run: &FormattingContextRun,
    fc_type: FormattingContextType,
    input: &LayoutInput,
    result: &ChildLayoutResult,
) -> OwnBlockSizeObservation {
    let item_is_plain_block_container_laid_out_without_its_own_block_size = fc_type == FormattingContextType::Block
        && !run.callbacks.frame.is_transposed()
        && parent_owns_root_input_cells(&run.callbacks, run.box_, input)
        && !result.depends_on_percentage_block_size
        && !run
            .state
            .percentage_block_size_dependency_of(run.box_)
            .has_descendant_that_depends_on_percentage_block_size
        && !block_formatting_context::root_lays_out_content_against_its_own_block_size(&run.sizing(), run.box_)
        && !root_content_includes_an_orthogonal_flow_sized_by_the_root_block_size(&run.callbacks, run.box_);
    if item_is_plain_block_container_laid_out_without_its_own_block_size {
        OwnBlockSizeObservation::Unobserved
    } else {
        OwnBlockSizeObservation::Observed
    }
}

fn root_content_includes_an_orthogonal_flow_sized_by_the_root_block_size(
    callbacks: &LayoutPass<'_>,
    root: Node,
) -> bool {
    let mut boxes_whose_children_the_root_contains: smallvec::SmallVec<[Node; 8]> = smallvec::smallvec![root];
    while let Some(container) = boxes_whose_children_the_root_contains.pop() {
        for child in callbacks.children(container) {
            let facts = NodeFacts::new(callbacks, child);
            if facts.is_text_node() || facts.is_absolutely_positioned() {
                continue;
            }
            if frame_established_by(callbacks, child).is_orthogonal_to(callbacks.frame) {
                return true;
            }
            if facts.is_anonymous() || (facts.is_inline() && !facts.is_atomic_inline()) {
                boxes_whose_children_the_root_contains.push(child);
            }
        }
    }
    false
}

/// Whether the run is of an item block container whose content resolves percentages against the container itself.
fn item_run_never_observes_percentage_inline_basis(
    run: &FormattingContextRun,
    fc_type: FormattingContextType,
    input: &LayoutInput,
) -> bool {
    fc_type == FormattingContextType::Block
        && parent_owns_root_input_cells(&run.callbacks, run.box_, input)
        && !run.sizing().measurement_root_observes_percentage_inline_basis(run.box_)
}

pub(super) fn content_block_size_adopted_after_run(
    input: &LayoutInput,
    automatic_content_block_size: CssPixels,
) -> Option<CssPixels> {
    if input.participation != ParticipationInParentFormattingContext::Item {
        return None;
    }
    input.sizing.content_block_size_after_inside_layout.or(input
        .sizing
        .adopt_automatic_content_block_size
        .then_some(automatic_content_block_size))
}

pub(super) fn run_reads_content_block_size_adopted_after_it(
    fc_type: FormattingContextType,
    input: &LayoutInput,
) -> bool {
    input.participation == ParticipationInParentFormattingContext::Item
        && fc_type.registers_abspos_children_against_its_final_block_size()
}

fn child_run_outcome_from_outputs(
    callbacks: &LayoutPass<'_>,
    parent_fragments: Option<&fragment_tree::RunFragmentBuilder>,
    child: Node,
    input: &LayoutInput,
    root_cells: used_values::RootInputCells,
    outputs: RunOutputs,
    reuses_committed_subtree: bool,
) -> ChildRunOutcome {
    let RunOutputs {
        result,
        root,
        root_outcome,
        inputs_never_observed: _,
        host_findings,
    } = outputs;
    callbacks.receive_host_findings(host_findings);
    let content_block_size_adopted_after_run =
        content_block_size_adopted_after_run(input, result.automatic_content_block_size);
    let laid_out_box = root_outcome.laid_out_child_box(root_cells, &result, content_block_size_adopted_after_run);
    let fragment_arrival = parent_fragments.and(root).map(|root| {
        debug_assert!(root.node == child, "a child run returned a root for a different box");
        fragment_tree::ChildFragmentArrival::RunRoot {
            root,
            reuses_committed_subtree,
        }
    });
    ChildRunOutcome {
        result,
        laid_out_box,
        fragment_arrival,
    }
}

pub(super) fn independent_formatting_context_type(box_: Node, callbacks: &LayoutPass<'_>) -> FormattingContextType {
    let facts = NodeFacts::new(callbacks, box_);
    if let Some(fc_type) = formatting_context_type_created_by_box(facts) {
        return fc_type;
    }
    assert!(
        facts.is_block_container(),
        "a box that creates no formatting context must be a block container to be laid out in one"
    );
    FormattingContextType::Block
}

/// Whether the block-axis percentage insets of the boxes whose containing block is `containing_block` resolve against a
/// definite block size: whether the box has a definite block size, or, where it is an anonymous box percentages see
/// through, whether `basis_beyond_it_is_definite`.
pub(crate) fn percentage_block_insets_have_definite_basis_in(
    callbacks: &LayoutPass<'_>,
    containing_block: Node,
    containing_block_has_definite_block_size: impl FnOnce() -> bool,
    basis_beyond_it_is_definite: bool,
) -> bool {
    let facts = NodeFacts::new(callbacks, containing_block);
    if !facts.is_anonymous() || facts.is_table_cell() {
        return containing_block_has_definite_block_size();
    }
    basis_beyond_it_is_definite
}

/// Lays the document out from its viewport over the arena and layout scratch of `state`:
/// propagates the root and body styles the viewport takes over, syncs enrolled content, then
/// computes and commits fragments. Answers what committing them owes the host; what the
/// propagated styles owe the shells goes into `work`.
pub(crate) fn lay_out_root(
    state: &mut super::ArenaHandle,
    work: &super::tree_mutation::OwedHostWork,
    root: NodeSlotId,
    stage: LayoutStageFacts,
    should_collect_devtools_layout_data: bool,
) -> commit::CommitNotifications {
    assert!(!root.is_invalid());
    // The style rewrites enroll the affected boxes' text children for content sync, so the sync
    // follows them, and both precede the pass, which caches decoded style.
    let facts = viewport_propagation::viewport_propagation_facts(state.arena());
    viewport_propagation::propagate_root_styles_to_viewport(
        super::tree_mutation::HostCalls(work),
        state.arena_mut(),
        root,
        &facts,
    );
    super::layout_node_arena::sync_enrolled_content_for_layout(state.arena_mut());
    LayoutStageJob {
        kind: LayoutStageKind::Root {
            should_collect_devtools_layout_data,
        },
        root,
        stage,
    }
    .run(state)
}

/// Lays out one partial relayout boundary over the arena and layout scratch of `state`, in the
/// containing block the planner found it still has, and commits its fragments. Answers what
/// committing them owes the host. Enrolled content is not synced here: the caller syncs once
/// ahead of a batch of boundaries.
pub(crate) fn lay_out_boundary(
    state: &mut super::ArenaHandle,
    boundary: super::partial_relayout::PlannedBoundary,
    stage: LayoutStageFacts,
) -> commit::CommitNotifications {
    LayoutStageJob {
        kind: LayoutStageKind::Boundary {
            containing_block: boundary.containing_block(),
        },
        root: boundary.root(),
        stage,
    }
    .run(state)
}

/// What every layout stage of a round reads besides the arena: the document's viewport and quirks
/// mode as the round began.
#[derive(Clone, Copy)]
pub(crate) struct LayoutStageFacts {
    pub(crate) viewport_inline_size: CssPixels,
    pub(crate) viewport_block_size: CssPixels,
    pub(crate) document_in_quirks_mode: bool,
}

/// What a layout stage computes from: the arena and the layout stage's scratch, borrowed for the
/// pass, and the facts of the round. It holds no main thread token, so nothing the stage calls can
/// reach the rest of the host.
struct LayoutStageInput<'a> {
    arena: &'a LayoutNodeArena,
    scratch: &'a super::run_state::LayoutScratch,
    root: NodeSlotId,
    stage: LayoutStageFacts,
}

/// The fragments a layout stage computed, which its commit consumes, and what the stage found out for the host.
struct LayoutStageOutput {
    fragments: fragment_tree::CompletedPassFragments,
    host_findings: super::run_state::HostFindings,
}

/// One layout stage: computes the fragments of the whole document from its viewport, or of one
/// partial relayout boundary in the containing block the planner found it still has.
struct LayoutStageJob {
    kind: LayoutStageKind,
    root: NodeSlotId,
    stage: LayoutStageFacts,
}

#[derive(Clone, Copy)]
enum LayoutStageKind {
    Root { should_collect_devtools_layout_data: bool },
    Boundary { containing_block: NodeSlotId },
}

impl LayoutStageJob {
    /// Runs the stage over the arena and layout scratch of `state`, commits the fragments it
    /// computed, and settles the arena-local bookkeeping every stage owes its caller: cache
    /// maintenance, and the reset of the update flags the committed subtree satisfied. Answers what
    /// committing them owes the host.
    fn run(self, state: &mut super::ArenaHandle) -> commit::CommitNotifications {
        let Self { kind, root, stage } = self;
        prepare_layout_stage(state, kind, root);
        let input = LayoutStageInput {
            arena: state.arena(),
            scratch: state.layout_scratch(),
            root,
            stage,
        };
        let output = match kind {
            LayoutStageKind::Root {
                should_collect_devtools_layout_data,
            } => compute_root_layout(input, should_collect_devtools_layout_data),
            LayoutStageKind::Boundary { containing_block } => compute_subtree_layout_fragments(input, containing_block),
        };
        let mut notifications = commit::commit_replacing(root, state.arena_mut(), &output.fragments);
        let super::run_state::HostFindings {
            messages,
            document_svg_root_natural_size,
        } = output.host_findings;
        if let Some((root_box, natural_size)) = document_svg_root_natural_size {
            state
                .arena_mut()
                .note_document_svg_root_natural_size(root_box, natural_size);
        }
        notifications.prepend_layout_messages(messages);
        let (arena, scratch) = state.arena_mut_and_layout_scratch();
        scratch.layout_trace.hand_back_to(&mut arena.layout_trace);
        scratch.end_layout_pass(arena);
        arena.reset_layout_update_flags_in_subtree(root);
        arena.leave_active_layout_pass();
        match kind {
            LayoutStageKind::Root { .. } => arena.did_commit_full_layout(root),
            LayoutStageKind::Boundary { .. } => {
                // Commit reset the subtree's rows, and its new size may affect ancestor scrollable
                // overflow. Partial relayout roots are SVG viewports or abspos boxes, never SVG
                // content boxes that would require a new layout instead of an overflow update.
                debug_assert!(!node_facts::kind_is_svg_box(arena.data(root).kind.get()));
                arena.schedule_scrollable_overflow_recalculation(root);
            }
        }
        notifications
    }
}

/// The full layout stage: computes the fragments of the document from its viewport, without the
/// host.
fn compute_root_layout(input: LayoutStageInput<'_>, should_collect_devtools_layout_data: bool) -> LayoutStageOutput {
    let LayoutStageInput {
        arena,
        scratch,
        root,
        stage,
    } = input;
    let callbacks = LayoutPass::new(arena, scratch, stage);
    let viewport_inline_size = stage.viewport_inline_size;
    let viewport_block_size = stage.viewport_block_size;

    let root_constraints = ContainingBlockConstraints {
        percentage_basis_inline_size: Some(viewport_inline_size),
        percentage_basis_block_size: Some(viewport_block_size),
        ..ContainingBlockConstraints::default()
    };
    RunState::with_root(callbacks, root, Node::INVALID, None, |entry_run_state, callbacks| {
        let _trace = scratch.layout_trace.pass(None);
        let entry_fragments = std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new_entry_accumulator(root));
        let entry_run = FormattingContextRun {
            state: entry_run_state,
            box_: root,
            callbacks,
            should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: false,
            fragments: Some(entry_fragments.clone()),
            fragmentainer: None,
        };

        let first_child = callbacks.first_child(root);
        let root_for_layout = if !first_child.is_invalid() && NodeFacts::new(&callbacks, first_child).is_svg_svg_box() {
            let mut viewport_cells = used_values::cells_before_sizing(&callbacks, root, root_constraints);
            viewport_cells.set_content_inline_size(viewport_inline_size);
            viewport_cells.set_content_block_size(viewport_block_size);
            place_child(
                &entry_run,
                root,
                PlacedChild::new(fragment_tree::FragmentGeometry {
                    cells: viewport_cells,
                    line_data: None,
                }),
                FfiCssPixelPoint::default(),
                None,
                None,
            );
            first_child
        } else {
            root
        };
        let root_cells = used_values::cells_before_sizing(&callbacks, root_for_layout, root_constraints);
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::definite(viewport_inline_size),
                block_size: AvailableSize::definite(viewport_block_size),
            },
            ContainingBlockConstraints::default(),
            ParticipationInParentFormattingContext::Root,
        )
        .with_forced_sizes(viewport_inline_size, viewport_block_size);
        let fc_type = independent_formatting_context_type(root_for_layout, &callbacks);
        let root_outcome = run_formatting_context(
            Some(&entry_fragments),
            root_cells,
            root_for_layout,
            callbacks.in_flow_containing_block(root_for_layout),
            None,
            fc_type,
            should_collect_devtools_layout_data,
            callbacks,
            input,
            None,
        );
        place_child(
            &entry_run,
            root_for_layout,
            root_outcome.placed_child(),
            FfiCssPixelPoint::default(),
            None,
            root_outcome.fragment_arrival,
        );
        finish_entry_pass(
            entry_run_state,
            &entry_fragments,
            &callbacks,
            scratch,
            should_collect_devtools_layout_data,
        )
    })
}

/// Brings the arena up to date for a stage before the stage borrows it: derives the facts its runs read, marks the
/// flex and grid items of the nodes marked for layout, and drops the cached intrinsic sizes of released slots.
fn prepare_layout_stage(state: &mut super::ArenaHandle, kind: LayoutStageKind, root: NodeSlotId) {
    let (arena, scratch) = state.arena_mut_and_layout_scratch();
    match kind {
        LayoutStageKind::Root { .. } => {
            // NB: The tree builder derives the facts of rebuilt subtrees. Unclassified invalidations
            // require deriving them for the entire tree instead.
            if arena.pending_updates_escape_partial_relayout {
                arena.derive_facts_in_subtree(root);
                arena.clear_partial_relayout_escape();
                arena.pending_attached_subtree_roots.clear();
            } else {
                // NB: Top-layer updates can attach boxes without running the tree builder.
                arena.derive_facts_after_tree_update(&[]);
            }
            arena.mark_flex_and_grid_items_of_nodes_marked_for_layout();
        }
        LayoutStageKind::Boundary { .. } => {
            // The boundary can be wider than the rebuilt roots that led to it, and laying it out may
            // re-enter intrinsic sizing for descendants outside those roots, including anonymous boxes
            // the incremental tree build created; derive the facts of the whole subtree.
            arena.derive_facts_in_subtree(root);
        }
    }
    drop_intrinsic_size_cache_entries_of_released_slots(arena, scratch);
    scratch.apply_memo_invalidations(arena);
    scratch.layout_trace.take_over_from(&mut arena.layout_trace);
    arena.begin_active_layout_pass();
}

/// Drops the intrinsic size cache entries of the slots the arena let go of, or whose cache epoch wrapped, since the
/// last stage, before any run of this one reads a cache.
fn drop_intrinsic_size_cache_entries_of_released_slots(
    arena: &mut LayoutNodeArena,
    scratch: &super::run_state::LayoutScratch,
) {
    let drops = arena.take_intrinsic_size_cache_drops();
    if !drops.is_empty() {
        scratch.intrinsic_size_caches.drop_slots(drops);
    }
}

fn finish_entry_pass(
    entry_run_state: &RunState,
    entry_fragments: &std::rc::Rc<fragment_tree::RunFragmentBuilder>,
    callbacks: &LayoutPass<'_>,
    scratch: &super::run_state::LayoutScratch,
    should_collect_devtools_layout_data: bool,
) -> LayoutStageOutput {
    abspos_engine::drain_abspos_with_placed_containing_blocks(
        entry_run_state,
        *callbacks,
        should_collect_devtools_layout_data,
        entry_fragments,
    );
    let pass_fragments = entry_fragments.take_completed_pass(callbacks);
    debug_assert!(
        !pass_fragments.roots.is_empty(),
        "an entry pass always produces the entry root's fragment"
    );
    let mut host_findings = scratch.take_host_findings_outside_runs();
    if let Some(found_by_entry_run) = entry_run_state.take_host_findings() {
        host_findings.receive(*found_by_entry_run);
    }
    LayoutStageOutput {
        fragments: pass_fragments,
        host_findings,
    }
}

/// The partial layout stage: computes the fragments of one partial relayout boundary in place,
/// without the host, in the containing block the planner found the boundary still has.
fn compute_subtree_layout_fragments(input: LayoutStageInput<'_>, containing_block: NodeSlotId) -> LayoutStageOutput {
    let LayoutStageInput {
        arena,
        scratch,
        root,
        stage,
    } = input;
    let callbacks = LayoutPass::new(arena, scratch, stage);

    // NB: Either way, the boundary root's own geometry lies in the frame of its containing block.
    let callbacks = if containing_block.is_invalid() {
        callbacks
    } else {
        callbacks.in_frame_of(frame_established_by(&callbacks, containing_block).writing_mode())
    };

    // Abspos boundaries recompute their size and position in their containing block's space.
    // In-flow SVG boundaries keep their committed geometry and lay out only their contents.
    let root_is_absolutely_positioned = NodeFacts::new(&callbacks, root).is_absolutely_positioned();
    let (entry_root, entry_root_containing_block) = if root_is_absolutely_positioned {
        (containing_block, NodeSlotId::INVALID)
    } else {
        (root, containing_block)
    };
    RunState::with_root(
        callbacks,
        entry_root,
        entry_root_containing_block,
        None,
        |entry_run_state, callbacks| {
            let _trace = scratch.layout_trace.pass(Some(root));
            let entry_fragments =
                std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new_entry_accumulator(entry_root));
            let entry_run = FormattingContextRun {
                state: entry_run_state,
                box_: entry_root,
                callbacks,
                should_collect_devtools_layout_data: false,
                treat_block_axis_percentage_insets_as_auto_beyond_root: false,
                fragments: Some(entry_fragments.clone()),
                fragmentainer: None,
            };
            if root_is_absolutely_positioned {
                abspos_engine::AbsposEngine::for_run(&entry_run).replay(&entry_run, root);
            } else {
                layout_subtree_with_frozen_root_geometry(&entry_run);
            }
            finish_entry_pass(entry_run_state, &entry_fragments, &callbacks, scratch, false)
        },
    )
}

fn layout_subtree_with_frozen_root_geometry(run: &FormattingContextRun<'_>) {
    let root = run.box_;
    let callbacks = &run.callbacks;
    let committed_root = used_values::committed_box_from_fragment_link(callbacks, root)
        .expect("partial relayout root must have committed geometry");
    let root_cells = committed_root.cells;
    let fragments = run.fragments.as_deref().expect("partial relayout must build fragments");
    let mut input = LayoutInput::new(
        AvailableSpace {
            inline_size: AvailableSize::definite(root_cells.content_inline_size),
            block_size: AvailableSize::definite(root_cells.content_block_size),
        },
        // The subtree root has definite sizes in both axes, so boxes
        // below it do not need inherited percentage constraints.
        ContainingBlockConstraints::default(),
        ParticipationInParentFormattingContext::Root,
    );
    input.sizing.root_keeps_its_committed_geometry = true;

    let facts = NodeFacts::new(callbacks, root);
    debug_assert!(facts.is_svg_svg_box());
    let fc_type = formatting_context_type_created_by_box(facts)
        .expect("partial relayout root must establish an independent formatting context");
    let root_outcome = run_formatting_context(
        Some(fragments),
        root_cells,
        root,
        callbacks.in_flow_containing_block(root),
        None,
        fc_type,
        run.should_collect_devtools_layout_data,
        *callbacks,
        input,
        None,
    );
    let ChildRunOutcome {
        laid_out_box: root_box,
        fragment_arrival,
        ..
    } = root_outcome;
    let root_geometry = fragment_tree::FragmentGeometry {
        cells: root_box.cells,
        line_data: root_box.line_data,
    };
    let facts_decided_by_parent = committed_root.facts_decided_by_parent;
    let placement = facts_decided_by_parent
        .placement
        .expect("the root of a partial relayout keeps the placement it committed");
    fragments.note_placed_box(
        root,
        placement,
        cells_kept_for_contained_abspos(run, root, || root_geometry.cells),
    );
    // The retained offset already includes relative positioning. Publish it directly instead
    // of applying placement adjustments a second time.
    let Some(fragment_tree::ChildFragmentArrival::RunRoot {
        root: unplaced_root,
        reuses_committed_subtree,
    }) = fragment_arrival
    else {
        panic!("the root of a partial relayout is laid out by a run of its own");
    };
    fragments.take_in_child_root_at_placement(root, unplaced_root, reuses_committed_subtree, callbacks);
    fragments.build_fragment_for_placed_box(
        callbacks,
        root,
        None,
        root_geometry,
        facts_decided_by_parent,
        None,
        None,
        placement.content_offset_in_containing_block,
        None,
    );
}

#[cfg(test)]
mod baseline_tests {
    use super::*;
    use crate::css::computed_values::{BoxValues, GroupPayloadRef, clone_group_payload, release_group_payload};
    use crate::css::css_enums::keyword;
    use crate::css::display::FfiDisplay;
    use crate::css::table_group_builder::group_index;
    use crate::layout::layout_node_arena::LayoutNodeArena;
    use crate::layout::node_data::{NodeConstructionFacts, NodeKind, StylePayloadsRef};
    use crate::layout::run_state::LayoutScratch;

    #[test]
    fn block_derivation_preserves_flex_last_baseline() {
        check_block_derivation(keyword::FLEX);
    }

    #[test]
    fn block_derivation_preserves_grid_last_baseline() {
        check_block_derivation(keyword::GRID);
    }

    fn check_block_derivation(display: u16) {
        let mut arena = LayoutNodeArena::new();
        let scratch = LayoutScratch::default();
        let mut child_style = FfiStylePayloads {
            groups: std::array::from_fn(|index| GroupPayloadRef::default_payload(index).as_ptr()),
        };
        let mut parent_style = child_style;
        // SAFETY: The source is the immortal default box payload. The clone is private until installed below.
        let parent_box = unsafe { clone_group_payload(group_index::BOX, parent_style.groups[group_index::BOX]) };
        // SAFETY: The clone has the BoxValues layout and no readers yet.
        unsafe { (*parent_box.cast::<BoxValues>()).display = FfiDisplay::block() };
        parent_style.groups[group_index::BOX] = parent_box;
        // SAFETY: The source is the immortal default box payload. The clone is private until installed below.
        let child_box = unsafe { clone_group_payload(group_index::BOX, child_style.groups[group_index::BOX]) };
        // SAFETY: The clone has the BoxValues layout and no readers yet.
        unsafe { (*child_box.cast::<BoxValues>()).display = FfiDisplay::from_single_keyword(display).unwrap() };
        child_style.groups[group_index::BOX] = child_box;
        let facts = NodeConstructionFacts {
            kind: NodeKind::BlockContainer,
            is_anonymous: false,
            is_html_input_element: false,
            is_html_html_element: false,
            is_document_element: false,
            is_in_user_agent_shadow_tree: false,
            uses_button_layout: false,
            is_editing_host: false,
            is_body: false,
            dom_paint_facts: 0,
            style_node: 0,
        };
        let parent = arena.allocate(facts);
        let child = arena.allocate(facts);
        arena.set_node_style(parent, 0, StylePayloadsRef::new(&parent_style));
        arena.set_node_style(child, 0, StylePayloadsRef::new(&child_style));
        arena.insert_child(parent, child, Node::INVALID);
        let callbacks = LayoutPass::new(
            &arena,
            &scratch,
            LayoutStageFacts {
                viewport_inline_size: CssPixels::from_integer(800),
                viewport_block_size: CssPixels::from_integer(600),
                document_in_quirks_mode: false,
            },
        );
        RunState::with_root(callbacks, parent, Node::INVALID, None, |_, callbacks| {
            let child_cells = used_values::RootInputCells::default();
            assert!(child_gives_container_baselines(&callbacks, parent, child));
            let mut baselines = DerivedBaselines::default();
            baselines.fold_in_later_child(baselines_laid_out_child_gives_container(
                &callbacks,
                LaidOutChild {
                    node: child,
                    metrics: &child_cells,
                    content_baselines: DerivedBaselines {
                        first: Some(CssPixels::from_integer(10)),
                        last: Some(CssPixels::from_integer(30)),
                        inline_alignment: None,
                    },
                    content_offset: FfiCssPixelPoint::default(),
                },
            ));
            assert_eq!(baselines.first, Some(CssPixels::from_integer(10)));
            assert_eq!(baselines.last, Some(CssPixels::from_integer(30)));
            assert_eq!(baselines.inline_alignment, Some(CssPixels::from_integer(10)));
        });
        arena
            .free_subtree(parent)
            .destroy_shells_and_invoke_callbacks(&crate::stage::MainThread::for_test());
        release_group_payload(group_index::BOX, child_box);
        release_group_payload(group_index::BOX, parent_box);
    }
}
