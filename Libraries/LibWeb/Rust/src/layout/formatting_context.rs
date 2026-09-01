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
    pub(crate) resolves_top: bool,
    pub(crate) top_is_auto: bool,
    pub(crate) top: CssPixels,
    pub(crate) resolves_right: bool,
    pub(crate) right_is_auto: bool,
    pub(crate) right: CssPixels,
    pub(crate) resolves_bottom: bool,
    pub(crate) bottom_is_auto: bool,
    pub(crate) bottom: CssPixels,
    pub(crate) resolves_left: bool,
    pub(crate) left_is_auto: bool,
    pub(crate) left: CssPixels,
}

impl ResolvedAnchorInsets {
    pub(crate) fn override_for(&self, field: style_values::InsetField) -> Option<style_values::ResolvedInsetOverride> {
        let (resolves, is_auto, px) = match field {
            style_values::InsetField::Top => (self.resolves_top, self.top_is_auto, self.top),
            style_values::InsetField::Right => (self.resolves_right, self.right_is_auto, self.right),
            style_values::InsetField::Bottom => (self.resolves_bottom, self.bottom_is_auto, self.bottom),
            style_values::InsetField::Left => (self.resolves_left, self.left_is_auto, self.left),
        };
        resolves.then_some(style_values::ResolvedInsetOverride { is_auto, px })
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct PhysicalRect {
    pub(crate) x: CssPixels,
    pub(crate) y: CssPixels,
    pub(crate) width: CssPixels,
    pub(crate) height: CssPixels,
}

impl PhysicalRect {
    pub(super) fn left(self) -> CssPixels {
        self.x
    }

    pub(super) fn top(self) -> CssPixels {
        self.y
    }

    pub(super) fn right(self) -> CssPixels {
        self.x + self.width
    }

    pub(super) fn bottom(self) -> CssPixels {
        self.y + self.height
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
}

impl LayoutPurpose {
    pub(crate) fn is_measurement(self) -> bool {
        self == LayoutPurpose::Measurement
    }
}

pub(crate) struct MeasurementState {
    callbacks: FfiLayoutFcCallbacks,
}

impl MeasurementState {
    pub(crate) fn create(callbacks: FfiLayoutFcCallbacks) -> Self {
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
        let fc_type = independent_formatting_context_type(node, &self.callbacks);
        run_formatting_context(
            LayoutPurpose::Measurement,
            None,
            node_used,
            node,
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

    pub(crate) fn callbacks(&self) -> &FfiLayoutFcCallbacks {
        &self.callbacks
    }

    pub(crate) fn create_used_values(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> std::rc::Rc<UsedValues> {
        used_values::create_used_values(&self.callbacks, node, constraints)
    }
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

pub(crate) fn place_child(
    run: &FormattingContextRun,
    node: Node,
    offset: FfiCssPixelPoint,
    containing_line_box_fragment: Option<used_values::LineBoxFragmentCoordinate>,
) {
    let purpose = run.purpose;
    let records = &*run.records;
    let callbacks = &run.callbacks;
    let fragments = run.fragments.as_deref();
    let used = records.used_values(node);
    assert!(!used.has_content_offset.get());
    used.has_content_offset.set(true);
    used.content_offset.set(offset);
    used.seal_committed_box_metrics();
    if let Some(fragments) = fragments {
        fragments.normalize_arrivals_for_placement(node);
        loop {
            let batch = fragments.take_drainable_abspos(node, records, callbacks);
            if batch.is_empty() {
                break;
            }
            let engine = abspos_engine::AbsposEngine::for_run(run);
            for entry in batch {
                engine.layout_pending_child(run, entry);
            }
        }
        let containing_block = callbacks.containing_block(node);
        let containing_block_is_sealed = !containing_block.is_invalid()
            && records
                .used_values_if_owned(containing_block)
                .is_none_or(|containing_block_used| containing_block_used.has_content_offset.get());
        let node_facts = NodeFacts::new(callbacks, node);
        let own_anchor_candidate_border_box_rect = (node_facts.is_box() && node_facts.has_anchor_names()).then(|| {
            let collapsed = used.uses_collapsing_borders_model.get();
            PhysicalRect {
                x: used.content_offset.get().x - used.border_box_left(collapsed),
                y: used.content_offset.get().y - used.border_box_top(collapsed),
                width: used.border_box_inline_size(collapsed),
                height: used.border_box_block_size(collapsed),
            }
        });
        fragments.build_fragment_for_placed_box(
            callbacks,
            node,
            (!containing_block.is_invalid()).then_some(containing_block),
            &used,
            containing_block_is_sealed,
            resolve_containing_line_box_index(
                records,
                callbacks,
                node,
                containing_block,
                containing_line_box_fragment,
                offset,
            ),
            point_add(
                offset,
                committed_offset_delta_at_placement(purpose, records, callbacks, node, containing_block, &used),
            ),
            own_anchor_candidate_border_box_rect,
        );
    }
}

/// The line box index to record for atomic inlines whose containing line
/// survived line post-processing.
fn resolve_containing_line_box_index(
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
    node: Node,
    containing_block: Node,
    coordinate: Option<used_values::LineBoxFragmentCoordinate>,
    placed_offset: FfiCssPixelPoint,
) -> Option<usize> {
    let coordinate = coordinate?;
    let facts = NodeFacts::new(callbacks, node);
    if !facts.is_non_fragmented_box() {
        return None;
    }
    assert!(!containing_block.is_invalid());
    let containing_block_used = records.used_values(containing_block);
    let data = containing_block_used.line_data_ref()?;
    let line = data.line_boxes.get(coordinate.line_box_index)?;
    if let Some(fragment) = line.fragments.get(coordinate.fragment_index) {
        let (x, y) = fragment.offset();
        debug_assert_eq!(
            FfiCssPixelPoint { x, y },
            placed_offset,
            "stored line fragment offset diverged from the placed offset (is_block_outside={})",
            facts.display().is_block_outside()
        );
    }
    Some(coordinate.line_box_index)
}

fn committed_offset_delta_at_placement(
    purpose: LayoutPurpose,
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
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
    callbacks: &FfiLayoutFcCallbacks,
    fragments: Option<&fragment_tree::RunFragmentBuilder>,
    coordinate_space_box: Node,
    child: Node,
    static_position_rect: abspos_inputs::StaticPositionRect,
    containing_block_info_override: Option<abspos_inputs::AbsposContainingBlockInfo>,
) {
    let Some(fragments) = fragments else {
        return;
    };
    if callbacks.containing_block(child).is_invalid() {
        return;
    }
    let inline_containing_block = callbacks.inline_containing_block(child);
    fragments.register_pending_abspos(
        coordinate_space_box,
        abspos_inputs::PendingAbsposChild {
            child_box: child,
            coordinate_space_box,
            static_position_rect,
            containing_block_info_override,
            inline_containing_block,
        },
    );
}

pub(crate) fn box_baseline(
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    used: &UsedValues,
    baseline_set: BaselineSet,
) -> CssPixels {
    box_baseline_with_content_baselines(callbacks, box_, used, baseline_set, used.content_baselines_from_cells())
}

pub(crate) fn box_baseline_with_content_baselines(
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    used: &UsedValues,
    mut baseline_set: BaselineSet,
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
                let containing_block = callbacks.containing_block(box_);
                assert!(!containing_block.is_invalid());
                let containing_style = StyleValues::for_node(callbacks, containing_block);
                return used.margin_box_block_size(collapsed) / 2
                    + CssPixels::nearest_value_for_f32(containing_style.font_x_height() / 2.0);
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
                let containing_block = callbacks.containing_block(box_);
                assert!(!containing_block.is_invalid());
                let containing_style = StyleValues::for_node(callbacks, containing_block);
                return used.margin_box_block_size(collapsed)
                    - CssPixels::nearest_value_for_f32(containing_style.font_descent());
            }
            _ => {}
        }
    }

    // https://drafts.csswg.org/css-inline-3/#baseline-source
    // auto: Specifies last-baseline alignment for inline-block, first-baseline alignment for everything else.
    // NB: Callers ask an inline-level box for its last baseline set, since that is what CSS2's inline-block rule below
    //     describes; inline-level flex and grid containers participate with their first baseline set instead.
    let display = facts.display();
    let is_flex_or_grid_container = display.is_flex_inside() || display.is_grid_inside();
    if display.is_inline_outside() && is_flex_or_grid_container {
        baseline_set = BaselineSet::First;
    }

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
    let has_visible_overflow = style.overflow_x() == overflow::VISIBLE && style.overflow_y() == overflow::VISIBLE;
    let derive_baseline_from_content =
        baseline_set == BaselineSet::First || is_flex_or_grid_container || has_visible_overflow;

    // AD-HOC: We also use the content-derived baseline for <input> elements with block children. Per the HTML spec,
    //         inputs have `overflow: clip !important`, so CSS2 says to use bottom margin edge. However, the internal
    //         shadow tree baseline should determine the control's baseline for proper alignment with adjacent text.
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

    // If the box has no baseline set, the bottom margin edge of the box is used.
    used.margin_box_block_size(collapsed)
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
}

// NOTE: A box can be baselined more than once (e.g. table cells are laid out
//       twice), so both presence bits are re-assigned on every store. The
//       payload cells are left untouched for an absent baseline so that
//       resetting the presence bits does not perturb payloads observed by the
//       existing derivation flow.
pub(crate) fn store_derived_baselines(used: &UsedValues, baselines: DerivedBaselines) {
    used.has_first_baseline.set(baselines.first.is_some());
    if let Some(value) = baselines.first {
        used.first_baseline.set(value);
    }
    used.has_last_baseline.set(baselines.last.is_some());
    if let Some(value) = baselines.last {
        used.last_baseline.set(value);
    }
}

pub(crate) fn derive_baselines(
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    inhibits_floating: bool,
) -> DerivedBaselines {
    let facts = NodeFacts::new(callbacks, box_);
    let own_used = records.used_values(box_);
    let own_line_data = own_used.line_data_ref();
    let line_count = own_line_data.as_ref().map_or(0, |data| data.line_boxes.len());
    if line_count > 0 {
        let baseline_for_line_box = |line_index: usize, baseline_set: BaselineSet| -> CssPixels {
            let (has_block_level_box, block_start, baseline, fragment_count, fragment_node) = {
                let line = &own_line_data.as_ref().unwrap().line_boxes[line_index];
                (
                    line.has_block_level_box,
                    line.physical_vertical_end() - line.block_length,
                    line.baseline,
                    line.fragments.len(),
                    line.fragments.first().map(|fragment| fragment.layout_node),
                )
            };
            if !has_block_level_box {
                return block_start + baseline;
            }

            assert_eq!(fragment_count, 1);
            let fragment_node = fragment_node.expect("block-level line must have one fragment");
            let block_child_state = records.used_values(fragment_node);
            let child_offset_from_margin_edge = block_child_state.content_offset.get().y
                - block_child_state.margin_box_top(block_child_state.uses_collapsing_borders_model.get());
            child_offset_from_margin_edge + box_baseline(callbacks, fragment_node, &block_child_state, baseline_set)
        };

        let mut first_line_index = 0;
        while first_line_index < line_count {
            let is_empty = own_line_data.as_ref().unwrap().line_boxes[first_line_index].is_empty();
            if !is_empty {
                break;
            }
            first_line_index += 1;
        }
        if first_line_index == line_count {
            first_line_index = 0;
        }
        let first_baseline = baseline_for_line_box(first_line_index, BaselineSet::First);

        let mut last_line_index = line_count - 1;
        while last_line_index > 0 {
            let is_empty = own_line_data.as_ref().unwrap().line_boxes[last_line_index].is_empty();
            if !is_empty {
                break;
            }
            last_line_index -= 1;
        }
        let last_baseline = baseline_for_line_box(last_line_index, BaselineSet::Last);
        return DerivedBaselines {
            first: Some(first_baseline),
            last: Some(last_baseline),
        };
    }

    if callbacks.first_child(box_).is_invalid() || facts.children_are_inline() {
        return DerivedBaselines::default();
    }

    // Derive baselines from the first/last in-flow child that has a baseline set of its own.
    // https://drafts.csswg.org/css-flexbox-1/#flex-baselines
    // Otherwise, if the flex container has at least one flex item, the flex container's first/last main-axis baseline
    // set is generated from the alignment baseline of the startmost/endmost flex item.
    // https://drafts.csswg.org/css-grid-1/#grid-baselines
    // Otherwise, the grid container's first (last) baseline set is generated from the alignment baseline of the first
    // (last) grid item in row-major grid order.
    // FIXME: This does not yet select the spec-defined startmost/endmost flex item, or the first/last grid item in
    //        row-major grid order.
    let container_display = facts.display();
    let container_skips_anonymous_whitespace_runs =
        container_display.is_flex_inside() || container_display.is_grid_inside();
    let baseline_from_children = |baseline_set: BaselineSet| -> Option<CssPixels> {
        let mut children = Vec::new();
        let mut child = callbacks.first_child(box_);
        while !child.is_invalid() {
            children.push(child);
            child = callbacks.next_sibling(child);
        }
        if baseline_set == BaselineSet::Last {
            children.reverse();
        }
        for child in children {
            let child_facts = NodeFacts::new(callbacks, child);
            if !child_facts.is_flow_layout_participant() || (!inhibits_floating && child_facts.is_floating()) {
                continue;
            }
            if !table_formatting_context::child_participates_in_table_run(container_display, &child_facts) {
                continue;
            }
            if container_skips_anonymous_whitespace_runs && callbacks.can_skip_is_anonymous_text_run(child) {
                continue;
            }
            let child_state = records.used_values(child);
            match baseline_set {
                BaselineSet::First if child_state.has_first_baseline.get() => {}
                BaselineSet::Last if child_state.has_last_baseline.get() => {}
                _ => continue,
            }
            let child_offset_from_margin_edge = child_state.content_offset.get().y
                - child_state.margin_box_top(child_state.uses_collapsing_borders_model.get());
            return Some(child_offset_from_margin_edge + box_baseline(callbacks, child, &child_state, baseline_set));
        }
        None
    };
    DerivedBaselines {
        first: baseline_from_children(BaselineSet::First),
        last: baseline_from_children(BaselineSet::Last),
    }
}

const NO_FORMATTING_CONTEXT: u8 = u8::MAX;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiFormattingContextType {
    Block,
    Inline,
    Flex,
    Grid,
    Table,
    Svg,
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
    pub automatic_content_inline_size: CssPixels,
    pub min_content_inline_size_from_max_content_layout: Option<CssPixels>,
    pub automatic_content_block_size: CssPixels,
    pub baselines: DerivedBaselines,
    pub table_box_in_wrapper_border_box_block_size: Option<CssPixels>,
    pub depends_on_percentage_block_size: bool,
}

#[derive(Clone)]
pub(crate) struct RunRootOutcome {
    pub(super) cells: used_values::UsedValuesCellState,
    pub(super) own_metrics_sealed: bool,
    pub(super) line_data: Option<std::rc::Rc<used_values::LineData>>,
    pub(super) rare: Option<used_values::UsedValuesRareData>,
}

impl RunRootOutcome {
    fn apply_to_record(self, record: &UsedValues) {
        self.cells.apply_to_record(record);
        if let Some(line_data) = self.line_data {
            *record.line_data_cell().borrow_mut() = line_data;
        }
        if let Some(rare) = self.rare {
            rare.install_present_payloads_into(record);
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

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct FlexLayoutItemRect {
    pub(crate) x: CssPixels,
    pub(crate) y: CssPixels,
    pub(crate) width: CssPixels,
    pub(crate) height: CssPixels,
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
    pub(crate) node_id: Option<i64>,
    pub(crate) rect: FlexLayoutItemRect,
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

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutFcCallbacks {
    pub context: *mut c_void,
    pub arena: *mut c_void,
    pub initial_containing_block_inline_size: CssPixels,
    pub document_in_quirks_mode: bool,
    pub report_unexpected_fragmented_inline: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub build_svg_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> svg_formatting_context::FfiSvgElementFacts,
    pub compute_svg_path: unsafe extern "C" fn(
        *mut c_void,
        *mut c_void,
        svg_formatting_context::FfiSvgPathRequest,
    ) -> svg_formatting_context::FfiSvgPathResult,
    pub svg_image_bounding_box:
        unsafe extern "C" fn(*mut c_void, *mut c_void, CssPixels, CssPixels) -> svg_formatting_context::FfiFloatRect,
    pub anchor_lookup: unsafe extern "C" fn(*mut c_void, *mut c_void, usize, *const *mut c_void, usize) -> NodeSlotId,
    pub node_unique_id: unsafe extern "C" fn(*mut c_void) -> i64,
}

impl FfiLayoutFcCallbacks {
    pub(crate) fn arena(&self) -> &LayoutNodeArena {
        // SAFETY: C++ borrows the document arena for the synchronous layout
        // pass represented by this callback table.
        unsafe { LayoutNodeArena::from_handle(self.arena) }
    }

    pub(crate) fn node_data(&self, node: Node) -> &NodeData {
        self.arena().data(node)
    }

    pub(crate) fn text_content(&self, node: Node) -> &'static crate::layout::layout_node_arena::TextContent {
        let content = self
            .arena()
            .text_content(node)
            .expect("text node content must be synced to the arena before layout");
        // SAFETY: The document arena outlives the layout pass, and text
        // content is only mutated between passes.
        unsafe { &*std::ptr::from_ref(content) }
    }

    pub(crate) fn style_payloads(&self, node: Node) -> &'static crate::layout::FfiStylePayloads {
        let payloads = self
            .arena()
            .style_payloads(node)
            .expect("styled node must publish its style container before layout");
        // SAFETY: The node's ComputedValues keep the style container alive
        // for the pass, and the container is only replaced between passes:
        // set_computed_values verifies no pass is running and no layout node
        // is created mid-pass.
        unsafe { &*std::ptr::from_ref(payloads) }
    }

    pub(crate) fn replaced_content_facts(&self, node: Node) -> Option<crate::layout::FfiReplacedContentFacts> {
        self.arena().replaced_content_facts(node)
    }

    pub(crate) fn computed_values_view_if_styled(&self, node: Node) -> Option<ComputedValuesView<'static>> {
        let payloads = self.arena().style_payloads(node)?;
        // SAFETY: The node's ComputedValues keep the style container alive
        // for the pass, and the container is only replaced between passes:
        // set_computed_values verifies no pass is running and no layout node
        // is created mid-pass.
        let payloads: &'static FfiStylePayloads = unsafe { &*std::ptr::from_ref(payloads) };
        Some(ComputedValuesView::new(&payloads.groups))
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

    pub(crate) fn shell(&self, node: Node) -> *mut c_void {
        let shell = self.arena().node_shell(node);
        assert!(!shell.is_null());
        shell
    }

    #[inline]
    pub(crate) fn slot_index(&self, node: Node) -> u32 {
        node.slot_index()
    }

    pub(crate) fn is_before(&self, node: Node, other: Node) -> bool {
        self.arena().is_before(self.node_data(node), self.node_data(other))
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, node: Node) -> Option<abspos_inputs::AbsposLayoutInputs> {
        let data = self.arena().data(node);
        assert!(node_facts::kind_is_box(data.kind.get()));
        self.arena().saved_abspos_layout_inputs(data)
    }

    pub(crate) fn committed_fragment_link(&self, node: Node) -> Option<FragmentLink> {
        self.arena().committed_fragment_link(self.arena().data(node))
    }

    pub(crate) fn set_committed_fragment_link(&self, node: Node, link: FragmentLink) {
        self.arena().set_committed_fragment_link(self.arena().data(node), link);
    }

    #[inline]
    pub(crate) fn has_committed_fragment_link(&self, node: Node) -> bool {
        self.node_data(node).flags.get() & NodeFlag::HasCommittedFragmentLink as u32 != 0
    }

    pub(crate) fn set_saved_abspos_layout_inputs(&self, node: Node, inputs: Option<abspos_inputs::AbsposLayoutInputs>) {
        let data = self.arena().data(node);
        // Match prepare_node's former as_if<Box>() guard.
        if !node_facts::kind_is_box(data.kind.get()) {
            return;
        }
        self.arena().set_saved_abspos_layout_inputs(data, inputs);
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
    pub(crate) fn next_sibling(&self, node: Node) -> Node {
        self.node_data(node).next_sibling.get()
    }

    #[inline]
    pub(crate) fn containing_block(&self, node: Node) -> Node {
        self.node_data(node).containing_block.get()
    }

    #[inline]
    pub(crate) fn inline_containing_block(&self, node: Node) -> Node {
        self.node_data(node).inline_containing_block.get()
    }

    pub(crate) fn is_ancestor(&self, ancestor: Node, mut node: Node) -> bool {
        while !node.is_invalid() {
            if node == ancestor {
                return true;
            }
            node = self.node_data(node).parent.get();
        }
        false
    }

    pub(crate) fn non_anonymous_containing_block(&self, node: Node) -> Node {
        let mut containing_block = self.node_data(node).containing_block.get();
        assert!(!containing_block.is_invalid());
        while self.node_data(containing_block).flags.get() & NodeFlag::Anonymous as u32 != 0 {
            containing_block = self.node_data(containing_block).containing_block.get();
            assert!(!containing_block.is_invalid());
        }
        containing_block
    }
}

pub(crate) struct FormattingContextRun {
    pub(crate) purpose: LayoutPurpose,
    pub(crate) records: std::rc::Rc<RunRecords>,
    pub(crate) box_: Node,
    pub(crate) layout_mode: LayoutMode,
    pub(crate) callbacks: FfiLayoutFcCallbacks,
    pub(crate) should_collect_devtools_layout_data: bool,
    pub(crate) treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    pub(crate) fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
    pub(crate) previous_line_data: Option<std::rc::Rc<used_values::LineData>>,
}

impl FormattingContextRun {
    pub(crate) fn sizing(&self) -> sizing_context::SizingContext {
        sizing_context::SizingContext::new(self.purpose, self.records.clone(), self.callbacks)
    }

    pub(crate) fn outputs(
        &self,
        result: ChildLayoutResult,
        root: Option<fragment_tree::UnplacedRootFragment>,
        atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above: Option<CssPixels>,
    ) -> RunOutputs {
        let record = self.records.used_values(self.box_);
        let root_outcome = RunRootOutcome {
            cells: used_values::UsedValuesCellState::capture(&record),
            own_metrics_sealed: record.own_metrics_are_sealed(),
            line_data: record.line_data.get().map(std::cell::RefCell::take),
            rare: record.rare_data.get().map(std::cell::RefCell::take),
        };
        RunOutputs {
            result,
            root,
            root_outcome,
            atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
        }
    }
}

enum FormattingContextImplementation<'pass> {
    Block(Box<block_formatting_context::BlockFormattingContext>),
    Flex(Box<flex_formatting_context::FlexFormattingContext<'pass>>),
    Grid(Box<grid_formatting_context::GridFormattingContext>),
    Table(Box<table_formatting_context::TableFormattingContext>),
    Svg(Box<svg_formatting_context::SvgFormattingContext>),
    ReplacedWithChildren,
    InternalReplaced,
    InternalDummy,
}

pub(crate) fn formatting_context_type_created_by_node_data(
    data: &NodeData,
    style: Option<ComputedValuesView<'_>>,
    parent_style: Option<ComputedValuesView<'_>>,
) -> Option<FfiFormattingContextType> {
    if data.kind.get() == crate::layout::node_data::NodeKind::SVGSVGBox {
        return Some(FfiFormattingContextType::Svg);
    }
    let is_replaced_box = node_facts::kind_is_replaced_box(data.kind.get());
    let can_have_children = node_facts::node_can_have_children(data);
    if is_replaced_box && can_have_children {
        return Some(FfiFormattingContextType::ReplacedWithChildren);
    }
    if is_replaced_box {
        return Some(FfiFormattingContextType::InternalReplaced);
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
            FfiFormattingContextType::Block
        } else {
            FfiFormattingContextType::InternalReplaced
        });
    }
    let display = style.map(|style| style.display());
    if display.is_some_and(|display| display.is_flex_inside()) {
        return Some(FfiFormattingContextType::Flex);
    }
    if display.is_some_and(|display| display.is_table_inside()) {
        return Some(FfiFormattingContextType::Table);
    }
    if display.is_some_and(|display| display.is_grid_inside()) {
        return Some(FfiFormattingContextType::Grid);
    }
    if display.is_some_and(|display| display.is_math_inside())
        || node_facts::node_creates_block_formatting_context(data, style, parent_style)
    {
        return Some(FfiFormattingContextType::Block);
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
        return Some(FfiFormattingContextType::InternalDummy);
    }
    None
}

pub(crate) fn formatting_context_type_created_by_box(facts: NodeFacts<'_>) -> Option<FfiFormattingContextType> {
    formatting_context_type_created_by_node_data(
        facts.data(),
        facts.computed_values_view_if_styled(),
        facts.parent_computed_values_view_if_styled(),
    )
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiFormattingContextArenaFacts {
    pub arena: *mut c_void,
    pub node: NodeSlotId,
}

#[unsafe(no_mangle)]
pub extern "C" fn rust_layout_formatting_context_type_for_box(facts: FfiFormattingContextArenaFacts) -> u8 {
    // SAFETY: The C++ caller borrows the box's arena for this synchronous
    // classification.
    let arena = unsafe { LayoutNodeArena::from_handle(facts.arena) };
    let data = arena.data(facts.node);
    let style = arena
        .style_payloads(facts.node)
        .map(|payloads| ComputedValuesView::new(&payloads.groups));
    let parent_style = (!data.parent.get().is_invalid())
        .then(|| arena.style_payloads(data.parent.get()))
        .flatten()
        .map(|payloads| ComputedValuesView::new(&payloads.groups));
    formatting_context_type_created_by_node_data(data, style, parent_style)
        .map(|type_| type_ as u8)
        .unwrap_or(NO_FORMATTING_CONTEXT)
}

fn create_formatting_context_implementation<'pass>(
    run: &FormattingContextRun,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    fc_type: FfiFormattingContextType,
) -> FormattingContextImplementation<'pass> {
    match fc_type {
        FfiFormattingContextType::Block => {
            FormattingContextImplementation::Block(Box::new(block_formatting_context::BlockFormattingContext::new(run)))
        }
        FfiFormattingContextType::Flex => {
            FormattingContextImplementation::Flex(Box::new(flex_formatting_context::FlexFormattingContext::new(run)))
        }
        FfiFormattingContextType::Grid => FormattingContextImplementation::Grid(Box::new(
            grid_formatting_context::GridFormattingContext::new(run, parent_grid),
        )),
        FfiFormattingContextType::Table => {
            FormattingContextImplementation::Table(Box::new(table_formatting_context::TableFormattingContext::new(run)))
        }
        FfiFormattingContextType::Svg => {
            FormattingContextImplementation::Svg(Box::new(svg_formatting_context::SvgFormattingContext::new(run)))
        }
        FfiFormattingContextType::ReplacedWithChildren => FormattingContextImplementation::ReplacedWithChildren,
        FfiFormattingContextType::InternalReplaced => FormattingContextImplementation::InternalReplaced,
        FfiFormattingContextType::InternalDummy => FormattingContextImplementation::InternalDummy,
        FfiFormattingContextType::Inline => panic!("no Rust implementation for inline formatting contexts"),
    }
}

fn register_table_abspos_descendants(run: &FormattingContextRun, parent: Node) {
    let mut child = run.callbacks.first_child(parent);
    while !child.is_invalid() {
        let next = run.callbacks.next_sibling(child);
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
        child = next;
    }
}

pub(crate) fn independent_root_automatic_block_size(
    purpose: LayoutPurpose,
    records: &std::rc::Rc<RunRecords>,
    callbacks: &FfiLayoutFcCallbacks,
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
        return sizing_context::SizingContext::new(purpose, records.clone(), *callbacks)
            .calculate_max_content_block_size(node, available_inner_space.inline_size.to_px_or_zero(), constraints);
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
    if !style.display().is_flex_inside() || !style.min_height().is_auto() {
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
                &run.records,
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
    fc_type: FfiFormattingContextType,
) -> RootSizingOutcome {
    let mut atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above = None;
    let body_input = match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => dimension_block_level_root(run, input),
        ParticipationInParentFormattingContext::Float => body_input_with_inner_available_space(run, input),
        ParticipationInParentFormattingContext::AtomicInline => {
            let run_cache_may_store_this_block_formatting_context_run = fc_type == FfiFormattingContextType::Block
                && run.layout_mode == LayoutMode::Normal
                && !run.purpose.is_measurement()
                && fc_run_cache::fc_run_cache_mode_from_environment() != fc_run_cache::FcRunCacheMode::Disabled;
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

fn dimension_block_level_root(run: &FormattingContextRun, input: &LayoutInput) -> LayoutInput {
    let node = run.box_;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let sizing = run.sizing();
    let style = StyleValues::for_node(&run.callbacks, node);
    let mut body_input = body_input_with_inner_available_space(run, input);
    let mut measured_content_block_size = None;
    if sizing.should_treat_block_size_as_auto(node, available_space, constraints) && !style.min_height().is_auto() {
        let content_block_size =
            sizing.measure_automatic_content_block_size(node, run.layout_mode, body_input.available_space, constraints);
        measured_content_block_size = Some(content_block_size);
        let min_block_size = sizing.calculate_inner_block_size(node, available_space, style.min_height(), constraints);
        if content_block_size < min_block_size {
            body_input.available_space.block_size = AvailableSize::definite(min_block_size);
        }
    }
    sizing.make_button_content_box_definite(
        node,
        run.layout_mode,
        available_space,
        constraints,
        measured_content_block_size,
    );
    body_input
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
    if !style.display().is_table_inside() {
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
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    fc_type: FfiFormattingContextType,
    layout_mode: LayoutMode,
    should_collect_devtools_layout_data: bool,
    callbacks: FfiLayoutFcCallbacks,
    input: LayoutInput,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    table_inline_layout: Option<table_formatting_context::TableInlineLayout>,
) -> ChildLayoutResult {
    let root_cells = used_values::UsedValuesCellState::capture(parent_used);
    let cache_attempt = match fc_run_cache::FcRunCacheAttempt::probe(
        purpose,
        box_,
        parent_grid.is_some(),
        fc_type,
        layout_mode,
        should_collect_devtools_layout_data,
        &callbacks,
        &input,
        &root_cells,
    ) {
        Ok(attempt) => attempt,
        Err(entry) => {
            let reuses_committed_subtree = entry.can_reuse_committed_subtree();
            let outputs = if reuses_committed_subtree {
                entry.outputs_for_reused_subtree()
            } else {
                entry.outputs.clone()
            };
            return absorb_run_outputs(parent_fragments, parent_used, box_, outputs, reuses_committed_subtree);
        }
    };
    if let Some(parent_fragments) = parent_fragments {
        // A later fresh run for this root supersedes a hit recorded earlier in the same pass.
        parent_fragments.clear_reused_subtree_root(box_);
    }
    let previous_line_data = cache_attempt.previous_line_data();
    let outputs = execute_formatting_context_run(
        purpose,
        root_cells,
        box_,
        parent_grid,
        fc_type,
        layout_mode,
        should_collect_devtools_layout_data,
        callbacks,
        input,
        parent_block,
        previous_line_data,
        table_inline_layout,
    );
    cache_attempt.conclude(&callbacks, box_, &outputs);
    absorb_run_outputs(parent_fragments, parent_used, box_, outputs, false)
}

#[expect(clippy::too_many_arguments)]
fn execute_formatting_context_run(
    purpose: LayoutPurpose,
    root_cells: used_values::UsedValuesCellState,
    box_: Node,
    parent_grid: Option<&grid_formatting_context::GridFormattingContext>,
    fc_type: FfiFormattingContextType,
    layout_mode: LayoutMode,
    should_collect_devtools_layout_data: bool,
    callbacks: FfiLayoutFcCallbacks,
    input: LayoutInput,
    parent_block: Option<&block_formatting_context::BlockFormattingContext>,
    previous_line_data: Option<std::rc::Rc<used_values::LineData>>,
    table_inline_layout: Option<table_formatting_context::TableInlineLayout>,
) -> RunOutputs {
    assert!(!box_.is_invalid());
    let root_used = std::rc::Rc::new(root_cells.materialize_record());
    let run = FormattingContextRun {
        purpose,
        records: std::rc::Rc::new(RunRecords::new(callbacks.arena, box_, root_used)),
        box_,
        layout_mode,
        callbacks,
        should_collect_devtools_layout_data,
        treat_block_axis_percentage_insets_as_auto_beyond_root: input
            .sizing
            .treat_block_axis_percentage_insets_as_auto_beyond_root,
        fragments: (layout_mode == LayoutMode::Normal && !purpose.is_measurement()).then(|| {
            let root_containing_block = callbacks.containing_block(box_);
            std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new(
                box_,
                (!root_containing_block.is_invalid()).then_some(root_containing_block),
            ))
        }),
        previous_line_data,
    };
    let run = &run;
    if let Some(table_inline_layout) = table_inline_layout {
        run.records
            .store_table_inline_layout(table_inline_layout.table_box(), table_inline_layout);
    }
    let RootSizingOutcome {
        body_input,
        atomic_root_sizing_repeats_for_available_inline_sizes_at_or_above,
    } = apply_root_sizing_directives(run, &input, fc_type);

    let cached_atomic_block_size = if matches!(
        input.participation,
        ParticipationInParentFormattingContext::AtomicInline
    ) {
        run.sizing().apply_cached_intrinsic_inline_measurement(
            run.box_,
            input.available_space.inline_size,
            body_input.available_space.block_size,
            input.containing_block_constraints,
        )
    } else {
        None
    };
    let mut implementation = None;
    let mut result = if let Some((cached_block_size, cached_baselines)) = cached_atomic_block_size {
        ChildLayoutResult {
            automatic_content_block_size: cached_block_size,
            baselines: cached_baselines,
            ..ChildLayoutResult::default()
        }
    } else if layout_mode == LayoutMode::Normal
        && !purpose.is_measurement()
        && matches!(
            input.participation,
            ParticipationInParentFormattingContext::AtomicInline
        )
        && fc_type == FfiFormattingContextType::Block
        && callbacks.first_child(box_).is_invalid()
    {
        // An empty atomic block context has no body output. Root sizing and finalization still
        // run through the shared paths around this branch.
        ChildLayoutResult::default()
    } else {
        let mut context_implementation = create_formatting_context_implementation(run, parent_grid, fc_type);
        let result = match &mut context_implementation {
            FormattingContextImplementation::Block(context) => {
                context.run(run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                store_derived_baselines(&run.records.used_values(run.box_), baselines);
                ChildLayoutResult {
                    automatic_content_inline_size: context.automatic_content_inline_size(),
                    min_content_inline_size_from_max_content_layout: context
                        .min_content_inline_size_from_max_content_layout(),
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    table_box_in_wrapper_border_box_block_size: context.table_box_in_wrapper_border_box_block_size(),
                    depends_on_percentage_block_size: false,
                }
            }
            FormattingContextImplementation::Flex(context) => {
                context.run(run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                store_derived_baselines(&run.records.used_values(run.box_), baselines);
                ChildLayoutResult {
                    automatic_content_inline_size: context.automatic_content_inline_size(),
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    ..ChildLayoutResult::default()
                }
            }
            FormattingContextImplementation::Grid(context) => {
                context.run(run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                store_derived_baselines(&run.records.used_values(run.box_), baselines);
                ChildLayoutResult {
                    automatic_content_inline_size: context.automatic_content_inline_size(),
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    ..ChildLayoutResult::default()
                }
            }
            FormattingContextImplementation::Table(context) => {
                context.run(run, body_input, run.records.take_table_inline_layout(run.box_));
                let baselines = context.derived_baselines_of_root_box();
                store_derived_baselines(&run.records.used_values(run.box_), baselines);
                ChildLayoutResult {
                    automatic_content_inline_size: context.automatic_content_inline_size(),
                    automatic_content_block_size: context.automatic_content_block_size,
                    baselines,
                    ..ChildLayoutResult::default()
                }
            }
            FormattingContextImplementation::Svg(context) => {
                context.run(run, body_input);
                ChildLayoutResult::default()
            }
            FormattingContextImplementation::ReplacedWithChildren => {
                replaced_with_children_formatting_context::layout_replaced_with_children(run, body_input)
            }
            FormattingContextImplementation::InternalReplaced | FormattingContextImplementation::InternalDummy => {
                ChildLayoutResult::default()
            }
        };
        implementation = Some(context_implementation);
        result
    };

    // https://drafts.csswg.org/css-sizing-4/#intrinsic-size-override
    // If an element has an explicit intrinsic inner size in an axis, then after laying out the element as normal for
    // size containment, the size of the contents in that axis are instead treated as being the explicit intrinsic inner
    // size instead of what was calculated in layout, and layout is performed again if necessary.
    //
    // Every formatting context reports its content sizes through ChildLayoutResult, so overriding here covers them all
    // rather than one context's root-height path.
    let containment_facts = NodeFacts::new(&run.callbacks, run.box_);
    if containment_facts.node_has_size_containment() {
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
    }

    match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => {
            finalize_block_level_root(run, &input, &result);
        }
        ParticipationInParentFormattingContext::Float => {
            finalize_float_root(run, &input, &result);
        }
        ParticipationInParentFormattingContext::AtomicInline => {
            let automatic_content_block_size_of_completed_body_run = cached_atomic_block_size
                .is_none()
                .then_some(result.automatic_content_block_size);
            finalize_atomic_root_block_size(
                run,
                &input,
                cached_atomic_block_size.map(|(block_size, _)| block_size),
                automatic_content_block_size_of_completed_body_run,
                parent_block,
            );
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
        }
        ParticipationInParentFormattingContext::Root => {}
    }
    result.depends_on_percentage_block_size = run.sizing().resolve_percentage_block_size_dependency(run.box_);

    let take_run_fragments = || {
        run.fragments
            .as_ref()
            .map(|fragments| fragments.take_unplaced_root(&run.records, &run.callbacks))
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
                register_table_abspos_descendants(run, box_);
            }
            FormattingContextImplementation::Flex(context) => {
                context.parent_did_dimension();
            }
            FormattingContextImplementation::Grid(context) => {
                context.parent_did_dimension();
            }
            FormattingContextImplementation::Svg(_) | FormattingContextImplementation::ReplacedWithChildren => {}
            FormattingContextImplementation::InternalReplaced | FormattingContextImplementation::InternalDummy => {
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
}

fn finalize_atomic_root_block_size(
    run: &FormattingContextRun,
    input: &LayoutInput,
    cached_intrinsic_measurement_block_size: Option<CssPixels>,
    automatic_content_block_size_of_completed_body_run: Option<CssPixels>,
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
        sizing.resolve_used_block_size_if_treated_as_auto(
            node,
            available_space,
            constraints,
            cached_intrinsic_measurement_block_size,
            || {
                let available_inner_space = run
                    .records
                    .used_values(node)
                    .available_inner_space_or_constraints_from(available_space);
                match parent_block {
                    Some(parent) => parent.compute_automatic_block_size_for_block_level_element(
                        node,
                        available_inner_space,
                        constraints,
                        automatic_content_block_size_of_completed_body_run,
                    ),
                    None => independent_root_automatic_block_size(
                        run.purpose,
                        &run.records,
                        &run.callbacks,
                        node,
                        available_inner_space,
                        constraints,
                        automatic_content_block_size_of_completed_body_run,
                    ),
                }
            },
        );
    } else {
        sizing.resolve_used_block_size_if_not_treated_as_auto(node, available_space, constraints);
    }
}

pub(crate) fn propagate_percentage_block_size_dependency_to_containing_block(
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
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
    let containing_block = callbacks.containing_block(child);
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
            &run.records,
            &run.callbacks,
            child,
            run.sizing().skipped_child_depends_on_percentage_block_size(child),
        );
    };
    if !force_independent_context_run
        && layout_mode == LayoutMode::IntrinsicSizing
        && matches!(
            input.participation,
            ParticipationInParentFormattingContext::AtomicInline
        )
        && formatting_context_type_created_by_box(facts) == Some(FfiFormattingContextType::Block)
        && run.callbacks.first_child(child).is_invalid()
    {
        // OPTIMIZATION: An empty atomic block has no formatting-context body output. Size it directly in the
        // parent measurement instead of constructing a child run for both min-content and max-content layout.
        let sizing = run.sizing();
        sizing.dimension_atomic_root(
            child,
            input.available_space,
            input.containing_block_constraints,
            layout_mode,
            false,
        );
        sizing.resolve_used_block_size_if_treated_as_auto(
            child,
            input.available_space,
            input.containing_block_constraints,
            Some(CssPixels::default()),
            || unreachable!("an empty atomic block has a zero automatic content block size"),
        );
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
        Some(FfiFormattingContextType::InternalReplaced | FfiFormattingContextType::ReplacedWithChildren)
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
    input.sizing.treat_block_axis_percentage_insets_as_auto_beyond_root =
        treat_block_axis_percentage_insets_as_auto_beyond_anonymous_child_root(
            &run.records,
            &run.callbacks,
            child,
            run.box_,
            run.treat_block_axis_percentage_insets_as_auto_beyond_root,
        );
    input.sizing.flex_self_block_size_resolution_space = dimension_root_in_parent_scope(
        parent_block,
        child,
        &input,
        layout_mode == LayoutMode::Normal && !run.purpose.is_measurement(),
    );
    let result = run_formatting_context(
        run.purpose,
        run.fragments.as_deref(),
        &used,
        child,
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
        &run.records,
        &run.callbacks,
        child,
        result.depends_on_percentage_block_size,
    );
    ChildLayoutOutcome::Created(result)
}

fn absorb_run_outputs(
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
    } = outputs;
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

pub(super) fn independent_formatting_context_type(
    box_: Node,
    callbacks: &FfiLayoutFcCallbacks,
) -> FfiFormattingContextType {
    let facts = NodeFacts::new(callbacks, box_);
    if let Some(fc_type) = formatting_context_type_created_by_box(facts) {
        return fc_type;
    }
    if facts.is_block_container() {
        return FfiFormattingContextType::Block;
    }

    // HACK: Instead of crashing in scenarios that assume the formatting context can be created, create a dummy formatting context that does nothing.
    eprintln!(
        "FIXME: An independent formatting context was requested from a Box that does not have a formatting context type. A dummy formatting context will be created instead."
    );
    FfiFormattingContextType::InternalDummy
}

pub(crate) fn resolve_block_axis_percentage_inset_basis_is_definite(
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
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
        candidate = callbacks.containing_block(candidate);
    }
    true
}

pub(crate) fn treat_block_axis_percentage_insets_as_auto_beyond_anonymous_child_root(
    records: &RunRecords,
    callbacks: &FfiLayoutFcCallbacks,
    child_root: Node,
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
        callbacks.containing_block(child_root),
        formatting_context_root,
        treat_block_axis_percentage_insets_as_auto_beyond_root,
    )
}

/// # Safety
///
/// The callback table and commit sink must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_run_root_layout(
    root: NodeSlotId,
    viewport_inline_size_raw: i32,
    viewport_block_size_raw: i32,
    should_collect_devtools_layout_data: bool,
    callbacks: *const FfiLayoutFcCallbacks,
    sink: *const commit::FfiCommitSink,
) {
    assert!(!root.is_invalid());
    assert!(!callbacks.is_null());
    assert!(!sink.is_null());
    // SAFETY: The C++ pass host keeps both callback tables live for this
    // synchronous entry.
    let callbacks = unsafe { *callbacks };
    let sink = unsafe { &*sink };
    let viewport_inline_size = CssPixels::from_raw(viewport_inline_size_raw);
    let viewport_block_size = CssPixels::from_raw(viewport_block_size_raw);

    let root_constraints = ContainingBlockConstraints {
        percentage_basis_inline_size: Some(viewport_inline_size),
        percentage_basis_block_size: Some(viewport_block_size),
        ..ContainingBlockConstraints::default()
    };
    let entry_records = std::rc::Rc::new(RunRecords::new_unrooted(callbacks.arena, root));
    let viewport_used = entry_records.create_used_values(&callbacks, root, root_constraints);
    let entry_fragments = std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new_entry_accumulator(root));
    let entry_run = FormattingContextRun {
        purpose: LayoutPurpose::Commit,
        records: entry_records.clone(),
        box_: root,
        layout_mode: LayoutMode::Normal,
        callbacks,
        should_collect_devtools_layout_data,
        treat_block_axis_percentage_insets_as_auto_beyond_root: false,
        fragments: Some(entry_fragments.clone()),
        previous_line_data: None,
    };

    let mut root_for_layout = root;
    let mut root_for_layout_used = viewport_used.clone();
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
        &root_for_layout_used,
        root_for_layout,
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
    drain_and_commit_entry_pass(
        &entry_records,
        &entry_fragments,
        &callbacks,
        should_collect_devtools_layout_data,
        root,
        sink,
    );
    callbacks.arena().sweep_stale_fc_run_cache_entries();
}

fn drain_and_commit_entry_pass(
    entry_records: &std::rc::Rc<RunRecords>,
    entry_fragments: &std::rc::Rc<fragment_tree::RunFragmentBuilder>,
    callbacks: &FfiLayoutFcCallbacks,
    should_collect_devtools_layout_data: bool,
    commit_root: Node,
    sink: &commit::FfiCommitSink,
) {
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
    commit::commit_replacing(commit_root, callbacks, sink, &pass_fragments);
}

/// # Safety
///
/// The callback table and commit sink must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_compute_subtree_layout(
    root: NodeSlotId,
    viewport: NodeSlotId,
    viewport_inline_size_raw: i32,
    viewport_block_size_raw: i32,
    callbacks: *const FfiLayoutFcCallbacks,
    sink: *const commit::FfiCommitSink,
) {
    assert!(!root.is_invalid());
    assert!(!callbacks.is_null());
    assert!(!sink.is_null());
    // SAFETY: The C++ pass host keeps both callback tables live for this
    // synchronous entry.
    let callbacks = unsafe { *callbacks };
    let sink = unsafe { &*sink };

    let entry_records = std::rc::Rc::new(RunRecords::new_unrooted(callbacks.arena, root));
    let root_used = used_values::used_values_from_committed_fragment_link(&callbacks, root)
        .expect("partial relayout root must have committed geometry");
    entry_records.register(root, root_used.clone());
    let entry_fragments = std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new_entry_accumulator(root));
    let entry_run = FormattingContextRun {
        purpose: LayoutPurpose::Commit,
        records: entry_records.clone(),
        box_: root,
        layout_mode: LayoutMode::Normal,
        callbacks,
        should_collect_devtools_layout_data: false,
        treat_block_axis_percentage_insets_as_auto_beyond_root: false,
        fragments: Some(entry_fragments.clone()),
        previous_line_data: None,
    };
    if !viewport.is_invalid() && viewport != root {
        let viewport_inline_size = CssPixels::from_raw(viewport_inline_size_raw);
        let viewport_block_size = CssPixels::from_raw(viewport_block_size_raw);
        let viewport_constraints = ContainingBlockConstraints {
            percentage_basis_inline_size: Some(viewport_inline_size),
            percentage_basis_block_size: Some(viewport_block_size),
            ..ContainingBlockConstraints::default()
        };
        let viewport_used = entry_records.create_used_values(&callbacks, viewport, viewport_constraints);
        viewport_used.set_content_inline_size(viewport_inline_size);
        viewport_used.set_content_block_size(viewport_block_size);
        place_child(&entry_run, viewport, FfiCssPixelPoint::default(), None);
    }
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

    let facts = NodeFacts::new(&callbacks, root);
    let fc_type = formatting_context_type_created_by_box(facts)
        .expect("partial relayout root must establish an independent formatting context");
    run_formatting_context(
        LayoutPurpose::Commit,
        Some(&entry_fragments),
        &root_used,
        root,
        None,
        fc_type,
        LayoutMode::Normal,
        false,
        callbacks,
        input,
        None,
        None,
    );
    entry_fragments.normalize_arrivals_for_placement(root);
    entry_fragments.build_fragment_for_placed_box(
        &callbacks,
        root,
        None,
        &root_used,
        false,
        None,
        root_used.content_offset.get(),
        None,
    );
    drain_and_commit_entry_pass(&entry_records, &entry_fragments, &callbacks, false, root, sink);
    callbacks.arena().sweep_stale_fc_run_cache_entries();
}

/// # Safety
///
/// The callback table and commit sink must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_replay_saved_abspos_layout(
    box_: NodeSlotId,
    callbacks: *const FfiLayoutFcCallbacks,
    sink: *const commit::FfiCommitSink,
) {
    assert!(!box_.is_invalid());
    assert!(!callbacks.is_null());
    assert!(!sink.is_null());
    // SAFETY: The C++ pass host keeps both callback tables live for this
    // synchronous entry.
    let callbacks = unsafe { *callbacks };
    let sink = unsafe { &*sink };
    let containing_block = callbacks.containing_block(box_);
    assert!(!containing_block.is_invalid());
    let entry_fragments = std::rc::Rc::new(fragment_tree::RunFragmentBuilder::new_entry_accumulator(
        containing_block,
    ));
    let entry_records = std::rc::Rc::new(RunRecords::new_unrooted(callbacks.arena, containing_block));
    let run = FormattingContextRun {
        purpose: LayoutPurpose::Commit,
        records: entry_records.clone(),
        box_: containing_block,
        layout_mode: LayoutMode::Normal,
        callbacks,
        should_collect_devtools_layout_data: false,
        treat_block_axis_percentage_insets_as_auto_beyond_root: false,
        fragments: Some(entry_fragments.clone()),
        previous_line_data: None,
    };
    abspos_engine::AbsposEngine::for_run(&run).replay(&run, box_);
    drain_and_commit_entry_pass(&entry_records, &entry_fragments, &callbacks, false, box_, sink);
    callbacks.arena().sweep_stale_fc_run_cache_entries();
}
