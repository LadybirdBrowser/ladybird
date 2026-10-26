/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

pub(super) const CALC_NUMERIC_KIND_LENGTH: u8 = 4;

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum LayoutMode {
    // Normal layout. No min-content or max-content constraints applied.
    Normal,

    // Intrinsic size determination. Boxes honor min-content and max-content
    // constraints stored in used values by considering their containing block
    // to be zero-sized or infinitely large in the relevant axis.
    // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizing
    IntrinsicSizing,
}

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
    pub(crate) fn override_for(&self, field: style_values::InsetField) -> Option<style_values::ResolvedInsetOverride> {
        match field {
            style_values::InsetField::Top => self.top,
            style_values::InsetField::Right => self.right,
            style_values::InsetField::Bottom => self.bottom,
            style_values::InsetField::Left => self.left,
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

#[derive(Clone, Copy, Debug)]
pub(crate) struct PixelFraction {
    pub(crate) numerator: CssPixels,
    pub(crate) denominator: CssPixels,
}

#[derive(Clone, Copy, Debug, Default)]
pub(super) struct ReplacedIntrinsicSize {
    pub(super) width: Option<CssPixels>,
    pub(super) height: Option<CssPixels>,
    pub(super) aspect_ratio: Option<PixelFraction>,
}

#[derive(Clone, Copy, Debug, Default)]
pub(super) struct ReplacedMaxContentSizeConstraints {
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

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum LayoutPurpose {
    Commit,
    Measurement,
    IntrinsicInlineMeasurement,
}

impl LayoutPurpose {
    pub(crate) fn is_measurement(self) -> bool {
        matches!(self, Self::Measurement | Self::IntrinsicInlineMeasurement)
    }
}

pub(crate) struct MeasurementState<'pass> {
    callbacks: LayoutPass<'pass>,
}

impl<'pass> MeasurementState<'pass> {
    pub(crate) fn create(callbacks: LayoutPass<'pass>) -> Self {
        Self { callbacks }
    }

    pub(super) fn run(&self, node: Node, node_used: &UsedValues, input: LayoutInput) -> ChildLayoutResult {
        self.run_with_layout_mode(node, node_used, LayoutMode::IntrinsicSizing, input)
    }

    pub(crate) fn run_with_layout_mode(
        &self,
        node: Node,
        node_used: &UsedValues,
        layout_mode: LayoutMode,
        input: LayoutInput,
    ) -> ChildLayoutResult {
        self.run_with_purpose(LayoutPurpose::Measurement, node, node_used, layout_mode, input)
    }

    pub(crate) fn run_with_purpose(
        &self,
        purpose: LayoutPurpose,
        node: Node,
        node_used: &UsedValues,
        layout_mode: LayoutMode,
        input: LayoutInput,
    ) -> ChildLayoutResult {
        let fc_type = independent_formatting_context_type(node, &self.callbacks);
        let root_containing_block = self
            .callbacks
            .containing_block_for_child_run(node, &input.participation);
        run_formatting_context(
            purpose,
            None,
            node_used,
            node,
            root_containing_block,
            None,
            fc_type,
            layout_mode,
            false,
            self.callbacks,
            input,
            None,
            None,
        )
    }

    pub(crate) fn callbacks(&self) -> &LayoutPass<'_> {
        &self.callbacks
    }

    pub(crate) fn create_used_values(&self, node: Node, constraints: ContainingBlockConstraints) -> UsedValues {
        used_values::create_used_values(&self.callbacks, node, constraints)
    }
}

pub(crate) fn run_fragmentainer(
    outer: &FormattingContextRun,
    purpose: LayoutPurpose,
    input: LayoutInput,
    fragmentainer: std::rc::Rc<fragmentation::FragmentainerContext>,
) -> RunOutputs {
    debug_assert_eq!(input.participation, ParticipationInParentFormattingContext::Root);
    let box_ = outer.box_;
    let root_containing_block = outer
        .callbacks
        .containing_block_for_child_run(box_, &input.participation);
    execute_formatting_context_run(
        purpose,
        used_values::UsedValuesCellState::capture(outer.records.used_values(box_)),
        box_,
        root_containing_block,
        None,
        FormattingContextType::Block,
        outer.layout_mode,
        outer.should_collect_devtools_layout_data,
        outer.callbacks,
        input,
        None,
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
/// block's writing mode.
pub(crate) fn lay_out_pending_abspos_children(
    run: &FormattingContextRun,
    batch: Vec<abspos_inputs::PendingAbsposChild>,
) {
    let engine = abspos_engine::AbsposEngine::for_run(run);
    for entry in batch {
        let containing_block = entry.containing_block();
        if frame_established_by(&run.callbacks, containing_block) == run.callbacks.frame {
            engine.layout_pending_child(run, entry);
            continue;
        }
        in_frame_established_by(run, containing_block, |run| {
            abspos_engine::AbsposEngine::for_run(run).layout_pending_child(run, entry);
        });
    }
}

/// Runs `body` with a run in the frame `node` establishes, with the record of `node` moved into that frame.
pub(crate) fn in_frame_established_by<R>(
    run: &FormattingContextRun,
    node: Node,
    body: impl FnOnce(&FormattingContextRun) -> R,
) -> R {
    let parent_frame = run.callbacks.frame;
    let own_frame = frame_established_by(&run.callbacks, node);
    if own_frame == parent_frame {
        return body(run);
    }
    let used = run.records.used_values_if_owned(node);
    if let Some(used) = used {
        used.move_box_metrics_to_frame(parent_frame, own_frame);
    }
    let framed_run = run.in_frame_of(own_frame);
    let result = body(&framed_run);
    if let Some(used) = used {
        used.move_box_metrics_to_frame(own_frame, parent_frame);
    }
    result
}

pub(crate) fn place_child(
    run: &FormattingContextRun,
    node: Node,
    offset: FfiCssPixelPoint,
    containing_line_box_fragment: Option<used_values::LineBoxFragmentCoordinate>,
) -> Option<fragment_tree::PlacedFragment> {
    let containing_block = run.callbacks.in_flow_containing_block(node);
    place_child_in_containing_block(run, node, containing_block, offset, containing_line_box_fragment)
}

pub(crate) fn place_child_in_containing_block(
    run: &FormattingContextRun,
    node: Node,
    containing_block: Node,
    offset: FfiCssPixelPoint,
    containing_line_box_fragment: Option<used_values::LineBoxFragmentCoordinate>,
) -> Option<fragment_tree::PlacedFragment> {
    let records = run.records;
    let callbacks = &run.callbacks;
    let used = records.used_values(node);
    set_placement(used, containing_block, offset);
    let fragments = run.fragments.as_deref()?;
    if let Some((fragment, reuses_subtree)) = fragments.take_reused_committed_fragment(callbacks, node, used) {
        fragments.attach_committed_fragment(
            node,
            fragment.clone(),
            reuses_subtree,
            (!containing_block.is_invalid()).then_some(containing_block),
            used,
            containing_block_is_sealed(records, containing_block),
            resolve_containing_line_box_index(records, callbacks, node, containing_block, containing_line_box_fragment),
            point_add(
                offset,
                committed_offset_delta_at_placement(run.purpose, records, callbacks, node, containing_block, used),
            ),
            callbacks.frame,
        );
        return Some(fragment_tree::PlacedFragment {
            fragment,
            exports_payloads: false,
        });
    }
    fragments.normalize_arrivals_for_placement(node, callbacks);
    loop {
        let batch = fragments.take_drainable_abspos(node, records, callbacks);
        if batch.is_empty() {
            break;
        }
        lay_out_pending_abspos_children(run, batch);
    }
    let node_facts = NodeFacts::new(callbacks, node);
    let own_anchor_candidate_border_box_rect = (node_facts.is_box() && node_facts.has_anchor_names()).then(|| {
        let collapsed = used.uses_collapsing_borders_model.get();
        CssPixelRect {
            x: used.content_offset.get().x - used.border_box_left(collapsed),
            y: used.content_offset.get().y - used.border_box_top(collapsed),
            width: used.border_box_inline_size(collapsed),
            height: used.border_box_block_size(collapsed),
        }
    });
    Some(fragments.build_fragment_for_placed_box(
        callbacks,
        node,
        (!containing_block.is_invalid()).then_some(containing_block),
        used,
        containing_block_is_sealed(records, containing_block),
        resolve_containing_line_box_index(records, callbacks, node, containing_block, containing_line_box_fragment),
        point_add(
            offset,
            committed_offset_delta_at_placement(run.purpose, records, callbacks, node, containing_block, used),
        ),
        own_anchor_candidate_border_box_rect,
    ))
}

/// Places an in-flow child whose committed subtree is reused as it is, with the committed fragment it was laid out
/// to. The child's record must already hold what its layout left in it.
pub(crate) fn place_reused_child(
    run: &FormattingContextRun,
    node: Node,
    offset: FfiCssPixelPoint,
    fragment: std::sync::Arc<fragment_tree::Fragment>,
) {
    let records = run.records;
    let callbacks = &run.callbacks;
    let containing_block = callbacks.in_flow_containing_block(node);
    let used = records.used_values(node);
    set_placement(used, containing_block, offset);
    let fragments = run
        .fragments
        .as_deref()
        .expect("a reused subtree is placed by a run that builds fragments");
    fragments.attach_committed_fragment(
        node,
        fragment,
        true,
        (!containing_block.is_invalid()).then_some(containing_block),
        used,
        containing_block_is_sealed(records, containing_block),
        None,
        point_add(
            offset,
            committed_offset_delta_at_placement(run.purpose, records, callbacks, node, containing_block, used),
        ),
        callbacks.frame,
    );
}

fn set_placement(used: &UsedValues, containing_block: Node, offset: FfiCssPixelPoint) {
    assert!(!used.has_content_offset.get());
    used.has_content_offset.set(true);
    used.content_offset.set(offset);
    used.placed_in.set(containing_block);
    used.seal_committed_box_metrics();
}

fn containing_block_is_sealed(records: &RunRecords, containing_block: Node) -> bool {
    !containing_block.is_invalid()
        && records
            .used_values_if_owned(containing_block)
            .is_none_or(|containing_block_used| containing_block_used.has_content_offset.get())
}

/// The line box index to record for atomic inlines whose containing line
/// survived line post-processing.
fn resolve_containing_line_box_index(
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
    node: Node,
    containing_block: Node,
    coordinate: Option<used_values::LineBoxFragmentCoordinate>,
) -> Option<usize> {
    let coordinate = coordinate?;
    if !NodeFacts::new(callbacks, node).is_non_fragmented_box() {
        return None;
    }
    assert!(!containing_block.is_invalid());
    let containing_block_used = records.used_values(containing_block);
    let data = containing_block_used.line_data_ref()?;
    let line = data.building().line_boxes.get(coordinate.line_box_index)?;
    // The placed offset is not the fragment's offset: atomic inlines are placed shifted by their inline ancestors'
    // relative insets, which the line data only folds in after placement. An interrupting block is placed before
    // its line gets its fragment.
    debug_assert!(
        line.fragments
            .get(coordinate.fragment_index)
            .is_none_or(|fragment| fragment.layout_node == node),
        "a line box fragment coordinate must name the placed box's own fragment"
    );
    Some(coordinate.line_box_index)
}

fn committed_offset_delta_at_placement(
    purpose: LayoutPurpose,
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
    node: Node,
    containing_block: Node,
    used: &UsedValues,
) -> FfiCssPixelPoint {
    let mut delta = FfiCssPixelPoint::default();
    if purpose.is_measurement() {
        return delta;
    }
    let facts = NodeFacts::new(callbacks, node);
    if !facts.is_non_fragmented_box() {
        return delta;
    }
    if facts.is_relatively_positioned() {
        delta.x += used.inset_left.get();
        delta.y += used.inset_top.get();
    }
    if facts.is_in_flow() && facts.display().is_block_outside() {
        let chain = inline_formatting_context::accumulated_relative_insets_from_inline_ancestor_chain(
            records,
            callbacks,
            callbacks.parent(node),
            containing_block,
        );
        if chain.found_fragmented_inline_node {
            delta.x += chain.offset_x;
            delta.y += chain.offset_y;
        }
    }
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

pub(crate) fn placement_containing_block(records: &RunRecords, callbacks: &LayoutPass<'_>, node: Node) -> Node {
    let placed_in = records
        .used_values_if_owned(node)
        .filter(|used| used.has_content_offset.get())
        .map_or(NodeSlotId::INVALID, |used| used.placed_in.get());
    if !placed_in.is_invalid() {
        return placed_in;
    }
    let facts = NodeFacts::new(callbacks, node);
    if !facts.is_absolutely_positioned() {
        return callbacks.in_flow_containing_block(node);
    }
    let mut search = abspos_inputs::ContainingBlockSearch::starting_at(node, facts.is_fixed_position());
    callbacks
        .arena()
        .continue_containing_block_search(&mut search, records.root());
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

pub(crate) fn box_baseline(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    used: &UsedValues,
    baseline_set: BaselineSet,
) -> CssPixels {
    box_baseline_with_content_baselines(callbacks, box_, used, baseline_set, used.content_baselines_from_cells())
}

pub(crate) fn box_baseline_with_content_baselines(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    used: &UsedValues,
    baseline_set: BaselineSet,
    content_baselines: DerivedBaselines,
) -> CssPixels {
    let facts = NodeFacts::new(callbacks, box_);
    let style = StyleValues::for_node(callbacks, box_);
    let collapsed = used.uses_collapsing_borders_model.get();

    // https://drafts.csswg.org/css2/#propdef-vertical-align
    if facts.vertical_align_applies() && style.vertical_align_is_keyword() {
        match style.vertical_align_keyword() {
            vertical_align::TOP => {
                // Top: Align the top of the aligned subtree with the top of the line box.
                return used.border_box_top(collapsed);
            }
            vertical_align::MIDDLE => {
                // Middle: Align the vertical midpoint of the box with the baseline of the parent box plus half the x-height of the parent.
                let containing_block = callbacks.in_flow_containing_block(box_);
                assert!(!containing_block.is_invalid());
                let containing_style = StyleValues::for_node(callbacks, containing_block);
                return used.margin_box_block_size(collapsed) / 2
                    + CssPixels::nearest_value_for_f32(containing_style.font_x_height() / 2.0);
            }
            vertical_align::_LIBWEB_BASELINE_MIDDLE => {
                // NB: The vertical midpoint of the box is aligned with the baseline of the parent box.
                return used.margin_box_block_size(collapsed) / 2;
            }
            vertical_align::BOTTOM => {
                // Bottom: Align the bottom of the aligned subtree with the bottom of the line box.
                return used.content_block_size.get() + used.margin_box_top(collapsed);
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
                return used.margin_box_block_size(collapsed)
                    - CssPixels::nearest_value_for_f32(containing_style.font_descent());
            }
            _ => {}
        }
    }

    // INTEROP: A range input's baseline is its bottom border edge in Chrome, whatever its internal layout.
    if facts.is_range_input_box() {
        return used.margin_box_top(collapsed) + used.content_block_size.get() + used.border_box_bottom(collapsed);
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
    let derive_baseline_from_content =
        baseline_set == BaselineSet::First || is_flex_or_grid_container || !facts.is_scroll_container();

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
        return used.margin_box_top(collapsed) + content_baseline;
    }

    // AD-HOC: Inline buttons without a content baseline synthesize one from their content-box bottom edge in
    //         Chromium and Firefox, rather than the margin-box edge used by ordinary inline-blocks.
    if facts.uses_button_layout() && display.is_inline_outside() && content_baseline.is_none() {
        return used.margin_box_top(collapsed) + used.content_block_size.get();
    }

    // If the box has no baseline set, the bottom margin edge of the box is used.
    used.margin_box_block_size(collapsed)
}

#[derive(Clone, Copy, PartialEq, Eq)]
pub(crate) enum BaselinePurpose {
    Container,
    InlineAlignment,
}

pub(crate) fn inline_box_baseline(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    used: &UsedValues,
    content_baselines: DerivedBaselines,
) -> CssPixels {
    box_baseline_for_purpose(
        callbacks,
        box_,
        used,
        BaselineSet::Last,
        content_baselines,
        BaselinePurpose::InlineAlignment,
    )
}

fn box_baseline_for_purpose(
    callbacks: &LayoutPass<'_>,
    box_: Node,
    used: &UsedValues,
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
        if flex_or_grid_baseline || inline_table_baseline {
            baseline_set = BaselineSet::First;
        } else {
            content_baselines.last = content_baselines.inline_alignment.or(content_baselines.last);
        }
    }
    box_baseline_with_content_baselines(callbacks, box_, used, baseline_set, content_baselines)
}

/// First and last baseline sets derived for one box, relative to its content
/// box. A formatting context derives its own root box's baselines during its
/// run and records them on the instance; run_formatting_context, the side
/// that ran the context, stores them into the root's used values. Interior
/// boxes keep the derive-and-store shortcut.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct DerivedBaselines {
    pub(crate) first: Option<CssPixels>,
    pub(crate) last: Option<CssPixels>,
    // NB: Inline alignment can export a descendant flex/grid first baseline through block containers while
    //     ordinary container derivation must retain the distinct last baseline set.
    pub(crate) inline_alignment: Option<CssPixels>,
}

// NOTE: A box can be baselined more than once (e.g. table cells are laid out
//       twice), so both presence bits are re-assigned on every store. The
//       payload cells are left untouched for an absent baseline so that
//       resetting the presence bits does not perturb payloads observed by the
//       existing derivation flow.
pub(crate) fn store_derived_baselines(used: &UsedValues, baselines: DerivedBaselines) {
    used.inline_alignment_baseline.set(baselines.inline_alignment);
    used.has_first_baseline.set(baselines.first.is_some());
    if let Some(value) = baselines.first {
        used.first_baseline.set(value);
    }
    used.has_last_baseline.set(baselines.last.is_some());
    if let Some(value) = baselines.last {
        used.last_baseline.set(value);
    }
}

pub(crate) fn derive_baselines(records: &RunRecords, callbacks: &LayoutPass<'_>, box_: Node) -> DerivedBaselines {
    let facts = NodeFacts::new(callbacks, box_);
    let own_used = records.used_values(box_);
    let own_line_data = own_used.line_data_ref();
    let mut lines = own_line_data.iter().flat_map(|data| data.lines());
    let first = lines
        .find(|line| !line.is_empty)
        .or_else(|| own_line_data.as_ref()?.lines().next());
    if let Some(first) = first {
        let last = lines.rfind(|line| !line.is_empty).unwrap_or(first);
        let baseline_for_line_box =
            |line: inline_content::LineRecord, baseline_set: BaselineSet, purpose: BaselinePurpose| -> CssPixels {
                if !line.has_block_level_box {
                    return line.physical_vertical_end() - line.block_length + line.baseline;
                }
                assert_eq!(line.layout_fragment_count, 1);
                let fragment_node = line
                    .first_fragment_node
                    .expect("block-level line must have one fragment");
                let block_child_state = records.used_values(fragment_node);
                let child_offset_from_margin_edge = block_child_state.content_offset.get().y
                    - block_child_state.margin_box_top(block_child_state.uses_collapsing_borders_model.get());
                child_offset_from_margin_edge
                    + box_baseline_for_purpose(
                        callbacks,
                        fragment_node,
                        block_child_state,
                        baseline_set,
                        block_child_state.content_baselines_from_cells(),
                        purpose,
                    )
            };
        return DerivedBaselines {
            first: Some(baseline_for_line_box(
                first,
                BaselineSet::First,
                BaselinePurpose::Container,
            )),
            last: Some(baseline_for_line_box(
                last,
                BaselineSet::Last,
                BaselinePurpose::Container,
            )),
            inline_alignment: Some(baseline_for_line_box(
                last,
                BaselineSet::Last,
                BaselinePurpose::InlineAlignment,
            )),
        };
    }

    if callbacks.first_child(box_).is_invalid() || facts.children_are_inline() {
        return DerivedBaselines::default();
    }

    // Derive baselines from the first/last in-flow child that has a baseline set of its own.
    // NB: Flex and grid containers derive their baselines from their items instead.
    let container_display = facts.display();
    let baseline_from_children = |baseline_set: BaselineSet, purpose: BaselinePurpose| -> Option<CssPixels> {
        let mut next_child = match baseline_set {
            BaselineSet::First => callbacks.first_child(box_),
            BaselineSet::Last => callbacks.last_child(box_),
        };
        while !next_child.is_invalid() {
            let child = next_child;
            next_child = match baseline_set {
                BaselineSet::First => callbacks.next_sibling(child),
                BaselineSet::Last => callbacks.previous_sibling(child),
            };
            let child_facts = NodeFacts::new(callbacks, child);
            if !child_facts.is_flow_layout_participant() || child_facts.is_floating() {
                continue;
            }
            if facts.is_fieldset_box() && facts.rendered_legend() == child {
                continue;
            }
            if !table_formatting_context::child_participates_in_table_run(container_display, &child_facts) {
                continue;
            }
            // A table wrapper box has the baselines of its table box, not of a caption above it: "The baseline of an
            // 'inline-table' is the baseline of the first row of the table."
            // https://www.w3.org/TR/CSS22/tables.html#height-layout
            if facts.is_table_wrapper() && !child_facts.display().is_table_inside() {
                continue;
            }
            if let Some(baseline) = baseline_of_child_for_purpose(
                records,
                callbacks,
                child,
                baseline_set,
                ChildBaselineSource::Own,
                purpose,
            ) {
                return Some(baseline);
            }
        }
        None
    };
    DerivedBaselines {
        first: baseline_from_children(BaselineSet::First, BaselinePurpose::Container),
        last: baseline_from_children(BaselineSet::Last, BaselinePurpose::Container),
        inline_alignment: baseline_from_children(BaselineSet::Last, BaselinePurpose::InlineAlignment),
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum ChildBaselineSource {
    Own,
    OwnOrSynthesized,
    Synthesized,
}

/// The baseline of a laid out child, relative to its container's margin edge. A synthesized baseline comes from the
/// child's border box.
pub(crate) fn baseline_of_child(
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
    child: Node,
    baseline_set: BaselineSet,
    source: ChildBaselineSource,
) -> Option<CssPixels> {
    baseline_of_child_for_purpose(
        records,
        callbacks,
        child,
        baseline_set,
        source,
        BaselinePurpose::Container,
    )
}

pub(crate) fn baseline_of_child_for_purpose(
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
    child: Node,
    baseline_set: BaselineSet,
    source: ChildBaselineSource,
    purpose: BaselinePurpose,
) -> Option<CssPixels> {
    let child_state = records.used_values_if_owned(child)?;
    let has_baseline = match baseline_set {
        BaselineSet::First => child_state.has_first_baseline.get(),
        BaselineSet::Last => child_state.has_last_baseline.get(),
    };
    let collapsed = child_state.uses_collapsing_borders_model.get();
    if !has_baseline || source == ChildBaselineSource::Synthesized {
        if source == ChildBaselineSource::Own {
            return None;
        }
        // https://drafts.csswg.org/css-align-3/#synthesize-baseline
        // The alphabetic baseline is synthesized from the line-under edge of the border box.
        return Some(
            child_state.content_offset.get().y
                + child_state.content_block_size.get()
                + child_state.border_box_bottom(collapsed),
        );
    }
    let child_offset_from_margin_edge = child_state.content_offset.get().y - child_state.margin_box_top(collapsed);
    Some(
        child_offset_from_margin_edge
            + box_baseline_for_purpose(
                callbacks,
                child,
                child_state,
                baseline_set,
                child_state.content_baselines_from_cells(),
                purpose,
            ),
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
    InternalDummy,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct BorderData {
    pub color: u32,
    pub line_style: u8,
    pub width: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ChildLayoutResult {
    pub omitted_line_layout: bool,
    pub automatic_content_inline_size: CssPixels,
    pub min_content_inline_size_from_max_content_layout: Option<CssPixels>,
    pub automatic_content_block_size: CssPixels,
    pub automatic_line_clamp_max_lines: Option<usize>,
    pub baselines: DerivedBaselines,
    pub table_box_in_wrapper_border_box_block_size: Option<CssPixels>,
    /// For a box with a preferred aspect ratio, the block size of its laid out content.
    pub content_block_size_for_aspect_ratio_minimum: Option<CssPixels>,
    pub depends_on_percentage_block_size: bool,
}

#[derive(Clone)]
pub(crate) struct RunRootOutcome {
    pub(super) cells: used_values::UsedValuesCellState,
    pub(super) own_metrics_sealed: bool,
    pub(super) line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    /// Boxed, as most roots have none and a cached run holds its outcome for as long as it is reusable.
    pub(super) rare: Option<Box<used_values::UsedValuesRareData>>,
}

impl RunRootOutcome {
    fn apply_to_record(self, record: &UsedValues) {
        self.cells.apply_to_record(record);
        if let Some(line_data) = self.line_data {
            record.set_finished_line_data(line_data);
        }
        if let Some(rare) = self.rare {
            (*rare).install_present_payloads_into(record);
        }
        if self.own_metrics_sealed {
            record.seal_own_metrics();
        }
    }
}

#[derive(Clone)]
pub(crate) struct RunOutputs {
    pub(crate) result: ChildLayoutResult,
    pub(crate) root: Option<fragment_tree::UnplacedRootFragment>,
    pub(crate) root_outcome: RunRootOutcome,
    pub(crate) atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: Option<CssPixels>,
    /// Boxed, as most runs find out nothing for the host.
    pub(crate) host_findings: Option<Box<super::run_records::HostFindings>>,
}

pub(crate) enum ChildLayoutOutcome {
    Skipped,
    Created(ChildLayoutResult),
    ReenterCurrent,
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

/// The document-side answers every layout pass needs, registered once per arena. Each callback
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
    /// What a container-relative length on the element with the given identity resolves against.
    container_length_bases: extern "C" fn(*mut c_void, u32) -> svg_formatting_context::FfiContainerLengthBases,
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

    /// The one question a layout pass may ask the document while it runs.
    pub(crate) fn container_length_bases_query(
        &self,
        _: &crate::stage::MainThread,
    ) -> layout_pass::ContainerLengthBasesQuery {
        layout_pass::ContainerLengthBasesQuery::new(self.context, self.container_length_bases)
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
    pub(crate) purpose: LayoutPurpose,
    pub(crate) records: &'pass RunRecords<'pass>,
    pub(crate) box_: Node,
    pub(crate) layout_mode: LayoutMode,
    pub(crate) callbacks: LayoutPass<'pass>,
    pub(crate) should_collect_devtools_layout_data: bool,
    pub(crate) treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    pub(crate) fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
    pub(crate) previous_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    pub(crate) fragmentainer: Option<std::rc::Rc<fragmentation::FragmentainerContext>>,
}

impl<'pass> FormattingContextRun<'pass> {
    /// This run, laying out in `frame`.
    pub(crate) fn in_frame_of(&self, frame: geometry::Frame) -> Self {
        Self {
            purpose: self.purpose,
            records: self.records,
            box_: self.box_,
            layout_mode: self.layout_mode,
            callbacks: self.callbacks.in_frame_of(frame.writing_mode()),
            should_collect_devtools_layout_data: self.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: self
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            fragments: self.fragments.clone(),
            previous_line_data: self.previous_line_data.clone(),
            fragmentainer: self.fragmentainer.clone(),
        }
    }

    #[inline]
    pub(crate) fn sizing(&self) -> sizing_context::SizingContext<'pass> {
        sizing_context::SizingContext::new(self.purpose, self.records, self.callbacks)
    }

    pub(crate) fn outputs(
        &self,
        result: ChildLayoutResult,
        root: Option<fragment_tree::UnplacedRootFragment>,
        atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: Option<CssPixels>,
    ) -> RunOutputs {
        let record = self.records.used_values(self.box_);
        let root_outcome = RunRootOutcome {
            cells: used_values::UsedValuesCellState::capture(record),
            own_metrics_sealed: record.own_metrics_are_sealed(),
            line_data: record.finish_line_data(&self.callbacks),
            rare: record.rare_data.get().map(|rare| Box::new(rare.take())),
        };
        RunOutputs {
            result,
            root,
            root_outcome,
            atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
            host_findings: self.records.take_host_findings(),
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
    InternalDummy,
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
    if node_facts::node_creates_block_formatting_context(
        data,
        style,
        parent_is_flex_or_grid_container,
        parent_writing_mode,
    ) {
        return Some(FormattingContextType::Block);
    }
    if node_facts::has_flag(data, NodeFlag::ChildrenAreInline)
        || display.is_some_and(|display| {
            display.is_table_column()
                || display.is_table_column_group()
                || display.is_table_row()
                || display.is_table_row_group()
                || display.is_table_header_group()
                || display.is_table_footer_group()
        })
    {
        return None;
    }
    if !display.is_some_and(|display| display.is_flow_inside()) {
        return Some(FormattingContextType::InternalDummy);
    }
    None
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
    parent_grid: Option<&grid_formatting_context::GridFormattingContext<'pass>>,
    fc_type: FormattingContextType,
) -> FormattingContextImplementation<'pass> {
    match fc_type {
        FormattingContextType::Block => {
            FormattingContextImplementation::Block(Box::new(block_formatting_context::BlockFormattingContext::new(run)))
        }
        FormattingContextType::Flex => {
            FormattingContextImplementation::Flex(Box::new(flex_formatting_context::FlexFormattingContext::new(run)))
        }
        FormattingContextType::Grid => FormattingContextImplementation::Grid(Box::new(
            grid_formatting_context::GridFormattingContext::new(run, parent_grid),
        )),
        FormattingContextType::Table => {
            FormattingContextImplementation::Table(Box::new(table_formatting_context::TableFormattingContext::new(run)))
        }
        FormattingContextType::Svg => {
            FormattingContextImplementation::Svg(Box::new(svg_formatting_context::SvgFormattingContext::new(run)))
        }
        FormattingContextType::Math => {
            FormattingContextImplementation::Math(Box::new(math_formatting_context::MathFormattingContext::new(run)))
        }
        FormattingContextType::ReplacedWithChildren => FormattingContextImplementation::ReplacedWithChildren,
        FormattingContextType::InternalReplaced => FormattingContextImplementation::InternalReplaced,
        FormattingContextType::InternalDummy => FormattingContextImplementation::InternalDummy,
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
    purpose: LayoutPurpose,
    records: &RunRecords<'_>,
    callbacks: &LayoutPass<'_>,
    node: Node,
    available_inner_space: AvailableSpace,
    constraints: ContainingBlockConstraints,
    automatic_content_block_size_of_completed_run: Option<CssPixels>,
) -> CssPixels {
    let facts = NodeFacts::new(callbacks, node);
    if facts.creates_block_formatting_context() {
        return automatic_content_block_size_of_completed_run.unwrap_or_default();
    }
    let style = StyleValues::for_node(callbacks, node);
    // https://w3c.github.io/mathml-core/#layout-algorithms
    // After calculating the line-ascent and line-descent of a math content box, its block size is set to their sum.
    if style.display().is_math_inside() {
        return automatic_content_block_size_of_completed_run.unwrap_or_default();
    }
    if style.display().is_flex_inside()
        && let Some(automatic_content_block_size_reported_by_completed_run) =
            automatic_content_block_size_of_completed_run
    {
        return automatic_content_block_size_reported_by_completed_run;
    }
    if style.display().is_flex_inside() || style.display().is_grid_inside() || style.display().is_table_inside() {
        // The automatic block size of a flex, grid, or table container is its
        // max-content size.
        // https://drafts.csswg.org/css-flexbox-1/#algo-main-container
        // https://www.w3.org/TR/css-grid-2/#intrinsic-sizes
        return sizing_context::SizingContext::new(purpose, records, *callbacks).calculate_max_content_block_size(
            node,
            available_inner_space.inline_size.to_px_or_zero(),
            constraints,
        );
    }
    debug_assert!(false, "independent formatting context root of unexpected kind");
    CssPixels::default()
}

fn flex_self_block_size_resolution_space(
    child_run_builds_fragments: bool,
    sizing: &sizing_context::SizingContext,
    node: Node,
    resolution_space: AvailableSpace,
    constraints: ContainingBlockConstraints,
) -> Option<AvailableSpace> {
    if !child_run_builds_fragments {
        return None;
    }
    let style = sizing.style(node);
    if !style.display().is_flex_inside() || !crate::layout::sizing_context::is_plain_auto(style.min_height()) {
        return None;
    }
    if sizing.facts(node).document_in_quirks_mode() {
        return None;
    }
    if !sizing.should_treat_block_size_as_auto(node, resolution_space, constraints)
        || sizing.box_is_sized_as_replaced_element(node, resolution_space, constraints)
    {
        return None;
    }
    Some(resolution_space)
}

// Flex containers with an automatic block size are treated as max-content, so resolve it early.
fn eagerly_resolve_atomic_flex_root_auto_block_size(
    run: &FormattingContextRun,
    input: &LayoutInput,
    inline_definite_space: AvailableSpace,
) {
    let node = run.box_;
    let sizing = run.sizing();
    if !StyleValues::for_node(&run.callbacks, node).display().is_flex_inside()
        || sizing.box_is_sized_as_replaced_element(node, input.available_space, input.containing_block_constraints)
    {
        return;
    }
    sizing.resolve_used_block_size_if_treated_as_auto(
        node,
        inline_definite_space,
        input.containing_block_constraints,
        None,
        || {
            independent_root_automatic_block_size(
                run.purpose,
                run.records,
                &run.callbacks,
                node,
                run.records
                    .used_values(node)
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
    input: &LayoutInput,
    fc_type: FormattingContextType,
) -> RootSizingOutcome {
    let mut atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above = None;
    let body_input = match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => dimension_block_level_root(run, input, fc_type),
        ParticipationInParentFormattingContext::Float => body_input_with_inner_available_space(run, input),
        ParticipationInParentFormattingContext::AtomicInline => {
            let run_cache_may_store_this_block_formatting_context_run = fc_type == FormattingContextType::Block
                && run.layout_mode == LayoutMode::Normal
                && !run.purpose.is_measurement();
            atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above = run.sizing().dimension_atomic_root(
                run.box_,
                input.available_space,
                input.containing_block_constraints,
                run.layout_mode,
                run_cache_may_store_this_block_formatting_context_run,
            );
            let mut body_input = body_input_with_inner_available_space(run, input);
            let inline_definite_space = AvailableSpace {
                inline_size: AvailableSize::definite(run.records.used_values(run.box_).content_inline_size.get()),
                block_size: AvailableSize::Indefinite,
            };
            let flex_self_resolution_space = flex_self_block_size_resolution_space(
                run.fragments.is_some(),
                &run.sizing(),
                run.box_,
                inline_definite_space,
                input.containing_block_constraints,
            );
            if flex_self_resolution_space.is_none() {
                eagerly_resolve_atomic_flex_root_auto_block_size(run, input, inline_definite_space);
            }
            body_input.sizing.flex_self_block_size_resolution_space = flex_self_resolution_space;
            body_input
        }
        ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
            abspos_engine::AbsposEngine::for_run(run).dimension_out_of_flow_root(run.box_, abspos_inputs);
            body_input_with_inner_available_space(run, input)
        }
        ParticipationInParentFormattingContext::Item => {
            if cfg!(debug_assertions) && run.layout_mode == LayoutMode::Normal && !run.purpose.is_measurement() {
                debug_assert!(
                    run.records.used_values(run.box_).has_definite_inline_size.get(),
                    "container-internal run root must arrive with a container-assigned inline size"
                );
            }
            *input
        }
        ParticipationInParentFormattingContext::Root => {
            let directives = input.sizing;
            if directives.forced_content_inline_size.is_some() || directives.forced_content_block_size.is_some() {
                let used = run.records.used_values(run.box_);
                if let Some(inline_size) = directives.forced_content_inline_size {
                    used.set_content_inline_size(inline_size);
                }
                if let Some(block_size) = directives.forced_content_block_size {
                    used.set_content_block_size(block_size);
                    used.has_definite_block_size.set(true);
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
    input: &LayoutInput,
    fc_type: FormattingContextType,
) -> LayoutInput {
    let node = run.box_;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let sizing = run.sizing();
    let style = StyleValues::for_node(&run.callbacks, node);
    let mut body_input = body_input_with_inner_available_space(run, input);
    if let Some(block_size) = input.sizing.forced_content_block_size {
        let used = run.records.used_values(node);
        used.set_content_block_size(block_size);
        used.has_definite_block_size.set(true);
        body_input.available_space.block_size = AvailableSize::definite(block_size);
        return body_input;
    }
    let mut measured_content_block_size = None;
    if sizing.should_treat_block_size_as_auto(node, available_space, constraints)
        && !crate::layout::sizing_context::is_plain_auto(style.min_height())
        && lays_out_content_against_min_block_size(NodeFacts::new(&run.callbacks, node), fc_type)
    {
        let content_block_size =
            sizing.measure_automatic_content_block_size(node, run.layout_mode, body_input.available_space, constraints);
        measured_content_block_size = Some(content_block_size);
        let min_block_size = sizing.calculate_inner_block_size(node, available_space, style.min_height(), constraints);
        if content_block_size < min_block_size {
            body_input.available_space.block_size = AvailableSize::definite(min_block_size);
        }
    }
    sizing.make_form_control_content_box_definite(
        node,
        run.layout_mode,
        available_space,
        constraints,
        measured_content_block_size,
    );
    body_input
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
    records: &RunRecords<'_>,
    callbacks: &LayoutPass<'_>,
    node: Node,
    automatic_content_block_size: CssPixels,
) -> Option<CssPixels> {
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
            in_flow_children_margin_box_block_extent(records, callbacks, node)
        }
        _ => None,
    }
}

fn in_flow_children_margin_box_block_extent(
    records: &RunRecords<'_>,
    callbacks: &LayoutPass<'_>,
    node: Node,
) -> Option<CssPixels> {
    let mut block_start = CssPixels::default();
    let mut block_end = CssPixels::default();
    for child in callbacks.children(node) {
        let facts = NodeFacts::new(callbacks, child);
        if facts.is_box() && !facts.is_absolutely_positioned() {
            let used = records.used_values_if_owned(child)?;
            if !used.has_content_offset.get() {
                return None;
            }
            let collapsed = used.uses_collapsing_borders_model.get();
            let content_block_offset = used.content_offset.get().y;
            block_start = block_start.min(content_block_offset - used.margin_box_top(collapsed));
            block_end =
                block_end.max(content_block_offset + used.content_block_size.get() + used.margin_box_bottom(collapsed));
        }
    }
    Some(block_end - block_start)
}

fn finalize_block_level_root(run: &FormattingContextRun, input: &LayoutInput, body_result: &ChildLayoutResult) {
    let node = run.box_;
    let facts = NodeFacts::new(&run.callbacks, node);
    if facts.is_table_wrapper() {
        run.records
            .used_values(node)
            .set_content_inline_size(body_result.automatic_content_inline_size);
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
            node,
            resolution_space,
            input.containing_block_constraints,
            Some(body_result.automatic_content_block_size),
            || unreachable!("a completed block-level run always supplies its automatic content block size"),
        );
    }
}

fn finalize_float_root(run: &FormattingContextRun, input: &LayoutInput, body_result: &ChildLayoutResult) {
    let node = run.box_;
    if NodeFacts::new(&run.callbacks, node).is_table_wrapper() {
        run.records
            .used_values(node)
            .set_content_inline_size(body_result.automatic_content_inline_size);
    }
    run.sizing().resolve_used_block_size_if_treated_as_auto(
        node,
        input.available_space,
        input.containing_block_constraints,
        Some(body_result.automatic_content_block_size),
        || unreachable!("a completed float run always supplies its automatic content block size"),
    );
}

fn dimension_root_in_parent_scope(
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    child: Node,
    input: &LayoutInput,
    child_run_builds_fragments: bool,
) -> Option<AvailableSpace> {
    match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => {
            let parent = parent_block.expect("a block-level run requires an enclosing block formatting context");
            parent.commit_block_level_root_inline_size(child, input);
            let flex_self_resolution_space = flex_self_block_size_resolution_space(
                child_run_builds_fragments,
                &parent.sizing(),
                child,
                input.available_space,
                input.containing_block_constraints,
            );
            parent.resolve_block_level_root_block_size_before_body(child, input, flex_self_resolution_space.is_some());
            flex_self_resolution_space
        }
        ParticipationInParentFormattingContext::Float => {
            let parent = parent_block.expect("a floating run requires an enclosing block formatting context");
            let flex_self_resolution_space = flex_self_block_size_resolution_space(
                child_run_builds_fragments,
                &parent.sizing(),
                child,
                input.available_space,
                input.containing_block_constraints,
            );
            parent.dimension_float_root(child, input, flex_self_resolution_space.is_some());
            flex_self_resolution_space
        }
        _ => None,
    }
}

fn size_skipped_independent_root(
    run: &FormattingContextRun,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    child: Node,
    input: &LayoutInput,
) {
    dimension_root_in_parent_scope(parent_block, child, input, false);
    match input.participation {
        ParticipationInParentFormattingContext::Float => {
            let parent = parent_block.expect("a floating run requires an enclosing block formatting context");
            parent.resolve_used_block_size_if_treated_as_auto(
                child,
                input.available_space,
                input.containing_block_constraints,
                None,
            );
        }
        ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
            let engine = abspos_engine::AbsposEngine::for_run(run);
            engine.dimension_out_of_flow_root(child, abspos_inputs);
            engine.finalize_out_of_flow_root_after_inside_layout(child, abspos_inputs, None);
        }
        ParticipationInParentFormattingContext::BlockLevel
        | ParticipationInParentFormattingContext::AtomicInline
        | ParticipationInParentFormattingContext::Item
        | ParticipationInParentFormattingContext::Root => {}
    }
}

fn body_input_with_inner_available_space(run: &FormattingContextRun, input: &LayoutInput) -> LayoutInput {
    let inner_available_space = run
        .records
        .used_values(run.box_)
        .available_inner_space_or_constraints_from(input.available_space);
    let mut body_input = *input;
    body_input.available_space = inner_available_space;
    body_input
}

#[expect(clippy::too_many_arguments)]
pub(super) fn run_formatting_context(
    purpose: LayoutPurpose,
    parent_fragments: Option<&fragment_tree::RunFragmentBuilder>,
    parent_used: &UsedValues,
    box_: Node,
    root_containing_block: Node,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    fc_type: FormattingContextType,
    layout_mode: LayoutMode,
    should_collect_devtools_layout_data: bool,
    callbacks: LayoutPass<'_>,
    input: LayoutInput,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    table_inline_layout: Option<table_formatting_context::TableInlineLayout>,
) -> ChildLayoutResult {
    // Independent children supply block sizes and baselines to their parent, so only paragraphs
    // in the width query's own block flow may skip their line geometry.
    // NB: A box that establishes an orthogonal flow contributes its block size to the inline size its parent
    //     measures, which takes its line geometry.
    let purpose = if purpose == LayoutPurpose::IntrinsicInlineMeasurement
        && (input.participation != ParticipationInParentFormattingContext::Root
            || frame_established_by(&callbacks, box_).is_orthogonal_to(callbacks.frame))
    {
        LayoutPurpose::Measurement
    } else {
        purpose
    };
    if let Some(parent_fragments) = parent_fragments {
        // A later run for this root supersedes a hit recorded earlier in the same pass.
        parent_fragments.clear_reused_subtree_root(box_, parent_used);
    }
    let root_cells = used_values::UsedValuesCellState::capture(parent_used);
    let cache_key = fc_run_cache::FcRunCacheKey::new(fc_type, input, root_cells);
    let memoizes_measurement = purpose.is_measurement()
        && callbacks.scratch().memoizes_measurements()
        && fc_run_cache::can_describe_run_in_any_layout_mode(
            box_,
            parent_grid.is_some(),
            should_collect_devtools_layout_data,
            &callbacks,
            &cache_key,
        );
    if memoizes_measurement
        && let Some(outputs) = callbacks
            .scratch()
            .memoized_measurement(box_, purpose, layout_mode, &cache_key)
    {
        return absorb_run_outputs(&callbacks, parent_fragments, parent_used, box_, outputs, false);
    }
    let cache_attempt = match fc_run_cache::FcRunCacheAttempt::probe(
        purpose,
        box_,
        parent_grid.is_some(),
        layout_mode,
        should_collect_devtools_layout_data,
        &callbacks,
        &cache_key,
    ) {
        Ok(attempt) => attempt,
        Err(entry) => {
            if let Some(result) = reuse_committed_subtree_as_it_is(
                purpose,
                parent_fragments,
                parent_used,
                box_,
                &callbacks,
                &cache_key,
                &entry,
            ) {
                return result;
            }
            let reuses_committed_subtree = entry.can_reuse_committed_subtree();
            let _trace = callbacks
                .arena()
                .layout_trace
                .run(box_, fc_type, purpose, layout_mode, || {
                    if reuses_committed_subtree {
                        "REUSE SUBTREE"
                    } else {
                        "REPLAY FRAGMENTS"
                    }
                });
            let outputs = entry.outputs_to_replay(&cache_key);
            if entry.is_uncommitted() {
                fc_run_cache::store_replayed_uncommitted_entry(&callbacks, box_, cache_key, &entry, &outputs);
            }
            return absorb_run_outputs(
                &callbacks,
                parent_fragments,
                parent_used,
                box_,
                outputs,
                reuses_committed_subtree,
            );
        }
    };
    let _trace = callbacks
        .arena()
        .layout_trace
        .run(box_, fc_type, purpose, layout_mode, || cache_attempt.trace_action());
    let previous_line_data = cache_attempt.previous_line_data();
    let outputs = execute_formatting_context_run(
        purpose,
        root_cells,
        box_,
        root_containing_block,
        parent_grid,
        fc_type,
        layout_mode,
        should_collect_devtools_layout_data,
        callbacks,
        input,
        parent_block,
        previous_line_data,
        table_inline_layout,
        None,
    );
    if memoizes_measurement {
        callbacks
            .scratch()
            .memoize_measurement(box_, purpose, layout_mode, cache_key, &outputs);
    }
    cache_attempt.conclude(&callbacks, box_, cache_key, &outputs);
    absorb_run_outputs(&callbacks, parent_fragments, parent_used, box_, outputs, false)
}

/// Takes a committing run of `box_` from the cache entry `entry` with nothing replayed: its root record gets what the
/// stored run left, and the run's placement attaches the box's committed fragment as it is. That is exact where the
/// committed fragment is the one the stored run produced, which a plain box shows by holding the record's metrics and
/// the very line data the run left, and where nothing from the stored run reaches past its root.
fn reuse_committed_subtree_as_it_is(
    purpose: LayoutPurpose,
    parent_fragments: Option<&fragment_tree::RunFragmentBuilder>,
    root_used: &UsedValues,
    box_: Node,
    callbacks: &LayoutPass<'_>,
    cache_key: &fc_run_cache::FcRunCacheKey,
    entry: &fc_run_cache::FcRunCacheEntry,
) -> Option<ChildLayoutResult> {
    let fragments = parent_fragments?;
    if purpose != LayoutPurpose::Commit
        || !entry.can_reuse_committed_subtree()
        || entry.is_uncommitted()
        || !cache_key.has_no_table_cell_intrinsic_block_padding()
        || entry.outputs.root.as_ref().is_some_and(|root| !root.carries_nothing())
        || NodeFacts::new(callbacks, box_).has_anchor_names()
    {
        return None;
    }
    let fragment = callbacks
        .arena()
        .with_current_committed_fragment(box_, |fragment| fragment.is_plain_box().then(|| fragment.clone()))
        .flatten()?;
    entry.outputs.root_outcome.clone().apply_to_record(root_used);
    if !root_used.shows_box_of_fragment(&fragment) {
        return None;
    }
    let _trace = callbacks.arena().layout_trace.run(
        box_,
        cache_key.fc_type(),
        purpose,
        LayoutMode::Normal,
        || "REUSE SUBTREE",
    );
    fragments.hold_reused_committed_fragment(box_, root_used, fragment);
    callbacks
        .scratch()
        .receive_host_findings(entry.outputs.host_findings.clone());
    Some(entry.outputs.result)
}

#[expect(clippy::too_many_arguments)]
fn execute_formatting_context_run(
    purpose: LayoutPurpose,
    root_cells: used_values::UsedValuesCellState,
    box_: Node,
    root_containing_block: Node,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    fc_type: FormattingContextType,
    layout_mode: LayoutMode,
    should_collect_devtools_layout_data: bool,
    callbacks: LayoutPass<'_>,
    input: LayoutInput,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    previous_line_data: Option<std::sync::Arc<inline_content::InlineContent>>,
    table_inline_layout: Option<table_formatting_context::TableInlineLayout>,
    fragmentainer: Option<std::rc::Rc<fragmentation::FragmentainerContext>>,
) -> RunOutputs {
    assert!(!box_.is_invalid());
    let root_used = root_cells.materialize_record();
    RunRecords::with_root(
        callbacks,
        box_,
        root_containing_block,
        &root_used,
        |records, callbacks| {
            let run = FormattingContextRun {
                purpose,
                records,
                box_,
                layout_mode,
                callbacks,
                should_collect_devtools_layout_data,
                treat_block_axis_percentage_insets_as_auto_beyond_root: input
                    .sizing
                    .treat_block_axis_percentage_insets_as_auto_beyond_root,
                fragments: (layout_mode == LayoutMode::Normal && !purpose.is_measurement()).then(|| {
                    std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new(
                        box_,
                        (!root_containing_block.is_invalid()).then_some(root_containing_block),
                    ))
                }),
                previous_line_data,
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
            if let Some(table_inline_layout) = table_inline_layout {
                run.records
                    .store_table_inline_layout(table_inline_layout.table_box(), table_inline_layout);
            }
            let RootSizingOutcome {
                body_input,
                atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
            } = apply_root_sizing_directives(run, &input, fc_type);

            let body_input = if changes_frame {
                enter_own_frame(run, body_run, &input, body_input, parent_frame, own_frame)
            } else {
                body_input
            };

            let mut implementation = None;
            let mut result = if !changes_frame
                && layout_mode == LayoutMode::Normal
                && !purpose.is_measurement()
                && matches!(
                    input.participation,
                    ParticipationInParentFormattingContext::AtomicInline
                )
                && fc_type == FormattingContextType::Block
                && callbacks.first_child(box_).is_invalid()
            {
                // An empty atomic block context has no body output. Root sizing and finalization still
                // run through the shared paths around this branch.
                ChildLayoutResult::default()
            } else {
                let mut context_implementation =
                    create_formatting_context_implementation(body_run, parent_grid, fc_type);
                let result = match &mut context_implementation {
                    FormattingContextImplementation::Block(context) => {
                        context.run(body_run, body_input);
                        let mut result = ChildLayoutResult {
                            automatic_content_inline_size: context.automatic_content_inline_size(),
                            min_content_inline_size_from_max_content_layout: context
                                .min_content_inline_size_from_max_content_layout(),
                            automatic_content_block_size: context.automatic_content_block_size(),
                            automatic_line_clamp_max_lines: context.automatic_line_clamp_max_lines(),
                            baselines: context.derived_baselines_of_root_box(),
                            table_box_in_wrapper_border_box_block_size: context
                                .table_box_in_wrapper_border_box_block_size(),
                            ..ChildLayoutResult::default()
                        };
                        // https://w3c.github.io/mathml-core/#layout-of-operators
                        // NB: An operator lays out its text in a block formatting context, unless it is drawn with a
                        //     stretched or large glyph.
                        if NodeFacts::new(&body_run.callbacks, box_).display().is_math_inside() {
                            math_formatting_context::layout_operator(body_run, &input, &mut result);
                        }
                        store_derived_baselines(run.records.used_values(run.box_), result.baselines);
                        result
                    }
                    FormattingContextImplementation::Flex(context) => {
                        context.run(body_run, body_input);
                        let baselines = context.derived_baselines_of_root_box();
                        store_derived_baselines(run.records.used_values(run.box_), baselines);
                        ChildLayoutResult {
                            automatic_content_inline_size: context.automatic_content_inline_size(),
                            automatic_content_block_size: context.automatic_content_block_size(),
                            baselines,
                            ..ChildLayoutResult::default()
                        }
                    }
                    FormattingContextImplementation::Grid(context) => {
                        context.run(body_run, body_input);
                        let baselines = context.derived_baselines_of_root_box();
                        store_derived_baselines(run.records.used_values(run.box_), baselines);
                        ChildLayoutResult {
                            automatic_content_inline_size: context.automatic_content_inline_size(),
                            automatic_content_block_size: context.automatic_content_block_size(),
                            baselines,
                            ..ChildLayoutResult::default()
                        }
                    }
                    FormattingContextImplementation::Table(context) => {
                        context.run(body_run, body_input, run.records.take_table_inline_layout(run.box_));
                        let baselines = context.derived_baselines_of_root_box();
                        store_derived_baselines(run.records.used_values(run.box_), baselines);
                        ChildLayoutResult {
                            automatic_content_inline_size: context.automatic_content_inline_size(),
                            automatic_content_block_size: context.automatic_content_block_size,
                            baselines,
                            ..ChildLayoutResult::default()
                        }
                    }
                    FormattingContextImplementation::Svg(context) => {
                        context.run(body_run, body_input);
                        ChildLayoutResult::default()
                    }
                    FormattingContextImplementation::Math(context) => {
                        context.run(body_run, body_input);
                        let baselines = context.derived_baselines_of_root_box();
                        store_derived_baselines(run.records.used_values(run.box_), baselines);
                        ChildLayoutResult {
                            automatic_content_inline_size: context.automatic_content_inline_size(),
                            automatic_content_block_size: context.automatic_content_block_size(),
                            baselines,
                            ..ChildLayoutResult::default()
                        }
                    }
                    FormattingContextImplementation::ReplacedWithChildren => {
                        replaced_with_children_formatting_context::layout_replaced_with_children(body_run, body_input)
                    }
                    FormattingContextImplementation::InternalReplaced
                    | FormattingContextImplementation::InternalDummy => ChildLayoutResult::default(),
                };
                implementation = Some(context_implementation);
                result
            };
            if changes_frame {
                result = leave_own_frame(run, body_run, result, parent_frame, own_frame);
            }

            // https://drafts.csswg.org/css-sizing-4/#intrinsic-size-override
            // If an element has an explicit intrinsic inner size in an axis, then after laying out the element as normal for
            // size containment, the size of the contents in that axis are instead treated as being the explicit intrinsic inner
            // size instead of what was calculated in layout, and layout is performed again if necessary.
            //
            // Every formatting context reports its content sizes through ChildLayoutResult, so overriding here covers them all
            // rather than one context's root-height path.
            let containment_facts = NodeFacts::new(&run.callbacks, run.box_);
            let lays_out_own_content = run.fragmentainer.is_none();
            if lays_out_own_content && containment_facts.node_has_size_containment() {
                let style = containment_facts.style();
                // https://drafts.csswg.org/css-contain-2/#containment-size
                // Giving an element size containment makes its principal box a size containment box and has the following
                // effects:
                // 1. The intrinsic sizes of the size containment box are determined as if the element had no content, following
                //    the same logic as when sizing as if empty.
                result.automatic_content_inline_size = if style.contain_intrinsic_width_has_length() {
                    CssPixels::nearest_value_for(style.contain_intrinsic_width_px())
                } else {
                    CssPixels::default()
                };
                result.automatic_content_block_size = if style.contain_intrinsic_height_has_length() {
                    CssPixels::nearest_value_for(style.contain_intrinsic_height_px())
                } else {
                    CssPixels::default()
                };
            } else if lays_out_own_content && containment_facts.node_has_inline_size_containment() {
                // https://drafts.csswg.org/css-contain-2/#containment-inline-size
                // "This means the inline-axis intrinsic sizes of the principal box are determined as if the element had
                //  no content."
                let style = containment_facts.style();
                result.automatic_content_inline_size = if style.contain_intrinsic_width_has_length() {
                    CssPixels::nearest_value_for(style.contain_intrinsic_width_px())
                } else {
                    CssPixels::default()
                };
            }

            if containment_facts.has_preferred_aspect_ratio() {
                result.content_block_size_for_aspect_ratio_minimum = content_block_size_for_aspect_ratio_minimum(
                    run.records,
                    &run.callbacks,
                    run.box_,
                    result.automatic_content_block_size,
                );
            }

            match input.participation {
                ParticipationInParentFormattingContext::BlockLevel => {
                    finalize_block_level_root(run, &input, &result);
                }
                ParticipationInParentFormattingContext::Float => {
                    finalize_float_root(run, &input, &result);
                }
                ParticipationInParentFormattingContext::AtomicInline => {
                    finalize_atomic_root_block_size(run, &input, result.automatic_content_block_size, parent_block);
                }
                ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
                    abspos_engine::AbsposEngine::for_run(run).finalize_out_of_flow_root_after_inside_layout(
                        run.box_,
                        abspos_inputs,
                        Some(result.automatic_content_block_size),
                    );
                }
                ParticipationInParentFormattingContext::Item => {
                    if input.sizing.adopt_automatic_content_block_size {
                        let used = run.records.used_values(run.box_);
                        used.set_content_block_size(result.automatic_content_block_size);
                    }
                    if let Some(block_size) = input.sizing.content_block_size_after_inside_layout {
                        run.records.used_values(run.box_).set_content_block_size(block_size);
                    }
                }
                ParticipationInParentFormattingContext::Root => {}
            }
            if matches!(
                input.participation,
                ParticipationInParentFormattingContext::BlockLevel
                    | ParticipationInParentFormattingContext::Float
                    | ParticipationInParentFormattingContext::AtomicInline
            ) {
                let sizing = run.sizing();
                sizing.apply_automatic_minimum_block_size_from_aspect_ratio(
                    run.box_,
                    sizing.available_space_for_block_size_resolution(
                        run.box_,
                        input.available_space,
                        input.containing_block_constraints,
                    ),
                    input.containing_block_constraints,
                    result.content_block_size_for_aspect_ratio_minimum,
                );
            }
            result.omitted_line_layout = run.records.omitted_line_layout();
            result.depends_on_percentage_block_size = run.sizing().resolve_percentage_block_size_dependency(run.box_);

            // NB: The root's descendants are laid out in the frame the root establishes.
            let take_run_fragments = || {
                run.fragments
                    .as_ref()
                    .map(|fragments| fragments.take_unplaced_root(run.records, &body_run.callbacks))
            };

            let registered_abspos_children_could_never_be_laid_out = run.fragments.is_none();
            if registered_abspos_children_could_never_be_laid_out {
                return run.outputs(
                    result,
                    take_run_fragments(),
                    atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
                );
            }
            if let Some(implementation) = implementation {
                match &implementation {
                    FormattingContextImplementation::Block(_) => {}
                    FormattingContextImplementation::Table(_) => {
                        let box_ = run.box_;
                        in_own_frame(run, parent_frame, own_frame, || {
                            register_table_abspos_descendants(body_run, box_);
                        });
                    }
                    FormattingContextImplementation::Flex(context) => {
                        in_own_frame(run, parent_frame, own_frame, || context.parent_did_dimension());
                    }
                    FormattingContextImplementation::Grid(context) => {
                        in_own_frame(run, parent_frame, own_frame, || context.parent_did_dimension());
                    }
                    FormattingContextImplementation::Math(context) => {
                        in_own_frame(run, parent_frame, own_frame, || context.parent_did_dimension());
                    }
                    FormattingContextImplementation::Svg(_) | FormattingContextImplementation::ReplacedWithChildren => {
                    }
                    FormattingContextImplementation::InternalReplaced
                    | FormattingContextImplementation::InternalDummy => {
                        return run.outputs(
                            result,
                            take_run_fragments(),
                            atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
                        );
                    }
                }
            }
            run.records.used_values(run.box_).seal_own_metrics();
            run.outputs(
                result,
                take_run_fragments(),
                atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
            )
        },
    )
}

/// The frame in which `node` lays out its contents: that of its writing mode.
pub(crate) fn frame_established_by(callbacks: &LayoutPass<'_>, node: Node) -> geometry::Frame {
    callbacks
        .computed_values_view_if_styled(node)
        .map_or(callbacks.frame, |style| geometry::Frame::of(style.writing_mode()))
}

/// Runs `body` with the run root's metrics read in the frame it lays out its contents in.
fn in_own_frame<R>(
    run: &FormattingContextRun,
    parent_frame: geometry::Frame,
    own_frame: geometry::Frame,
    body: impl FnOnce() -> R,
) -> R {
    if parent_frame == own_frame {
        return body();
    }
    let used = run.records.used_values(run.box_);
    used.move_box_metrics_to_frame(parent_frame, own_frame);
    let result = body();
    used.move_box_metrics_to_frame(own_frame, parent_frame);
    result
}

/// Moves the root of a run whose writing mode establishes a frame other than its parent's into its own frame, where
/// its contents are laid out, and returns the input for laying them out there.
fn enter_own_frame(
    run: &FormattingContextRun,
    body_run: &FormattingContextRun,
    input: &LayoutInput,
    body_input: LayoutInput,
    parent_frame: geometry::Frame,
    own_frame: geometry::Frame,
) -> LayoutInput {
    let node = run.box_;
    let used = run.records.used_values(node);
    used.move_box_metrics_to_frame(parent_frame, own_frame);
    if !parent_frame.is_orthogonal_to(own_frame) {
        // NB: Sizes keep their axes between frames that are not orthogonal, so the contents are laid out with the
        //     input the root's sizing gave them, apart from the parent's block formatting context they leave.
        return LayoutInput {
            content_box_position_in_bfc_root: None,
            ..body_input
        };
    }
    let mut constraints = input.containing_block_constraints;
    let mut available_space = input.available_space;
    let mut sizing = RootSizingDirectives {
        treat_block_axis_percentage_insets_as_auto_beyond_root: input
            .sizing
            .treat_block_axis_percentage_insets_as_auto_beyond_root,
        forced_content_inline_size: body_input.sizing.forced_content_inline_size,
        forced_content_block_size: body_input.sizing.forced_content_block_size,
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
    if used.inline_size_constraint.get() == SizeConstraint::None && !used.has_definite_inline_size.get() {
        // NB: A flex or grid container sizes its items in both axes before laying out their contents, even where it
        //     leaves a size indefinite for the percentages of their contents.
        let inline_size = if input.participation == ParticipationInParentFormattingContext::Item
            && NodeFacts::new(&run.callbacks, node).parent_is_flex_or_grid_container()
        {
            used.content_inline_size.get()
        } else {
            body_run
                .sizing()
                .orthogonal_flow_inline_size(node, available_space, constraints)
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

/// Moves the root of a run back into its parent's frame once its contents are laid out in its own frame, and maps
/// what the layout found into the parent's frame.
fn leave_own_frame(
    run: &FormattingContextRun,
    body_run: &FormattingContextRun,
    mut result: ChildLayoutResult,
    parent_frame: geometry::Frame,
    own_frame: geometry::Frame,
) -> ChildLayoutResult {
    let used = run.records.used_values(run.box_);
    // NB: The root's lines are laid out along its own inline axis, so finish them while that is what the root
    //     record holds.
    used.finish_line_data(&body_run.callbacks);
    if parent_frame.is_orthogonal_to(own_frame) {
        // https://drafts.csswg.org/css-writing-modes-4/#orthogonal-shrink-to-fit
        // Note that this section requires that when a child box auto-sized in its block axis establishes an
        // orthogonal flow, the used block size of the child is calculated to fit its content; and this resulting
        // content-based size is used as input to the inline-axis min-content size and max-content size of the
        // parent.
        // NB: A root measured under an intrinsic inline size constraint has its inline size only in the result.
        let content_inline_size = if used.inline_size_constraint.get() == SizeConstraint::None {
            used.content_inline_size.get()
        } else {
            result.automatic_content_inline_size
        };
        // NB: The root's baselines lie across its own block axis, which is the horizontal axis of its parent's frame.
        let block_start_is_left = matches!(
            own_frame.side_in(geometry::Side::Top, parent_frame),
            geometry::Side::Left
        );
        let content_block_size = used.content_block_size.get();
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
        if orthogonal_flow_baselines != DerivedBaselines::default() {
            used.rare_data_mut().orthogonal_flow_baselines = Some(orthogonal_flow_baselines);
        } else if used.orthogonal_flow_baselines().is_some() {
            used.rare_data_mut().orthogonal_flow_baselines = None;
        }
        result.automatic_content_inline_size = result.automatic_content_block_size;
        result.min_content_inline_size_from_max_content_layout = None;
        result.automatic_content_block_size = content_inline_size;
        result.baselines = DerivedBaselines::default();
        result.content_block_size_for_aspect_ratio_minimum = None;
        // NB: The block size of a table box in its parent's frame is its inline size in the frame of its wrapper.
        if result.table_box_in_wrapper_border_box_block_size.is_some() {
            let table_used = run
                .records
                .used_values(body_run.sizing().table_box_inside_wrapper(run.box_));
            result.table_box_in_wrapper_border_box_block_size =
                Some(table_used.border_box_inline_size(table_used.uses_collapsing_borders_model.get()));
        }
    }
    used.move_box_metrics_to_frame(own_frame, parent_frame);
    result
}

fn finalize_atomic_root_block_size(
    run: &FormattingContextRun,
    input: &LayoutInput,
    automatic_content_block_size_of_completed_body_run: CssPixels,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
) {
    let node = run.box_;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let sizing = run.sizing();
    if sizing.box_is_sized_as_replaced_element(node, available_space, constraints) {
        return;
    }
    if sizing.should_treat_block_size_as_auto(node, available_space, constraints) {
        sizing.resolve_used_block_size_if_treated_as_auto(node, available_space, constraints, None, || {
            let available_inner_space = run
                .records
                .used_values(node)
                .available_inner_space_or_constraints_from(available_space);
            match parent_block {
                Some(parent) => parent.compute_automatic_block_size_for_block_level_element(
                    node,
                    available_inner_space,
                    constraints,
                    Some(automatic_content_block_size_of_completed_body_run),
                ),
                None => independent_root_automatic_block_size(
                    run.purpose,
                    run.records,
                    &run.callbacks,
                    node,
                    available_inner_space,
                    constraints,
                    Some(automatic_content_block_size_of_completed_body_run),
                ),
            }
        });
    } else {
        sizing.resolve_used_block_size_if_not_treated_as_auto(node, available_space, constraints);
    }
}

pub(crate) fn propagate_percentage_block_size_dependency_to_containing_block(
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
    child: Node,
    child_depends_on_percentage_block_size: bool,
) {
    let run_root_reports_its_dependency_through_its_run_result = child == records.root();
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
    let containing_block = callbacks.in_flow_containing_block(child);
    let containing_block_record_or_run_root_that_forwarded_the_basis = (!containing_block.is_invalid())
        .then(|| records.used_values_if_owned(containing_block))
        .flatten()
        .unwrap_or_else(|| records.used_values(records.root()));
    containing_block_record_or_run_root_that_forwarded_the_basis
        .has_descendant_that_depends_on_percentage_block_size
        .set(true);
}

pub(crate) fn layout_inside_child(
    run: &FormattingContextRun,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    child: Node,
    layout_mode: LayoutMode,
    mut input: LayoutInput,
    force_independent_context_run: bool,
) -> ChildLayoutOutcome {
    let facts = NodeFacts::new(&run.callbacks, child);
    let used = run.records.used_values(child);
    if let Some((padding_top, padding_bottom)) = input.sizing.table_cell_intrinsic_block_padding {
        debug_assert!(facts.is_table_cell());
        debug_assert!(matches!(
            input.participation,
            ParticipationInParentFormattingContext::Item
        ));
        used.padding_top.set(used.padding_top.get() + padding_top);
        used.padding_bottom.set(used.padding_bottom.get() + padding_bottom);
    }
    let note_skipped_child_dependency = || {
        propagate_percentage_block_size_dependency_to_containing_block(
            run.records,
            &run.callbacks,
            child,
            run.sizing().skipped_child_depends_on_percentage_block_size(child),
        );
    };
    if !force_independent_context_run
        && (layout_mode == LayoutMode::IntrinsicSizing || !facts.node_has_size_containment())
        && matches!(
            input.participation,
            ParticipationInParentFormattingContext::AtomicInline
        )
        && formatting_context_type_created_by_box(facts) == Some(FormattingContextType::Block)
        && run.callbacks.first_child(child).is_invalid()
    {
        // OPTIMIZATION: An empty atomic block has no formatting-context body output. Size it in the
        // parent run, which will also construct its fragment when placing the box.
        // A fragment held by an earlier sizing of the box in this run may no longer match the record.
        used.held_committed_fragment.take();
        let committed_fragment = run.sizing().dimension_empty_atomic_root(
            child,
            input.available_space,
            input.containing_block_constraints,
            layout_mode,
        );
        // OPTIMIZATION: A box that took its metrics from its committed fragment is placed with that fragment, where
        // it shows the box as the record holds it.
        if let Some(fragment) = committed_fragment
            && run.purpose == LayoutPurpose::Commit
            && let Some(fragments) = run.fragments.as_deref()
            && fragment.is_plain_box()
            && used.shows_box_of_fragment(&fragment)
            && !facts.has_anchor_names()
        {
            fragments.hold_committed_fragment_of_empty_box(used, fragment);
        }
        store_derived_baselines(used, DerivedBaselines::default());
        note_skipped_child_dependency();
        return ChildLayoutOutcome::Skipped;
    }
    if !force_independent_context_run
        && layout_mode == LayoutMode::IntrinsicSizing
        && !facts.is_inline()
        && used.inline_size_constraint.get() == SizeConstraint::None
        && used.block_size_constraint.get() == SizeConstraint::None
        && used.has_definite_inline_size()
        && used.has_definite_block_size()
    {
        size_skipped_independent_root(run, parent_block, child, &input);
        note_skipped_child_dependency();
        return ChildLayoutOutcome::Skipped;
    }
    let creates_replaced_context = matches!(
        formatting_context_type_created_by_box(facts),
        Some(FormattingContextType::InternalReplaced | FormattingContextType::ReplacedWithChildren)
    );
    if !facts.can_have_children() && !creates_replaced_context {
        size_skipped_independent_root(run, parent_block, child, &input);
        note_skipped_child_dependency();
        return ChildLayoutOutcome::Skipped;
    }

    let fc_type = formatting_context_type_created_by_box(facts)
        .or_else(|| force_independent_context_run.then(|| independent_formatting_context_type(child, &run.callbacks)));
    let Some(fc_type) = fc_type else {
        if force_independent_context_run {
            note_skipped_child_dependency();
            return ChildLayoutOutcome::Skipped;
        }
        return ChildLayoutOutcome::ReenterCurrent;
    };
    let root_containing_block = run
        .callbacks
        .containing_block_for_child_run(child, &input.participation);
    // https://drafts.csswg.org/css-sizing-3/#definite
    // "Additionally, the size of the containing block of an absolutely positioned element is always definite with
    //  respect to that element."
    // NB: That containing block may also have been laid out by a run other than this one, which holds no record of it.
    input.sizing.treat_block_axis_percentage_insets_as_auto_beyond_root = match input.participation {
        ParticipationInParentFormattingContext::AbsolutelyPositioned(_) => false,
        _ => treat_block_axis_percentage_insets_as_auto_beyond_anonymous_child_root(
            run.records,
            &run.callbacks,
            child,
            root_containing_block,
            run.box_,
            run.treat_block_axis_percentage_insets_as_auto_beyond_root,
        ),
    };
    input.sizing.flex_self_block_size_resolution_space = dimension_root_in_parent_scope(
        parent_block,
        child,
        &input,
        layout_mode == LayoutMode::Normal && !run.purpose.is_measurement(),
    );
    let result = run_formatting_context(
        run.purpose,
        run.fragments.as_deref(),
        used,
        child,
        root_containing_block,
        parent_grid,
        fc_type,
        layout_mode,
        run.should_collect_devtools_layout_data,
        run.callbacks,
        input,
        parent_block,
        run.records.take_table_inline_layout(child),
    );
    propagate_percentage_block_size_dependency_to_containing_block(
        run.records,
        &run.callbacks,
        child,
        result.depends_on_percentage_block_size,
    );
    ChildLayoutOutcome::Created(result)
}

/// Runs a table cell's committing layout while its table still sizes rows, beside the cell's record, and keeps the
/// outputs in the run cache. The cell's committing run, once the rows are sized, replays them: its input differs only
/// in the intrinsic block padding, which the contents of an eligible cell never observe (see
/// fc_run_cache::table_cell_contents_never_observe_intrinsic_block_padding). Returns None for any other cell.
pub(crate) fn run_table_cell_ahead_of_its_intrinsic_block_padding(
    run: &FormattingContextRun,
    cell: Node,
    mut input: LayoutInput,
) -> Option<ChildLayoutResult> {
    debug_assert!(matches!(
        input.participation,
        ParticipationInParentFormattingContext::Item
    ));
    if run.purpose != LayoutPurpose::Commit
        || run.layout_mode != LayoutMode::Normal
        || formatting_context_type_created_by_box(NodeFacts::new(&run.callbacks, cell))
            != Some(FormattingContextType::Block)
        || !fc_run_cache::table_cell_contents_never_observe_intrinsic_block_padding(&run.callbacks, cell)
    {
        return None;
    }
    // Everything layout_inside_child() hands the committing run, but the intrinsic block padding.
    input.sizing.table_cell_intrinsic_block_padding = Some((CssPixels::default(), CssPixels::default()));
    let root_containing_block = run.callbacks.containing_block_for_child_run(cell, &input.participation);
    input.sizing.treat_block_axis_percentage_insets_as_auto_beyond_root =
        treat_block_axis_percentage_insets_as_auto_beyond_anonymous_child_root(
            run.records,
            &run.callbacks,
            cell,
            root_containing_block,
            run.box_,
            run.treat_block_axis_percentage_insets_as_auto_beyond_root,
        );
    input.sizing.flex_self_block_size_resolution_space = None;
    let root_cells = used_values::UsedValuesCellState::capture(run.records.used_values(cell));
    let cache_key = fc_run_cache::FcRunCacheKey::new(FormattingContextType::Block, input, root_cells);
    let cache_attempt = match fc_run_cache::FcRunCacheAttempt::probe(
        LayoutPurpose::Commit,
        cell,
        false,
        LayoutMode::Normal,
        run.should_collect_devtools_layout_data,
        &run.callbacks,
        &cache_key,
    ) {
        Ok(fc_run_cache::FcRunCacheAttempt::Bypass) => return None,
        Ok(attempt) => attempt,
        Err(entry) => {
            let _trace = run.callbacks.arena().layout_trace.run(
                cell,
                FormattingContextType::Block,
                LayoutPurpose::Commit,
                LayoutMode::Normal,
                || "READ RESULT AHEAD OF INTRINSIC BLOCK PADDING",
            );
            return Some(entry.outputs.result);
        }
    };
    let _trace = run.callbacks.arena().layout_trace.run(
        cell,
        FormattingContextType::Block,
        LayoutPurpose::Commit,
        LayoutMode::Normal,
        || "RUN AHEAD OF INTRINSIC BLOCK PADDING",
    );
    let previous_line_data = cache_attempt.previous_line_data();
    let outputs = execute_formatting_context_run(
        LayoutPurpose::Commit,
        root_cells,
        cell,
        root_containing_block,
        None,
        FormattingContextType::Block,
        LayoutMode::Normal,
        run.should_collect_devtools_layout_data,
        run.callbacks,
        input,
        None,
        previous_line_data,
        None,
        None,
    );
    cache_attempt.conclude_uncommitted(&run.callbacks, cell, cache_key, &outputs);
    Some(outputs.result)
}

fn absorb_run_outputs(
    callbacks: &LayoutPass<'_>,
    parent_fragments: Option<&fragment_tree::RunFragmentBuilder>,
    parent_used: &UsedValues,
    child: Node,
    outputs: RunOutputs,
    reuses_committed_subtree: bool,
) -> ChildLayoutResult {
    let RunOutputs {
        result,
        root,
        root_outcome,
        atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: _,
        host_findings,
    } = outputs;
    callbacks.scratch().receive_host_findings(host_findings);
    root_outcome.apply_to_record(parent_used);
    if let (Some(fragments), Some(root)) = (parent_fragments, root) {
        debug_assert!(root.node == child, "a child run returned a root for a different box");
        if reuses_committed_subtree {
            fragments.note_reused_subtree_root(child);
        }
        fragments.hold_unplaced_root(root);
    }
    result
}

pub(super) fn independent_formatting_context_type(box_: Node, callbacks: &LayoutPass<'_>) -> FormattingContextType {
    let facts = NodeFacts::new(callbacks, box_);
    if let Some(fc_type) = formatting_context_type_created_by_box(facts) {
        return fc_type;
    }
    if facts.is_block_container() {
        return FormattingContextType::Block;
    }

    // HACK: Instead of crashing in scenarios that assume the formatting context can be created, create a dummy formatting context that does nothing.
    eprintln!(
        "FIXME: An independent formatting context was requested from a Box that does not have a formatting context type. A dummy formatting context will be created instead."
    );
    FormattingContextType::InternalDummy
}

pub(crate) fn resolve_block_axis_percentage_inset_basis_is_definite(
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
    containing_block: Node,
    formatting_context_root: Node,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
) -> bool {
    let mut candidate = containing_block;
    while !candidate.is_invalid() {
        let facts = NodeFacts::new(callbacks, candidate);
        if !facts.is_anonymous() || facts.is_table_cell() {
            return records.used_values(candidate).has_definite_block_size();
        }
        if candidate == formatting_context_root {
            return !treat_block_axis_percentage_insets_as_auto_beyond_root;
        }
        candidate = callbacks.in_flow_containing_block(candidate);
    }
    true
}

pub(crate) fn treat_block_axis_percentage_insets_as_auto_beyond_anonymous_child_root(
    records: &RunRecords,
    callbacks: &LayoutPass<'_>,
    child_root: Node,
    child_root_containing_block: Node,
    formatting_context_root: Node,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
) -> bool {
    let child_root_facts = NodeFacts::new(callbacks, child_root);
    if !child_root_facts.is_anonymous() || child_root_facts.is_table_cell() {
        return false;
    }
    !resolve_block_axis_percentage_inset_basis_is_definite(
        records,
        callbacks,
        child_root_containing_block,
        formatting_context_root,
        treat_block_axis_percentage_insets_as_auto_beyond_root,
    )
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
    let arena = state.arena();
    viewport_propagation::propagate_root_styles_to_viewport(
        super::tree_mutation::HostCalls(work),
        arena,
        root,
        &viewport_propagation::viewport_propagation_facts(arena),
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

/// What every layout stage of a round reads besides the arena: the one question a pass may ask
/// the document, and the document's viewport and quirks mode as the round began.
#[derive(Clone, Copy)]
pub(crate) struct LayoutStageFacts {
    pub(crate) container_length_bases: layout_pass::ContainerLengthBasesQuery,
    pub(crate) viewport_inline_size_raw: i32,
    pub(crate) viewport_block_size_raw: i32,
    pub(crate) document_in_quirks_mode: bool,
}

/// What a layout stage computes from: the arena and the layout stage's scratch, borrowed for the
/// pass, the one question the pass may ask the document, and the facts of the pass. It holds no
/// main thread token, so nothing the stage calls can reach the rest of the host.
struct LayoutStageInput<'a> {
    arena: &'a LayoutNodeArena,
    scratch: &'a super::run_records::LayoutScratch,
    container_length_bases: layout_pass::ContainerLengthBasesQuery,
    root: NodeSlotId,
    viewport_inline_size_raw: i32,
    viewport_block_size_raw: i32,
    document_in_quirks_mode: bool,
}

/// The fragments a layout stage computed, which its commit consumes, and what the stage found out for the host.
struct LayoutStageOutput {
    fragments: fragment_tree::CompletedPassFragments,
    host_findings: super::run_records::HostFindings,
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
        let input = LayoutStageInput {
            arena: state.arena(),
            scratch: state.layout_scratch(),
            container_length_bases: stage.container_length_bases,
            root,
            viewport_inline_size_raw: stage.viewport_inline_size_raw,
            viewport_block_size_raw: stage.viewport_block_size_raw,
            document_in_quirks_mode: stage.document_in_quirks_mode,
        };
        let output = match kind {
            LayoutStageKind::Root {
                should_collect_devtools_layout_data,
            } => compute_root_layout(input, should_collect_devtools_layout_data),
            LayoutStageKind::Boundary { containing_block } => compute_subtree_layout_fragments(input, containing_block),
        };
        let mut notifications = commit::commit_replacing(root, state.arena_mut(), &output.fragments);
        let super::run_records::HostFindings {
            nodes_with_unresolved_container_lengths,
            messages,
            document_svg_root_natural_size,
        } = output.host_findings;
        if let Some((root_box, natural_size)) = document_svg_root_natural_size {
            state
                .arena_mut()
                .note_document_svg_root_natural_size(root_box, natural_size);
        }
        notifications.prepend_layout_messages(messages);
        let arena = state.arena();
        for node in nodes_with_unresolved_container_lengths {
            arena.note_unresolved_container_lengths(node);
        }
        arena.end_layout_pass();
        state.layout_scratch().end_layout_pass();
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
        container_length_bases,
        root,
        viewport_inline_size_raw,
        viewport_block_size_raw,
        document_in_quirks_mode,
    } = input;
    arena.begin_active_layout_pass();
    // NB: The tree builder derives the facts of rebuilt subtrees. Unclassified invalidations
    // require deriving them for the entire tree instead.
    if arena.pending_updates_escape_partial_relayout.get() {
        arena.derive_facts_in_subtree(root);
        arena.clear_partial_relayout_escape();
        arena.pending_attached_subtree_roots.borrow_mut().clear();
    } else {
        // NB: Top-layer updates can attach boxes without running the tree builder.
        arena.derive_facts_after_tree_update(&[]);
    }
    arena.mark_flex_and_grid_items_of_nodes_marked_for_layout();
    drop_intrinsic_size_cache_entries_of_released_slots(arena, scratch);
    let callbacks = LayoutPass::new(
        arena,
        scratch,
        container_length_bases,
        CssPixels::from_raw(viewport_inline_size_raw),
        CssPixels::from_raw(viewport_block_size_raw),
        document_in_quirks_mode,
    );
    let viewport_inline_size = CssPixels::from_raw(viewport_inline_size_raw);
    let viewport_block_size = CssPixels::from_raw(viewport_block_size_raw);

    let root_constraints = ContainingBlockConstraints {
        percentage_basis_inline_size: Some(viewport_inline_size),
        percentage_basis_block_size: Some(viewport_block_size),
        ..ContainingBlockConstraints::default()
    };
    RunRecords::with_unrooted(callbacks, root, NodeSlotId::INVALID, |entry_records, callbacks| {
        let _trace = arena.layout_trace.pass(None);
        let viewport_used = entry_records.create_used_values(&callbacks, root, root_constraints);
        let entry_fragments = std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new_entry_accumulator(root));
        let entry_run = FormattingContextRun {
            purpose: LayoutPurpose::Commit,
            records: entry_records,
            box_: root,
            layout_mode: LayoutMode::Normal,
            callbacks,
            should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: false,
            fragments: Some(entry_fragments.clone()),
            previous_line_data: None,
            fragmentainer: None,
        };

        let mut root_for_layout = root;
        let mut root_for_layout_used = viewport_used;
        let first_child = callbacks.first_child(root);
        if !first_child.is_invalid() && NodeFacts::new(&callbacks, first_child).is_svg_svg_box() {
            viewport_used.set_content_inline_size(viewport_inline_size);
            viewport_used.set_content_block_size(viewport_block_size);
            place_child(&entry_run, root, FfiCssPixelPoint::default(), None);
            root_for_layout_used = entry_records.create_used_values(&callbacks, first_child, root_constraints);
            root_for_layout = first_child;
        }
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
        run_formatting_context(
            LayoutPurpose::Commit,
            Some(&entry_fragments),
            root_for_layout_used,
            root_for_layout,
            callbacks.in_flow_containing_block(root_for_layout),
            None,
            fc_type,
            LayoutMode::Normal,
            should_collect_devtools_layout_data,
            callbacks,
            input,
            None,
            None,
        );
        place_child(&entry_run, root_for_layout, FfiCssPixelPoint::default(), None);
        finish_entry_pass(
            entry_records,
            &entry_fragments,
            &callbacks,
            should_collect_devtools_layout_data,
        )
    })
}

/// Drops the intrinsic size cache entries of the slots the arena let go of, or whose cache epoch wrapped, since the
/// last stage, before any run of this one reads a cache.
fn drop_intrinsic_size_cache_entries_of_released_slots(
    arena: &LayoutNodeArena,
    scratch: &super::run_records::LayoutScratch,
) {
    let drops = arena.take_intrinsic_size_cache_drops();
    if !drops.is_empty() {
        scratch.intrinsic_size_caches.drop_slots(drops);
    }
}

fn finish_entry_pass(
    entry_records: &RunRecords<'_>,
    entry_fragments: &std::rc::Rc<fragment_tree::RunFragmentBuilder>,
    callbacks: &LayoutPass<'_>,
    should_collect_devtools_layout_data: bool,
) -> LayoutStageOutput {
    abspos_engine::drain_abspos_with_placed_containing_blocks(
        entry_records,
        *callbacks,
        should_collect_devtools_layout_data,
        entry_fragments,
    );
    let pass_fragments = entry_fragments.take_completed_pass(entry_records, callbacks);
    debug_assert!(
        !pass_fragments.roots.is_empty(),
        "an entry pass always produces the entry root's fragment"
    );
    let mut host_findings = callbacks.scratch().take_host_findings_outside_runs();
    if let Some(found_by_entry_run) = entry_records.take_host_findings() {
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
        container_length_bases,
        root,
        viewport_inline_size_raw,
        viewport_block_size_raw,
        document_in_quirks_mode,
    } = input;
    arena.begin_active_layout_pass();
    let callbacks = LayoutPass::new(
        arena,
        scratch,
        container_length_bases,
        CssPixels::from_raw(viewport_inline_size_raw),
        CssPixels::from_raw(viewport_block_size_raw),
        document_in_quirks_mode,
    );
    // The boundary can be wider than the rebuilt roots that led to it, and laying it out may
    // re-enter intrinsic sizing for descendants outside those roots, including anonymous boxes
    // the incremental tree build created; derive the facts of the whole subtree.
    arena.derive_facts_in_subtree(root);
    drop_intrinsic_size_cache_entries_of_released_slots(arena, scratch);

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
    RunRecords::with_unrooted(
        callbacks,
        entry_root,
        entry_root_containing_block,
        |entry_records, callbacks| {
            let _trace = arena.layout_trace.pass(Some(root));
            let entry_fragments =
                std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new_entry_accumulator(entry_root));
            let entry_run = FormattingContextRun {
                purpose: LayoutPurpose::Commit,
                records: entry_records,
                box_: entry_root,
                layout_mode: LayoutMode::Normal,
                callbacks,
                should_collect_devtools_layout_data: false,
                treat_block_axis_percentage_insets_as_auto_beyond_root: false,
                fragments: Some(entry_fragments.clone()),
                previous_line_data: None,
                fragmentainer: None,
            };
            if root_is_absolutely_positioned {
                abspos_engine::AbsposEngine::for_run(&entry_run).replay(&entry_run, root);
            } else {
                layout_subtree_with_frozen_root_geometry(&entry_run);
            }
            finish_entry_pass(entry_records, &entry_fragments, &callbacks, false)
        },
    )
}

fn layout_subtree_with_frozen_root_geometry(run: &FormattingContextRun<'_>) {
    let root = run.box_;
    let callbacks = &run.callbacks;
    let root_used = run.records.register(
        root,
        used_values::used_values_from_committed_fragment_link(callbacks, root)
            .expect("partial relayout root must have committed geometry"),
    );
    let fragments = run.fragments.as_deref().expect("partial relayout must build fragments");
    let input = LayoutInput::new(
        AvailableSpace {
            inline_size: AvailableSize::definite(root_used.content_inline_size.get()),
            block_size: AvailableSize::definite(root_used.content_block_size.get()),
        },
        // The subtree root has definite sizes in both axes, so boxes
        // below it do not need inherited percentage constraints.
        ContainingBlockConstraints::default(),
        ParticipationInParentFormattingContext::Root,
    );

    let facts = NodeFacts::new(callbacks, root);
    debug_assert!(facts.is_svg_svg_box());
    let fc_type = formatting_context_type_created_by_box(facts)
        .expect("partial relayout root must establish an independent formatting context");
    run_formatting_context(
        run.purpose,
        Some(fragments),
        root_used,
        root,
        callbacks.in_flow_containing_block(root),
        None,
        fc_type,
        run.layout_mode,
        run.should_collect_devtools_layout_data,
        *callbacks,
        input,
        None,
        None,
    );
    // The retained offset already includes relative positioning. Publish it directly instead
    // of applying placement adjustments a second time.
    fragments.normalize_arrivals_for_placement(root, callbacks);
    fragments.build_fragment_for_placed_box(
        callbacks,
        root,
        None,
        root_used,
        false,
        None,
        root_used.content_offset.get(),
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
    use crate::layout::run_records::LayoutScratch;

    unsafe extern "C" fn unused_container_bases(
        _: *mut c_void,
        _: u32,
    ) -> svg_formatting_context::FfiContainerLengthBases {
        unreachable!("baseline derivation does not resolve container lengths")
    }

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
            layout_pass::ContainerLengthBasesQuery::new(std::ptr::null_mut(), unused_container_bases).sealed(),
            CssPixels::from_integer(800),
            CssPixels::from_integer(600),
            false,
        );
        let parent_used = UsedValues::default();
        RunRecords::with_root(callbacks, parent, Node::INVALID, &parent_used, |records, callbacks| {
            let child_used = records.register(child, UsedValues::default());
            store_derived_baselines(
                child_used,
                DerivedBaselines {
                    first: Some(CssPixels::from_integer(10)),
                    last: Some(CssPixels::from_integer(30)),
                    inline_alignment: None,
                },
            );
            let baselines = derive_baselines(records, &callbacks, parent);
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
