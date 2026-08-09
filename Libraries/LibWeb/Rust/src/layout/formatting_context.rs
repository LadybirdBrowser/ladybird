/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

const CALC_NUMERIC_KIND_LENGTH: u8 = 4;

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

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiResolvedAnchorInsets {
    pub resolves_top: bool,
    pub top_is_auto: bool,
    pub top: CssPixels,
    pub resolves_right: bool,
    pub right_is_auto: bool,
    pub right: CssPixels,
    pub resolves_bottom: bool,
    pub bottom_is_auto: bool,
    pub bottom: CssPixels,
    pub resolves_left: bool,
    pub left_is_auto: bool,
    pub left: CssPixels,
}


#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct PhysicalRect {
    pub(crate) x: CssPixels,
    pub(crate) y: CssPixels,
    pub(crate) width: CssPixels,
    pub(crate) height: CssPixels,
}

impl PhysicalRect {
    fn left(self) -> CssPixels {
        self.x
    }

    fn top(self) -> CssPixels {
        self.y
    }

    fn right(self) -> CssPixels {
        self.x + self.width
    }

    fn bottom(self) -> CssPixels {
        self.y + self.height
    }

}

fn point_add(left: FfiCssPixelPoint, right: FfiCssPixelPoint) -> FfiCssPixelPoint {
    FfiCssPixelPoint {
        x: left.x + right.x,
        y: left.y + right.y,
    }
}

fn point_sub(left: FfiCssPixelPoint, right: FfiCssPixelPoint) -> FfiCssPixelPoint {
    FfiCssPixelPoint {
        x: left.x - right.x,
        y: left.y - right.y,
    }
}

pub(crate) fn translate_static_position_between_chains(
    mut rect: StaticPositionRect,
    static_chain_offset: FfiCssPixelPoint,
    containing_chain_offset: FfiCssPixelPoint,
) -> StaticPositionRect {
    let physical_offset = point_sub(static_chain_offset, containing_chain_offset);
    rect.rect.offset.inline_offset += physical_offset.x;
    rect.rect.offset.block_offset += physical_offset.y;
    rect
}

pub(crate) fn anchor_rect_from_geometry(
    anchor_state: &UsedValues,
    containing_block_state: &UsedValues,
    anchor_offset: FfiCssPixelPoint,
) -> PhysicalRect {
    let collapsed = anchor_state.uses_collapsing_borders_model.get();
    PhysicalRect {
        x: anchor_offset.x - anchor_state.border_box_left(collapsed) + containing_block_state.padding_left.get(),
        y: anchor_offset.y - anchor_state.border_box_top(collapsed) + containing_block_state.padding_top.get(),
        width: anchor_state.border_box_inline_size(collapsed),
        height: anchor_state.border_box_block_size(collapsed),
    }
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
struct ReplacedIntrinsicSize {
    width: Option<CssPixels>,
    height: Option<CssPixels>,
    aspect_ratio: Option<PixelFraction>,
}

#[derive(Clone, Copy, Debug, Default)]
struct ReplacedMaxContentSizeConstraints {
    definite_size_in_ratio_determining_axis: Option<CssPixels>,
    minimum_inline_size: Option<CssPixels>,
    minimum_block_size: Option<CssPixels>,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum SizeDimension {
    Inline,
    Block,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub(crate) enum TableWrapperInlineSizeMode {
    ClampToAvailableInlineSize,
    UseTableUsedInlineSizeIfNotAuto,
}

pub(crate) struct MeasurementState {
    state: LayoutState,
    callbacks: FfiLayoutFcCallbacks,
    root: Node,
}

impl MeasurementState {
    pub(crate) fn create(callbacks: FfiLayoutFcCallbacks, node: Node, constraints: ContainingBlockConstraints) -> Self {
        let state = LayoutState::new(LayoutStatePurpose::Measurement);
        state.create_used_values(&callbacks, node, constraints);
        Self {
            state,
            callbacks,
            root: node,
        }
    }

    pub(crate) fn root_used(&self) -> &UsedValues {
        self.state.used_values(&self.callbacks, self.root)
    }

    fn run(&self, node: Node, input: LayoutInput) -> crate::layout::ChildLayoutResult {
        self.run_with_layout_mode(node, LayoutMode::IntrinsicSizing, input)
    }

    pub(crate) fn run_with_layout_mode(
        &self,
        node: Node,
        layout_mode: LayoutMode,
        input: LayoutInput,
    ) -> crate::layout::ChildLayoutResult {
        let layout_state = self.layout_state();
        let fc_type = crate::layout::independent_formatting_context_type(layout_state, node, &self.callbacks);
        crate::layout::run_formatting_context(
            layout_state,
            node,
            None,
            fc_type,
            layout_mode,
            false,
            self.callbacks,
            input,
            None,
        )
    }

    pub(crate) fn layout_state(&self) -> &LayoutState {
        &self.state
    }

    pub(crate) fn callbacks(&self) -> &FfiLayoutFcCallbacks {
        &self.callbacks
    }
}

fn cache_key(
    measured_at_inline_size: Option<CssPixels>,
    constraints: ContainingBlockConstraints,
) -> IntrinsicSizeCacheKey {
    IntrinsicSizeCacheKey {
        measured_at_inline_size,
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
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    node: Node,
    offset: FfiCssPixelPoint,
) {
    let used = state.used_values(callbacks, node);
    assert!(!used.has_content_offset.get());
    used.has_content_offset.set(true);
    used.content_offset.set(offset);
    used.committed_offset_delta
        .set(committed_offset_delta_at_placement(state, callbacks, node, used));
    used.seal_committed_box_metrics();
}

fn committed_offset_delta_at_placement(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    node: Node,
    used: &UsedValues,
) -> FfiCssPixelPoint {
    let mut delta = FfiCssPixelPoint::default();
    if state.is_measurement() {
        return delta;
    }
    let facts = state.node_facts(callbacks, node);
    if !facts.is_non_fragmented_box() {
        return delta;
    }
    if facts.is_relatively_positioned() {
        delta.x += used.inset_left.get();
        delta.y += used.inset_top.get();
    }
    if facts.is_in_flow() && facts.display().is_block_outside() {
        let chain = state.accumulated_relative_insets_from_inline_ancestor_chain(
            callbacks,
            callbacks.parent(node),
            callbacks.containing_block(node),
        );
        if chain.found_fragmented_inline_node {
            delta.x += chain.offset_x;
            delta.y += chain.offset_y;
        }
    }
    delta
}

pub(crate) fn register_contained_abspos_child(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    child: Node,
    static_position_rect: StaticPositionRect,
) {
    let mut target = callbacks.containing_block(child);
    if target.is_invalid() {
        return;
    }
    let inline_containing_block = callbacks.inline_containing_block(child);
    if !inline_containing_block.is_invalid() {
        state.note_inline_containing_block(inline_containing_block);
    }
    loop {
        let containing_block = callbacks.containing_block(target);
        let facts = state.node_facts(callbacks, target);
        if containing_block.is_invalid()
            || (formatting_context_type_created_by_box(facts).is_some()
                && !containing_block_geometry_is_finalized_by_the_table_run(state, callbacks, target))
        {
            break;
        }
        target = containing_block;
    }
    state.register_contained_abspos_child(callbacks, target, child, static_position_rect);
}

fn containing_block_geometry_is_finalized_by_the_table_run(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    containing_block: Node,
) -> bool {
    let facts = state.node_facts(callbacks, containing_block);
    facts.is_table_cell()
        || facts.is_table_row()
        || facts.is_table_row_group()
        || facts.is_table_header_group()
        || facts.is_table_footer_group()
}

pub(crate) fn box_baseline(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    used: &UsedValues,
    baseline_set: BaselineSet,
) -> CssPixels {
    box_baseline_with_content_baselines(state, callbacks, box_, used, baseline_set, used.content_baselines_from_cells())
}

pub(crate) fn box_baseline_with_content_baselines(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    used: &UsedValues,
    mut baseline_set: BaselineSet,
    content_baselines: DerivedBaselines,
) -> CssPixels {
    let facts = state.node_facts(callbacks, box_);
    let style = state.style_facts(callbacks, box_);
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
                let containing_style = state.style_facts(callbacks, containing_block);
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
                let containing_style = state.style_facts(callbacks, containing_block);
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
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    inhibits_floating: bool,
) -> DerivedBaselines {
    let facts = state.node_facts(callbacks, box_);
    let slot_index = callbacks.slot_index(box_);
    let line_count = state.line_data(slot_index).map_or(0, |data| data.line_boxes.len());
    if line_count > 0 {
        let baseline_for_line_box = |line_index: usize, baseline_set: BaselineSet| -> CssPixels {
            let (has_block_level_box, block_start, baseline, fragment_count, fragment_node) = {
                let line = &state.line_data(slot_index).unwrap().line_boxes[line_index];
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
            let block_child_state = state.used_values(callbacks, fragment_node);
            let child_offset_from_margin_edge = block_child_state.content_offset.get().y
                - block_child_state.margin_box_top(block_child_state.uses_collapsing_borders_model.get());
            child_offset_from_margin_edge + box_baseline(state, callbacks, fragment_node, block_child_state, baseline_set)
        };

        let mut first_line_index = 0;
        while first_line_index < line_count {
            let is_empty = state.line_data(slot_index).unwrap().line_boxes[first_line_index].is_empty();
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
            let is_empty = state.line_data(slot_index).unwrap().line_boxes[last_line_index].is_empty();
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
            let child_facts = state.node_facts(callbacks, child);
            if !child_facts.is_flow_layout_participant() || (!inhibits_floating && child_facts.is_floating()) {
                continue;
            }
            if !child_participates_in_table_run(container_display, &child_facts) {
                continue;
            }
            if container_skips_anonymous_whitespace_runs && callbacks.can_skip_is_anonymous_text_run(child) {
                continue;
            }
            let child_state = state.used_values(callbacks, child);
            match baseline_set {
                BaselineSet::First if child_state.has_first_baseline.get() => {}
                BaselineSet::Last if child_state.has_last_baseline.get() => {}
                _ => continue,
            }
            let child_offset_from_margin_edge = child_state.content_offset.get().y
                - child_state.margin_box_top(child_state.uses_collapsing_borders_model.get());
            return Some(child_offset_from_margin_edge + box_baseline(state, callbacks, child, child_state, baseline_set));
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
#[repr(C)]
pub struct FfiBorderData {
    pub color: u32,
    pub line_style: u8,
    pub width: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiBorderDataWithElementKind {
    pub border_data: FfiBorderData,
    pub element_kind: u8,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
#[repr(C)]
pub struct FfiBordersData {
    pub top: FfiBorderDataWithElementKind,
    pub right: FfiBorderDataWithElementKind,
    pub bottom: FfiBorderDataWithElementKind,
    pub left: FfiBorderDataWithElementKind,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ChildLayoutResult {
    pub automatic_content_inline_size: CssPixels,
    pub automatic_content_block_size: CssPixels,
    pub baselines: DerivedBaselines,
    pub table_box_in_wrapper_border_box_block_size: Option<CssPixels>,
}

pub(crate) enum ChildLayoutOutcome {
    Skipped,
    Created(ChildLayoutResult),
    ReenterCurrent,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct MeasuredCellContent {
    pub content_block_size: CssPixels,
    pub first_baseline: CssPixels,
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
#[repr(C)]
pub struct FfiFlexLayoutItemRect {
    pub x: CssPixels,
    pub y: CssPixels,
    pub width: CssPixels,
    pub height: CssPixels,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiFlexLayoutClampState {
    Unclamped,
    ClampedToMin,
    ClampedToMax,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
pub enum FfiFlexLayoutGrowthState {
    Growing,
    Shrinking,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFlexLayoutItem {
    pub node: *mut c_void,
    pub rect: FfiFlexLayoutItemRect,
    pub main_base_size: CssPixels,
    pub main_delta_size: CssPixels,
    pub main_min_size: CssPixels,
    pub main_max_size: CssPixels,
    pub cross_min_size: CssPixels,
    pub cross_max_size: CssPixels,
    pub clamp_state: FfiFlexLayoutClampState,
    pub flex_grow: f64,
    pub flex_shrink: f64,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFlexLayoutLine {
    pub growth_state: FfiFlexLayoutGrowthState,
    pub cross_start: CssPixels,
    pub cross_size: CssPixels,
    pub items: *const FfiFlexLayoutItem,
    pub item_count: usize,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiFlexLayoutData {
    pub align_content: u8,
    pub align_items: u8,
    pub flex_direction: u8,
    pub flex_wrap: u8,
    pub justify_content: u8,
    pub main_axis_direction: u8,
    pub cross_axis_direction: u8,
    pub lines: *const FfiFlexLayoutLine,
    pub line_count: usize,
}

pub(crate) struct OwnedFlexLayoutLine {
    pub(crate) growth_state: FfiFlexLayoutGrowthState,
    pub(crate) cross_start: CssPixels,
    pub(crate) cross_size: CssPixels,
    pub(crate) items: Vec<FfiFlexLayoutItem>,
}

pub(crate) struct OwnedFlexLayoutData {
    pub(crate) align_content: u8,
    pub(crate) align_items: u8,
    pub(crate) flex_direction: u8,
    pub(crate) flex_wrap: u8,
    pub(crate) justify_content: u8,
    pub(crate) main_axis_direction: u8,
    pub(crate) cross_axis_direction: u8,
    pub(crate) lines: Vec<OwnedFlexLayoutLine>,
}

impl OwnedFlexLayoutData {
    pub(crate) fn with_ffi_view(&self, callback: impl FnOnce(&FfiFlexLayoutData)) {
        let lines = self
            .lines
            .iter()
            .map(|line| FfiFlexLayoutLine {
                growth_state: line.growth_state,
                cross_start: line.cross_start,
                cross_size: line.cross_size,
                items: line.items.as_ptr(),
                item_count: line.items.len(),
            })
            .collect::<Vec<_>>();
        let view = FfiFlexLayoutData {
            align_content: self.align_content,
            align_items: self.align_items,
            flex_direction: self.flex_direction,
            flex_wrap: self.flex_wrap,
            justify_content: self.justify_content,
            main_axis_direction: self.main_axis_direction,
            cross_axis_direction: self.cross_axis_direction,
            lines: lines.as_ptr(),
            line_count: lines.len(),
        };
        callback(&view);
    }
}

#[derive(Clone, Copy)]
#[repr(C)]
pub struct FfiLayoutFcCallbacks {
    pub context: *mut c_void,
    pub arena: *mut c_void,
    pub initial_containing_block_inline_size: CssPixels,
    pub document_in_quirks_mode: bool,
    pub static_position_containing_block: unsafe extern "C" fn(*mut c_void, *mut c_void) -> NodeSlotId,
    pub needs_inset_resolution: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub report_unexpected_fragmented_inline: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub build_svg_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiSvgElementFacts,
    pub read_paintable_geometry:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *mut crate::layout::FfiPaintableGeometry) -> bool,
    pub read_paintable_svg_transforms:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut FfiSvgComputedTransforms) -> bool,
    pub compute_svg_path: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiSvgPathRequest) -> FfiSvgPathResult,
    pub svg_image_bounding_box: unsafe extern "C" fn(*mut c_void, *mut c_void, CssPixels, CssPixels) -> FfiFloatRect,
    pub anchor_lookup: unsafe extern "C" fn(*mut c_void, *mut c_void, usize, *const *mut c_void, usize) -> NodeSlotId,
    pub set_default_scroll_shift: unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, bool, bool),
}

impl FfiLayoutFcCallbacks {
    fn arena(&self) -> &LayoutNodeArena {
        // SAFETY: C++ borrows the document arena for the synchronous layout
        // pass represented by this callback table.
        unsafe { LayoutNodeArena::from_handle(self.arena) }
    }

    pub(crate) fn node_data(&self, node: Node) -> &NodeData {
        // SAFETY: Entry points guarantee that the arena remains live, and
        // data() validates the slot generation.
        unsafe { &*self.arena().data(node) }
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
        if !crate::layout::has_flag(data, NodeFlag::Anonymous) || data.generated_for != 0 {
            return false;
        }

        let mut child = data.first_child;
        while !child.is_invalid() {
            let data = self.node_data(child);
            if !crate::layout::kind_is_text(data.kind)
                || !self.text_content(child).untransformed_text_is_ascii_whitespace
            {
                return false;
            }
            child = data.next_sibling;
        }
        true
    }

    pub(crate) fn shell(&self, node: Node) -> *mut c_void {
        let shell = self.node_data(node).shell;
        assert!(!shell.is_null());
        shell
    }

    pub(crate) fn slot_index(&self, node: Node) -> u32 {
        node.slot_index()
    }

    pub(crate) fn is_before(&self, node: Node, other: Node) -> bool {
        self.arena().is_before(self.node_data(node), self.node_data(other))
    }

    pub(crate) fn saved_abspos_layout_inputs(&self, node: Node) -> Option<crate::layout::AbsposLayoutInputs> {
        let data = self.arena().data(node);
        // SAFETY: node_data_pointer() returns a live arena slot.
        assert!(unsafe { crate::layout::kind_is_box((*data).kind) });
        self.arena().saved_abspos_layout_inputs(data)
    }

    pub(crate) fn set_saved_abspos_layout_inputs(&self, node: Node, inputs: Option<crate::layout::AbsposLayoutInputs>) {
        let data = self.arena().data(node);
        // Match prepare_node's former as_if<Box>() guard.
        // SAFETY: node_data_pointer() returns a live arena slot.
        if !unsafe { crate::layout::kind_is_box((*data).kind) } {
            return;
        }
        self.arena().set_saved_abspos_layout_inputs(data, inputs);
    }

    pub(crate) fn parent(&self, node: Node) -> Node {
        self.node_data(node).parent
    }

    pub(crate) fn first_child(&self, node: Node) -> Node {
        self.node_data(node).first_child
    }

    pub(crate) fn next_sibling(&self, node: Node) -> Node {
        self.node_data(node).next_sibling
    }

    pub(crate) fn containing_block(&self, node: Node) -> Node {
        self.node_data(node).containing_block
    }

    pub(crate) fn inline_containing_block(&self, node: Node) -> Node {
        self.node_data(node).inline_containing_block
    }

    pub(crate) fn static_position_containing_block(&self, node: Node) -> Node {
        unsafe { (self.static_position_containing_block)(self.context, self.shell(node)) }
    }

    pub(crate) fn is_ancestor(&self, ancestor: Node, mut node: Node) -> bool {
        while !node.is_invalid() {
            if node == ancestor {
                return true;
            }
            node = self.node_data(node).parent;
        }
        false
    }

    pub(crate) fn non_anonymous_containing_block(&self, node: Node) -> Node {
        let mut containing_block = self.node_data(node).containing_block;
        assert!(!containing_block.is_invalid());
        while self.node_data(containing_block).flags & NodeFlag::Anonymous as u32 != 0 {
            containing_block = self.node_data(containing_block).containing_block;
            assert!(!containing_block.is_invalid());
        }
        containing_block
    }
}

pub(crate) struct FormattingContextRun<'pass> {
    pub(crate) state: &'pass LayoutState,
    pub(crate) box_: Node,
    pub(crate) layout_mode: LayoutMode,
    pub(crate) callbacks: FfiLayoutFcCallbacks,
    pub(crate) should_collect_devtools_layout_data: bool,
    pub(crate) treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
}

impl<'pass> FormattingContextRun<'pass> {
    pub(crate) fn new(
        state: &'pass LayoutState,
        box_: Node,
        layout_mode: LayoutMode,
        callbacks: FfiLayoutFcCallbacks,
        should_collect_devtools_layout_data: bool,
        treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    ) -> Self {
        Self {
            state,
            box_,
            layout_mode,
            callbacks,
            should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root,
        }
    }
}

enum FormattingContextImplementation<'pass> {
    Block(Box<BlockFormattingContext<'pass>>),
    Flex(Box<FlexFormattingContext<'pass>>),
    Grid(Box<GridFormattingContext<'pass>>),
    Table(Box<TableFormattingContext<'pass>>),
    Svg(Box<SvgFormattingContext<'pass>>),
    ReplacedWithChildren,
    InternalReplaced,
    InternalDummy,
}

pub(crate) fn formatting_context_type_created_by_node_data(
    data: &NodeData,
    style: Option<ComputedValuesView<'_>>,
    parent_style: Option<ComputedValuesView<'_>>,
) -> Option<FfiFormattingContextType> {
    if data.kind == crate::layout::node_data::NodeKind::SVGSVGBox {
        return Some(FfiFormattingContextType::Svg);
    }
    let is_replaced_box = crate::layout::kind_is_replaced_box(data.kind);
    let can_have_children = crate::layout::node_can_have_children(data);
    if is_replaced_box && can_have_children {
        return Some(FfiFormattingContextType::ReplacedWithChildren);
    }
    if is_replaced_box {
        return Some(FfiFormattingContextType::InternalReplaced);
    }
    if !can_have_children {
        return None;
    }
    if crate::layout::has_flag(data, NodeFlag::IsReplacedElement)
        && style.is_some_and(|style| {
            let display = style.display_before_box_type_transformation();
            display.is_table_inside() || display.is_internal_table() || display.is_table_caption()
        })
    {
        return Some(if crate::layout::kind_is_block_container(data.kind) {
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
        || crate::layout::node_creates_block_formatting_context(data, style, parent_style)
    {
        return Some(FfiFormattingContextType::Block);
    }
    if crate::layout::has_flag(data, NodeFlag::ChildrenAreInline)
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
    abort_on_panic(|| {
        // SAFETY: The C++ caller borrows the box's arena for this synchronous
        // classification.
        let arena = unsafe { LayoutNodeArena::from_handle(facts.arena) };
        // SAFETY: The caller supplies the live box's arena slot.
        let data = unsafe { &*arena.data(facts.node) };
        let style = arena.style_payloads(facts.node).map(|payloads| ComputedValuesView::new(&payloads.groups));
        let parent_style = (!data.parent.is_invalid())
            .then(|| arena.style_payloads(data.parent))
            .flatten()
            .map(|payloads| ComputedValuesView::new(&payloads.groups));
        formatting_context_type_created_by_node_data(data, style, parent_style)
            .map(|type_| type_ as u8)
            .unwrap_or(NO_FORMATTING_CONTEXT)
    })
}

fn create_formatting_context_implementation<'pass>(
    run: &FormattingContextRun<'pass>,
    parent_grid: Option<&GridFormattingContext<'pass>>,
    fc_type: FfiFormattingContextType,
) -> FormattingContextImplementation<'pass> {
    match fc_type {
        FfiFormattingContextType::Block => FormattingContextImplementation::Block(Box::new(BlockFormattingContext::new(
            run.state,
            run.box_,
            run.layout_mode,
            run.callbacks,
        ))),
        FfiFormattingContextType::Flex => FormattingContextImplementation::Flex(Box::new(FlexFormattingContext::new(run))),
        FfiFormattingContextType::Grid => FormattingContextImplementation::Grid(Box::new(GridFormattingContext::new(
            run.state,
            run.box_,
            parent_grid,
            run.layout_mode,
            run.callbacks,
            run.should_collect_devtools_layout_data,
        ))),
        FfiFormattingContextType::Table => FormattingContextImplementation::Table(Box::new(TableFormattingContext::new(run))),
        FfiFormattingContextType::Svg => FormattingContextImplementation::Svg(Box::new(SvgFormattingContext::new(
            run.state,
            run.box_,
            run.layout_mode,
            run.callbacks,
        ))),
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
        let facts = run.state.node_facts(&run.callbacks, child);
        if facts.is_box() {
            if facts.is_absolutely_positioned() {
                register_contained_abspos_child(
                    run.state,
                    &run.callbacks,
                    child,
                    StaticPositionRect {
                        rect: Default::default(),
                        inline_alignment: StaticPositionAlignment::Start,
                        block_alignment: StaticPositionAlignment::Start,
                        alignment_derives_from_own_computed_values: false,
                    },
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
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    node: Node,
    available_inner_space: AvailableSpace,
    constraints: ContainingBlockConstraints,
    automatic_content_block_size_of_completed_run: Option<CssPixels>,
) -> CssPixels {
    let facts = state.node_facts(callbacks, node);
    if facts.creates_block_formatting_context() {
        return automatic_content_block_size_of_completed_run.unwrap_or_default();
    }
    let style = state.style_facts(callbacks, node);
    if style.display().is_flex_inside() || style.display().is_grid_inside() || style.display().is_table_inside() {
        // The automatic block size of a flex, grid, or table container is its
        // max-content size.
        // https://drafts.csswg.org/css-flexbox-1/#algo-main-container
        // https://www.w3.org/TR/css-grid-2/#intrinsic-sizes
        return SizingContext::new(state, *callbacks).calculate_max_content_block_size(
            node,
            available_inner_space.inline_size.to_px_or_zero(),
            constraints,
        );
    }
    debug_assert!(false, "independent formatting context root of unexpected kind");
    CssPixels::default()
}

fn apply_root_sizing_directives(
    run: &FormattingContextRun,
    input: &LayoutInput,
    parent_block: Option<&BlockFormattingContext>,
) -> LayoutInput {
    match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => dimension_block_level_root(run, input, parent_block),
        ParticipationInParentFormattingContext::Float => {
            let parent = parent_block.expect("a floating run requires an enclosing block formatting context");
            parent.dimension_float_root(run.box_, input);
            body_input_with_inner_available_space(run, input)
        }
        ParticipationInParentFormattingContext::AtomicInline => {
            SizingContext::new(run.state, run.callbacks).dimension_atomic_root(
                run.box_,
                input.available_space,
                input.containing_block_constraints,
                run.layout_mode,
            );
            body_input_with_inner_available_space(run, input)
        }
        ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
            AbsposEngine::new(run.state, run.callbacks).dimension_out_of_flow_root(run.box_, abspos_inputs);
            body_input_with_inner_available_space(run, input)
        }
        ParticipationInParentFormattingContext::Item => {
            if cfg!(debug_assertions) && run.layout_mode == LayoutMode::Normal && !run.state.is_measurement() {
                debug_assert!(
                    run.state.used_values(&run.callbacks, run.box_).has_definite_inline_size.get(),
                    "container-internal run root must arrive with a container-assigned inline size"
                );
            }
            *input
        }
        ParticipationInParentFormattingContext::Root => {
            let directives = input.sizing;
            if directives.forced_content_inline_size.is_some() || directives.forced_content_block_size.is_some() {
                let used = run.state.used_values(&run.callbacks, run.box_);
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
    }
}

fn dimension_block_level_root(
    run: &FormattingContextRun,
    input: &LayoutInput,
    parent_block: Option<&BlockFormattingContext>,
) -> LayoutInput {
    let node = run.box_;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let parent = parent_block.expect("a block-level run requires an enclosing block formatting context");
    parent.commit_block_level_root_inline_size(node, input);
    parent.resolve_block_level_root_block_size_before_body(node, input);
    let sizing = SizingContext::new(run.state, run.callbacks);
    let style = run.state.style_facts(&run.callbacks, node);
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

fn finalize_block_level_root(
    run: &FormattingContextRun,
    input: &LayoutInput,
    parent_block: Option<&BlockFormattingContext>,
    body_result: &ChildLayoutResult,
) {
    let node = run.box_;
    let facts = run.state.node_facts(&run.callbacks, node);
    if facts.is_table_wrapper() {
        run
            .state
            .used_values(&run.callbacks, node)
            .set_content_inline_size(body_result.automatic_content_inline_size);
    }
    let style = run.state.style_facts(&run.callbacks, node);
    if !style.display().is_table_inside() {
        let parent = parent_block.expect("a block-level run requires an enclosing block formatting context");
        let resolution_space = parent.sizing().available_space_for_block_size_resolution(
            node,
            input.available_space,
            input.containing_block_constraints,
        );
        parent.resolve_used_block_size_if_treated_as_auto(
            node,
            resolution_space,
            input.containing_block_constraints,
            Some(body_result.automatic_content_block_size),
        );
    }
}

fn size_skipped_independent_root(
    run: &FormattingContextRun,
    parent_block: Option<&BlockFormattingContext>,
    child: Node,
    input: &LayoutInput,
) {
    match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => {
            let parent = parent_block.expect("a block-level run requires an enclosing block formatting context");
            parent.commit_block_level_root_inline_size(child, input);
            parent.resolve_block_level_root_block_size_before_body(child, input);
        }
        ParticipationInParentFormattingContext::Float => {
            let parent = parent_block.expect("a floating run requires an enclosing block formatting context");
            parent.dimension_float_root(child, input);
            parent.finalize_float_root(child, input, None);
        }
        ParticipationInParentFormattingContext::AbsolutelyPositioned(abspos_inputs) => {
            let engine = AbsposEngine::new(run.state, run.callbacks);
            engine.dimension_out_of_flow_root(child, abspos_inputs);
            engine.finalize_out_of_flow_root_after_inside_layout(child, abspos_inputs, None);
        }
        ParticipationInParentFormattingContext::AtomicInline | ParticipationInParentFormattingContext::Item | ParticipationInParentFormattingContext::Root => {}
    }
}

fn body_input_with_inner_available_space(run: &FormattingContextRun, input: &LayoutInput) -> LayoutInput {
    let inner_available_space = run
        .state
        .used_values(&run.callbacks, run.box_)
        .available_inner_space_or_constraints_from(input.available_space);
    let mut body_input = *input;
    body_input.available_space = inner_available_space;
    body_input
}

#[expect(clippy::too_many_arguments)]
fn run_formatting_context<'pass>(
    state: &'pass LayoutState,
    box_: Node,
    parent_grid: Option<&GridFormattingContext<'pass>>,
    fc_type: FfiFormattingContextType,
    layout_mode: LayoutMode,
    should_collect_devtools_layout_data: bool,
    callbacks: FfiLayoutFcCallbacks,
    input: LayoutInput,
    parent_block: Option<&BlockFormattingContext<'pass>>,
) -> ChildLayoutResult {
    assert!(!box_.is_invalid());
    let run = FormattingContextRun::new(
        state,
        box_,
        layout_mode,
        callbacks,
        should_collect_devtools_layout_data,
        input.sizing.treat_block_axis_percentage_insets_as_auto_beyond_root,
    );
    let run = &run;
    let body_input = apply_root_sizing_directives(run, &input, parent_block);

    let cached_atomic_block_size = if matches!(input.participation, ParticipationInParentFormattingContext::AtomicInline) {
        SizingContext::new(run.state, run.callbacks).apply_cached_intrinsic_inline_measurement(
            run.box_,
            input.available_space.inline_size,
            body_input.available_space.block_size,
            input.containing_block_constraints,
        )
    } else {
        None
    };
    let mut implementation = None;
    let result = if let Some((cached_block_size, cached_baselines)) = cached_atomic_block_size {
        ChildLayoutResult {
            automatic_content_block_size: cached_block_size,
            baselines: cached_baselines,
            ..ChildLayoutResult::default()
        }
    } else {
        let mut context_implementation = create_formatting_context_implementation(run, parent_grid, fc_type);
        let result = match &mut context_implementation {
            FormattingContextImplementation::Block(context) => {
                context.run(run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                store_derived_baselines(run.state.used_values(&run.callbacks, run.box_), baselines);
                ChildLayoutResult {
                    automatic_content_inline_size: context.automatic_content_inline_size(),
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    table_box_in_wrapper_border_box_block_size: context.table_box_in_wrapper_border_box_block_size(),
                }
            }
            FormattingContextImplementation::Flex(context) => {
                context.run(run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                store_derived_baselines(run.state.used_values(&run.callbacks, run.box_), baselines);
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
                store_derived_baselines(run.state.used_values(&run.callbacks, run.box_), baselines);
                ChildLayoutResult {
                    automatic_content_inline_size: context.automatic_content_inline_size(),
                    automatic_content_block_size: context.automatic_content_block_size(),
                    baselines,
                    ..ChildLayoutResult::default()
                }
            }
            FormattingContextImplementation::Table(context) => {
                context.run(run, body_input);
                let baselines = context.derived_baselines_of_root_box();
                store_derived_baselines(run.state.used_values(&run.callbacks, run.box_), baselines);
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
            FormattingContextImplementation::ReplacedWithChildren => layout_replaced_with_children(run, body_input),
            FormattingContextImplementation::InternalReplaced | FormattingContextImplementation::InternalDummy => ChildLayoutResult::default(),
        };
        implementation = Some(context_implementation);
        result
    };

    match input.participation {
        ParticipationInParentFormattingContext::BlockLevel => {
            finalize_block_level_root(run, &input, parent_block, &result);
        }
        ParticipationInParentFormattingContext::Float => {
            let parent = parent_block.expect("a floating run requires an enclosing block formatting context");
            parent.finalize_float_root(
                run.box_,
                &input,
                Some((result.automatic_content_inline_size, result.automatic_content_block_size)),
            );
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
            AbsposEngine::new(run.state, run.callbacks).finalize_out_of_flow_root_after_inside_layout(
                run.box_,
                abspos_inputs,
                Some(result.automatic_content_block_size),
            );
        }
        ParticipationInParentFormattingContext::Item => {
            if input.sizing.adopt_automatic_content_block_size {
                let used = run.state.used_values(&run.callbacks, run.box_);
                used.set_content_block_size(result.automatic_content_block_size);
            }
        }
        ParticipationInParentFormattingContext::Root => {}
    }

    let registered_abspos_children_could_never_be_laid_out =
        run.layout_mode != LayoutMode::Normal || run.state.is_measurement();
    if registered_abspos_children_could_never_be_laid_out {
        return result;
    }
    let implementation = implementation.expect("cached measurement replay only occurs on measurement states");
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
        FormattingContextImplementation::InternalReplaced | FormattingContextImplementation::InternalDummy => return result,
    }
    layout_contained_abspos_children(run);
    result
}

fn finalize_atomic_root_block_size(
    run: &FormattingContextRun,
    input: &LayoutInput,
    cached_intrinsic_measurement_block_size: Option<CssPixels>,
    automatic_content_block_size_of_completed_body_run: Option<CssPixels>,
    parent_block: Option<&BlockFormattingContext>,
) {
    let node = run.box_;
    let available_space = input.available_space;
    let constraints = input.containing_block_constraints;
    let sizing = SizingContext::new(run.state, run.callbacks);
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
                    .state
                    .used_values(&run.callbacks, node)
                    .available_inner_space_or_constraints_from(available_space);
                match parent_block {
                    Some(parent) => parent.compute_automatic_block_size_for_block_level_element(
                        node,
                        available_inner_space,
                        constraints,
                        automatic_content_block_size_of_completed_body_run,
                    ),
                    None => independent_root_automatic_block_size(
                        run.state,
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

pub(crate) fn layout_inside_child<'pass>(
    run: &FormattingContextRun<'pass>,
    parent_block: Option<&BlockFormattingContext<'pass>>,
    parent_grid: Option<&GridFormattingContext<'pass>>,
    child: Node,
    layout_mode: LayoutMode,
    mut input: LayoutInput,
    force_independent_context_run: bool,
) -> ChildLayoutOutcome {
    let facts = run.state.node_facts(&run.callbacks, child);
    let used = run.state.used_values(&run.callbacks, child);
    if let Some((padding_top, padding_bottom)) = input.sizing.table_cell_intrinsic_block_padding {
        debug_assert!(facts.is_table_cell());
        debug_assert!(matches!(input.participation, ParticipationInParentFormattingContext::Item));
        used.padding_top.set(used.padding_top.get() + padding_top);
        used.padding_bottom.set(used.padding_bottom.get() + padding_bottom);
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
        return ChildLayoutOutcome::Skipped;
    }
    let creates_replaced_context = matches!(
        formatting_context_type_created_by_box(facts),
        Some(FfiFormattingContextType::InternalReplaced | FfiFormattingContextType::ReplacedWithChildren)
    );
    if !facts.can_have_children() && !creates_replaced_context {
        size_skipped_independent_root(run, parent_block, child, &input);
        return ChildLayoutOutcome::Skipped;
    }

    let fc_type = formatting_context_type_created_by_box(facts).or_else(|| {
        force_independent_context_run.then(|| independent_formatting_context_type(run.state, child, &run.callbacks))
    });
    let Some(fc_type) = fc_type else {
        if force_independent_context_run {
            return ChildLayoutOutcome::Skipped;
        }
        return ChildLayoutOutcome::ReenterCurrent;
    };
    input.sizing.treat_block_axis_percentage_insets_as_auto_beyond_root =
        treat_block_axis_percentage_insets_as_auto_beyond_anonymous_child_root(
            run.state,
            &run.callbacks,
            child,
            run.box_,
            run.treat_block_axis_percentage_insets_as_auto_beyond_root,
        );
    ChildLayoutOutcome::Created(run_formatting_context(
        run.state,
        child,
        parent_grid,
        fc_type,
        layout_mode,
        run.should_collect_devtools_layout_data,
        run.callbacks,
        input,
        parent_block,
    ))
}

fn independent_formatting_context_type(
    state: &LayoutState,
    box_: Node,
    callbacks: &FfiLayoutFcCallbacks,
) -> FfiFormattingContextType {
    let facts = state.node_facts(callbacks, box_);
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
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    containing_block: Node,
    formatting_context_root: Node,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
) -> bool {
    let mut candidate = containing_block;
    while !candidate.is_invalid() {
        let facts = state.node_facts(callbacks, candidate);
        if !facts.is_anonymous() || facts.is_table_cell() {
            return state.used_values(callbacks, candidate).has_definite_block_size();
        }
        if candidate == formatting_context_root {
            return !treat_block_axis_percentage_insets_as_auto_beyond_root;
        }
        candidate = callbacks.containing_block(candidate);
    }
    true
}

pub(crate) fn treat_block_axis_percentage_insets_as_auto_beyond_anonymous_child_root(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    child_root: Node,
    formatting_context_root: Node,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
) -> bool {
    let child_root_facts = state.node_facts(callbacks, child_root);
    if !child_root_facts.is_anonymous() || child_root_facts.is_table_cell() {
        return false;
    }
    !resolve_block_axis_percentage_inset_basis_is_definite(
        state,
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
    sink: *const FfiCommitSink,
) {
    abort_on_panic(|| {
        assert!(!root.is_invalid());
        assert!(!callbacks.is_null());
        assert!(!sink.is_null());
        // SAFETY: The C++ pass host keeps both callback tables live for this
        // synchronous entry.
        let callbacks = unsafe { *callbacks };
        let sink = unsafe { &*sink };
        let viewport_inline_size = CssPixels::from_raw(viewport_inline_size_raw);
        let viewport_block_size = CssPixels::from_raw(viewport_block_size_raw);

        let state = LayoutState::new(LayoutStatePurpose::Commit);
        let root_constraints = crate::layout::ContainingBlockConstraints {
            percentage_basis_inline_size: Some(viewport_inline_size),
            percentage_basis_block_size: Some(viewport_block_size),
            ..crate::layout::ContainingBlockConstraints::default()
        };
        let viewport_used = state.create_used_values(&callbacks, root, root_constraints);

        let mut root_for_layout = root;
        let first_child = callbacks.first_child(root);
        if !first_child.is_invalid() && state.node_facts(&callbacks, first_child).is_svg_svg_box() {
            viewport_used.set_content_inline_size(viewport_inline_size);
            viewport_used.set_content_block_size(viewport_block_size);
            place_child(&state, &callbacks, root, FfiCssPixelPoint::default());
            state.create_used_values(&callbacks, first_child, root_constraints);
            root_for_layout = first_child;
        }
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::definite(viewport_inline_size),
                block_size: AvailableSize::definite(viewport_block_size),
            },
            crate::layout::ContainingBlockConstraints::default(),
            ParticipationInParentFormattingContext::Root,
        )
        .with_forced_sizes(viewport_inline_size, viewport_block_size);
        let state_ref = &state;
        let fc_type = independent_formatting_context_type(state_ref, root_for_layout, &callbacks);
        run_formatting_context(
            state_ref,
            root_for_layout,
            None,
            fc_type,
            LayoutMode::Normal,
            should_collect_devtools_layout_data,
            callbacks,
            input,
            None,
        );
        place_child(&state, &callbacks, root_for_layout, FfiCssPixelPoint::default());
        drain_remaining_abspos_targets(
            state_ref,
            callbacks,
            should_collect_devtools_layout_data,
            &[root, root_for_layout],
        );
        state.commit_replacing(root, std::ptr::null_mut(), &callbacks, sink);
    });
}

/// # Safety
///
/// The callback table and commit sink must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_compute_subtree_layout(
    root: NodeSlotId,
    viewport: NodeSlotId,
    paintable_to_replace: *mut c_void,
    viewport_inline_size_raw: i32,
    viewport_block_size_raw: i32,
    callbacks: *const FfiLayoutFcCallbacks,
    sink: *const FfiCommitSink,
) {
    abort_on_panic(|| {
        assert!(!root.is_invalid());
        assert!(!paintable_to_replace.is_null());
        assert!(!callbacks.is_null());
        assert!(!sink.is_null());
        // SAFETY: The C++ pass host keeps both callback tables live for this
        // synchronous entry.
        let callbacks = unsafe { *callbacks };
        let sink = unsafe { &*sink };

        let state = LayoutState::new(LayoutStatePurpose::Commit);
        let root_used = state
            .populate_from_paintable(&callbacks, root, paintable_to_replace)
            .expect("partial relayout root must have committed geometry");
        if !viewport.is_invalid() && viewport != root {
            let viewport_inline_size = CssPixels::from_raw(viewport_inline_size_raw);
            let viewport_block_size = CssPixels::from_raw(viewport_block_size_raw);
            let viewport_constraints = crate::layout::ContainingBlockConstraints {
                percentage_basis_inline_size: Some(viewport_inline_size),
                percentage_basis_block_size: Some(viewport_block_size),
                ..crate::layout::ContainingBlockConstraints::default()
            };
            let viewport_used = state.create_used_values(&callbacks, viewport, viewport_constraints);
            viewport_used.set_content_inline_size(viewport_inline_size);
            viewport_used.set_content_block_size(viewport_block_size);
            place_child(&state, &callbacks, viewport, FfiCssPixelPoint::default());
        }
        let input = LayoutInput::new(
            AvailableSpace {
                inline_size: AvailableSize::definite(root_used.content_inline_size.get()),
                block_size: AvailableSize::definite(root_used.content_block_size.get()),
            },
            // The subtree root has definite sizes in both axes, so boxes
            // below it do not need inherited percentage constraints.
            crate::layout::ContainingBlockConstraints::default(),
            ParticipationInParentFormattingContext::Root,
        );

        let state_ref = &state;
        let facts = state.node_facts(&callbacks, root);
        let fc_type = formatting_context_type_created_by_box(facts)
            .expect("partial relayout root must establish an independent formatting context");
        run_formatting_context(state_ref, root, None, fc_type, LayoutMode::Normal, false, callbacks, input, None);
        drain_remaining_abspos_targets(state_ref, callbacks, false, &[viewport, root]);
        state.commit_replacing(root, paintable_to_replace, &callbacks, sink);
    });
}

/// # Safety
///
/// The callback table and commit sink must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_replay_saved_abspos_layout(
    box_: NodeSlotId,
    paintable_to_replace: *mut c_void,
    callbacks: *const FfiLayoutFcCallbacks,
    sink: *const FfiCommitSink,
) {
    abort_on_panic(|| {
        assert!(!box_.is_invalid());
        assert!(!paintable_to_replace.is_null());
        assert!(!callbacks.is_null());
        assert!(!sink.is_null());
        // SAFETY: The C++ pass host keeps both callback tables live for this
        // synchronous entry.
        let callbacks = unsafe { *callbacks };
        let sink = unsafe { &*sink };
        let state = LayoutState::new(LayoutStatePurpose::Commit);
        let state_ref = &state;
        let containing_block = callbacks.containing_block(box_);
        assert!(!containing_block.is_invalid());
        let run = crate::layout::FormattingContextRun::new(state_ref, containing_block, LayoutMode::Normal, callbacks, false, false);
        AbsposEngine::new(state_ref, callbacks).replay(&run, box_);
        drain_remaining_abspos_targets(state_ref, callbacks, false, &[containing_block]);
        state.commit_replacing(box_, paintable_to_replace, &callbacks, sink);
    });
}
