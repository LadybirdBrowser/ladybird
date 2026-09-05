/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

use super::*;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FlexFactor {
    Grow,
    Shrink,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
enum AxisDirection {
    HorizontalLr,
    HorizontalRl,
    VerticalTb,
    VerticalBt,
}

#[derive(Clone, Copy, Debug, Default)]
struct DirectionAgnosticMargins {
    main_before: CssPixels,
    main_after: CssPixels,
    cross_before: CssPixels,
    cross_after: CssPixels,
    main_before_is_auto: bool,
    main_after_is_auto: bool,
    cross_before_is_auto: bool,
    cross_after_is_auto: bool,
}

#[derive(Clone, Copy)]
enum UsedFlexBasis<'pass> {
    Content,
    Size {
        value: &'pass ComputedSize,
        property: SizingProperty,
    },
}

struct FlexItem<'pass> {
    box_: Node,
    used_flex_basis: UsedFlexBasis<'pass>,
    used_flex_basis_is_definite: bool,
    main_size_was_resolved_from_aspect_ratio: bool,
    cross_size_was_resolved_from_aspect_ratio: bool,
    flex_base_size: CssPixels,
    hypothetical_main_size: CssPixels,
    hypothetical_cross_size: CssPixels,
    target_main_size: CssPixels,
    frozen: bool,
    flex_factor: f64,
    scaled_flex_shrink_factor: f64,
    desired_flex_fraction: f64,
    // The used main size of this flex item. Empty until determined.
    main_size: Option<CssPixels>,
    // The used cross size of this flex item. Empty until determined.
    cross_size: Option<CssPixels>,
    main_offset: CssPixels,
    cross_offset: CssPixels,
    margins: DirectionAgnosticMargins,
    borders: DirectionAgnosticMargins,
    padding: DirectionAgnosticMargins,
    is_min_violation: bool,
    is_max_violation: bool,
    content_baselines: DerivedBaselines,
}

impl FlexItem<'_> {
    fn new(box_: Node) -> Self {
        Self {
            box_,
            used_flex_basis: UsedFlexBasis::Content,
            used_flex_basis_is_definite: false,
            main_size_was_resolved_from_aspect_ratio: false,
            cross_size_was_resolved_from_aspect_ratio: false,
            flex_base_size: CssPixels::default(),
            hypothetical_main_size: CssPixels::default(),
            hypothetical_cross_size: CssPixels::default(),
            target_main_size: CssPixels::default(),
            frozen: false,
            flex_factor: 0.0,
            scaled_flex_shrink_factor: 0.0,
            desired_flex_fraction: 0.0,
            main_size: None,
            cross_size: None,
            main_offset: CssPixels::default(),
            cross_offset: CssPixels::default(),
            margins: DirectionAgnosticMargins::default(),
            borders: DirectionAgnosticMargins::default(),
            padding: DirectionAgnosticMargins::default(),
            is_min_violation: false,
            is_max_violation: false,
            content_baselines: DerivedBaselines::default(),
        }
    }

    fn hypothetical_cross_size_with_margins(&self) -> CssPixels {
        self.hypothetical_cross_size
            + self.margins.cross_before
            + self.margins.cross_after
            + self.borders.cross_after
            + self.borders.cross_before
            + self.padding.cross_after
            + self.padding.cross_before
    }

    fn outer_hypothetical_main_size(&self) -> CssPixels {
        self.add_main_margin_box_sizes(self.hypothetical_main_size)
    }

    fn outer_target_main_size(&self) -> CssPixels {
        self.add_main_margin_box_sizes(self.target_main_size)
    }

    fn outer_flex_base_size(&self) -> CssPixels {
        self.add_main_margin_box_sizes(self.flex_base_size)
    }

    fn add_main_margin_box_sizes(&self, content_size: CssPixels) -> CssPixels {
        content_size
            + self.margins.main_before
            + self.margins.main_after
            + self.borders.main_before
            + self.borders.main_after
            + self.padding.main_before
            + self.padding.main_after
    }

    fn add_cross_margin_box_sizes(&self, content_size: CssPixels) -> CssPixels {
        content_size
            + self.margins.cross_before
            + self.margins.cross_after
            + self.borders.cross_before
            + self.borders.cross_after
            + self.padding.cross_before
            + self.padding.cross_after
    }
}

#[derive(Clone)]
struct FlexLine {
    items: Vec<usize>,
    cross_size: CssPixels,
    has_baseline_aligned_items: bool,
    remaining_free_space: Option<CssPixels>,
    chosen_flex_fraction: f64,
    growth_state: formatting_context::FlexLayoutGrowthState,
}

impl Default for FlexLine {
    fn default() -> Self {
        Self {
            items: Vec::new(),
            cross_size: CssPixels::default(),
            has_baseline_aligned_items: false,
            remaining_free_space: None,
            chosen_flex_fraction: 0.0,
            growth_state: formatting_context::FlexLayoutGrowthState::Growing,
        }
    }
}

#[derive(Clone, Copy)]
struct AxisAgnosticAvailableSpace {
    main: AvailableSize,
    cross: AvailableSize,
    space: AvailableSpace,
}

pub(super) struct FlexFormattingContext<'pass> {
    purpose: formatting_context::LayoutPurpose,
    records: &'pass RunRecords<'pass>,
    flex_container: Node,
    layout_mode: LayoutMode,
    callbacks: LayoutPass<'pass>,
    fragments: Option<std::rc::Rc<fragment_tree::RunFragmentBuilder>>,
    should_collect_devtools_layout_data: bool,
    treat_block_axis_percentage_insets_as_auto_beyond_root: bool,
    flex_lines: Vec<FlexLine>,
    flex_items: Vec<FlexItem<'pass>>,
    derived_baselines_of_root_box: DerivedBaselines,
    flex_direction: u8,
    available_space_for_items: Option<AxisAgnosticAvailableSpace>,
    available_space: Option<AvailableSpace>,
    layout_input: Option<LayoutInput>,
    item_percentage_bases: ContainingBlockConstraints,
}

impl<'pass> FlexFormattingContext<'pass> {
    pub(super) fn new(run: &FormattingContextRun<'pass>) -> Self {
        let flex_direction = StyleValues::for_node(&run.callbacks, run.box_).effective_flex_direction();
        Self {
            purpose: run.purpose,
            records: run.records,
            flex_container: run.box_,
            layout_mode: run.layout_mode,
            callbacks: run.callbacks,
            fragments: run.fragments.clone(),
            should_collect_devtools_layout_data: run.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: run
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            flex_lines: Vec::new(),
            flex_items: Vec::new(),
            derived_baselines_of_root_box: DerivedBaselines::default(),
            flex_direction,
            available_space_for_items: None,
            available_space: None,
            layout_input: None,
            item_percentage_bases: ContainingBlockConstraints::default(),
        }
    }

    fn formatting_context_run(&self) -> FormattingContextRun<'pass> {
        FormattingContextRun {
            purpose: self.purpose,
            records: self.records,
            box_: self.flex_container,
            layout_mode: self.layout_mode,
            callbacks: self.callbacks,
            should_collect_devtools_layout_data: self.should_collect_devtools_layout_data,
            treat_block_axis_percentage_insets_as_auto_beyond_root: self
                .treat_block_axis_percentage_insets_as_auto_beyond_root,
            fragments: self.fragments.clone(),
            previous_line_data: None,
        }
    }

    fn item_used(&self, index: usize) -> std::rc::Rc<UsedValues> {
        self.records.used_values(self.flex_items[index].box_)
    }

    fn container_used(&self) -> std::rc::Rc<UsedValues> {
        self.records.used_values(self.flex_container)
    }
    fn style(&self, node: Node) -> StyleValues<'pass> {
        StyleValues::for_node(&self.callbacks, node)
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        NodeFacts::new(&self.callbacks, node)
    }

    fn sizing(&self) -> sizing_context::SizingContext<'pass> {
        sizing_context::SizingContext::new(self.purpose, self.records, self.callbacks)
    }

    fn create_used_values(&self, node: Node) -> std::rc::Rc<UsedValues> {
        let constraints = self.item_percentage_bases;
        self.records.create_used_values(&self.callbacks, node, constraints)
    }

    fn constraints_for_child_context(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> ContainingBlockConstraints {
        self.sizing().constraints_for_child_context(node, constraints)
    }

    fn item_containing_block_constraints(&self) -> ContainingBlockConstraints {
        let mut constraints = self.constraints_for_child_context(
            self.flex_container,
            self.layout_input.unwrap().containing_block_constraints,
        );
        constraints.percentage_basis_inline_size = self.item_percentage_bases.percentage_basis_inline_size;
        constraints.percentage_basis_block_size = self.item_percentage_bases.percentage_basis_block_size;
        constraints
    }

    // https://www.w3.org/TR/css-flexbox-1/#flex-direction-property
    fn inline_axis_is_horizontal(&self, node: Node) -> bool {
        self.style(node).writing_mode() == writing_mode::HORIZONTAL_TB
    }

    fn is_row_layout(&self) -> bool {
        matches!(self.flex_direction, flex_direction::ROW | flex_direction::ROW_REVERSE)
    }

    fn is_legacy_webkit_box(&self) -> bool {
        self.style(self.flex_container).display().inside == display_inside::_WEBKIT_BOX
    }

    fn used_flex_wrap(&self) -> u8 {
        // AD-HOC: A legacy webkit box is always single-line; `flex-wrap` does not apply, matching other engines.
        if self.is_legacy_webkit_box() {
            return flex_wrap::NOWRAP;
        }
        self.style(self.flex_container).flex_wrap()
    }

    fn flex_shrink_factor(&self, node: Node) -> f64 {
        let style = self.style(node);
        // AD-HOC: Items in a legacy webkit box resolve their flex shrink factor from `-webkit-box-flex`, which we
        //         implement as an alias for `flex-grow`; `flex-shrink` does not apply, matching other engines.
        if self.is_legacy_webkit_box() {
            return style.flex_grow();
        }
        style.flex_shrink()
    }

    fn is_single_line(&self) -> bool {
        self.used_flex_wrap() == flex_wrap::NOWRAP
    }

    fn main_axis_is_horizontal(&self) -> bool {
        self.inline_axis_is_horizontal(self.flex_container) == self.is_row_layout()
    }

    fn cross_axis_is_horizontal(&self) -> bool {
        !self.main_axis_is_horizontal()
    }

    // Picks whichever of the two values describes the main (resp. cross) axis of this flex container.
    fn select_main<T>(&self, horizontal: T, vertical: T) -> T {
        if self.main_axis_is_horizontal() {
            horizontal
        } else {
            vertical
        }
    }

    fn select_cross<T>(&self, horizontal: T, vertical: T) -> T {
        self.select_main(vertical, horizontal)
    }

    fn main_sizing_axis(&self) -> SizingAxis {
        self.select_main(SizingAxis::Inline, SizingAxis::Block)
    }

    fn cross_sizing_axis(&self) -> SizingAxis {
        self.select_cross(SizingAxis::Inline, SizingAxis::Block)
    }

    fn main_axis_is_parallel_to_inline_axis(&self, node: Node) -> bool {
        self.main_axis_is_horizontal() == self.inline_axis_is_horizontal(node)
    }

    fn inline_axis_is_reverse(&self, node: Node) -> bool {
        self.style(node).inline_axis_is_reverse()
    }

    fn block_axis_is_reverse(&self, node: Node) -> bool {
        self.style(node).block_axis_is_reverse()
    }

    fn cross_axis_is_reverse(&self) -> bool {
        let mut reverse = if self.main_axis_is_parallel_to_inline_axis(self.flex_container) {
            self.block_axis_is_reverse(self.flex_container)
        } else {
            self.inline_axis_is_reverse(self.flex_container)
        };
        if self.used_flex_wrap() == flex_wrap::WRAP_REVERSE {
            reverse = !reverse;
        }
        reverse
    }

    fn is_direction_reverse(&self) -> bool {
        let mut reverse = if self.is_row_layout() {
            self.inline_axis_is_reverse(self.flex_container)
        } else {
            self.block_axis_is_reverse(self.flex_container)
        };
        if matches!(
            self.flex_direction,
            flex_direction::COLUMN_REVERSE | flex_direction::ROW_REVERSE
        ) {
            reverse = !reverse;
        }
        reverse
    }

    fn main_size_from_cross_size_and_aspect_ratio(
        &self,
        cross_size: CssPixels,
        aspect_ratio: formatting_context::PixelFraction,
    ) -> CssPixels {
        if self.main_axis_is_horizontal() {
            aspect_ratio.multiply(cross_size)
        } else {
            aspect_ratio.divide(cross_size)
        }
    }

    fn cross_size_from_main_size_and_aspect_ratio(
        &self,
        main_size: CssPixels,
        aspect_ratio: formatting_context::PixelFraction,
    ) -> CssPixels {
        if self.main_axis_is_horizontal() {
            aspect_ratio.divide(main_size)
        } else {
            aspect_ratio.multiply(main_size)
        }
    }

    fn has_definite_main_size_used(&self, used: &UsedValues) -> bool {
        self.select_main(used.has_definite_inline_size(), used.has_definite_block_size())
    }

    fn has_definite_cross_size_used(&self, used: &UsedValues) -> bool {
        self.select_cross(used.has_definite_inline_size(), used.has_definite_block_size())
    }

    fn has_definite_main_size(&self, index: usize) -> bool {
        self.has_definite_main_size_used(&self.item_used(index))
    }

    fn has_definite_cross_size(&self, index: usize) -> bool {
        self.has_definite_cross_size_used(&self.item_used(index))
    }

    fn inner_main_size_used(&self, used: &UsedValues) -> CssPixels {
        self.select_main(used.content_inline_size.get(), used.content_block_size.get())
    }

    fn inner_cross_size_used(&self, used: &UsedValues) -> CssPixels {
        self.select_cross(used.content_inline_size.get(), used.content_block_size.get())
    }

    fn inner_main_size(&self, index: usize) -> CssPixels {
        self.inner_main_size_used(&self.item_used(index))
    }

    fn inner_cross_size(&self, index: usize) -> CssPixels {
        self.inner_cross_size_used(&self.item_used(index))
    }

    fn set_has_definite_main_size(&mut self, index: usize) {
        let used = self.item_used(index);
        self.select_main(&used.has_definite_inline_size, &used.has_definite_block_size)
            .set(true);
    }

    fn set_has_definite_cross_size(&mut self, index: usize) {
        let used = self.item_used(index);
        self.select_cross(&used.has_definite_inline_size, &used.has_definite_block_size)
            .set(true);
    }

    fn set_main_size(&mut self, index: usize, size: CssPixels) {
        self.set_main_size_used(&self.item_used(index), size);
    }

    fn set_cross_size(&mut self, index: usize, size: CssPixels) {
        self.set_cross_size_used(&self.item_used(index), size);
    }

    fn set_container_main_size(&mut self, size: CssPixels) {
        self.set_main_size_used(&self.container_used(), size);
    }

    fn set_container_cross_size(&mut self, size: CssPixels) {
        self.set_cross_size_used(&self.container_used(), size);
    }

    fn set_main_size_used(&self, used: &UsedValues, size: CssPixels) {
        if self.main_axis_is_horizontal() {
            used.set_content_inline_size(size);
        } else {
            used.set_content_block_size(size);
        }
    }

    fn set_cross_size_used(&self, used: &UsedValues, size: CssPixels) {
        if self.cross_axis_is_horizontal() {
            used.set_content_inline_size(size);
        } else {
            used.set_content_block_size(size);
        }
    }

    fn computed_main_size(&self, node: Node) -> (&'pass ComputedSize, SizingProperty) {
        let style = self.style(node);
        self.select_main(
            (style.width(), SizingProperty::Width),
            (style.height(), SizingProperty::Height),
        )
    }

    fn computed_main_min_size(&self, node: Node) -> (&'pass ComputedSize, SizingProperty) {
        let style = self.style(node);
        self.select_main(
            (style.min_width(), SizingProperty::MinWidth),
            (style.min_height(), SizingProperty::MinHeight),
        )
    }

    fn computed_main_max_size(&self, node: Node) -> (&'pass ComputedSize, SizingProperty) {
        let style = self.style(node);
        self.select_main(
            (style.max_width(), SizingProperty::MaxWidth),
            (style.max_height(), SizingProperty::MaxHeight),
        )
    }

    fn computed_cross_size(&self, node: Node) -> (&'pass ComputedSize, SizingProperty) {
        let style = self.style(node);
        self.select_cross(
            (style.width(), SizingProperty::Width),
            (style.height(), SizingProperty::Height),
        )
    }

    fn computed_cross_min_size(&self, node: Node) -> (&'pass ComputedSize, SizingProperty) {
        let style = self.style(node);
        self.select_cross(
            (style.min_width(), SizingProperty::MinWidth),
            (style.min_height(), SizingProperty::MinHeight),
        )
    }

    fn computed_cross_max_size(&self, node: Node) -> (&'pass ComputedSize, SizingProperty) {
        let style = self.style(node);
        self.select_cross(
            (style.max_width(), SizingProperty::MaxWidth),
            (style.max_height(), SizingProperty::MaxHeight),
        )
    }

    fn calculate_inner_size(
        &self,
        node: Node,
        axis: SizingAxis,
        property: SizingProperty,
        available_space: AvailableSpace,
    ) -> CssPixels {
        self.calculate_inner_size_with_constraints(
            node,
            axis,
            property,
            available_space,
            self.item_containing_block_constraints(),
        )
    }

    fn calculate_inner_size_with_constraints(
        &self,
        node: Node,
        axis: SizingAxis,
        property: SizingProperty,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.sizing()
            .calculate_inner_size_for_property(node, axis, property, available_space, constraints)
    }

    fn resolve_inner_inline_size(&self, index: usize, property: SizingProperty) -> CssPixels {
        self.calculate_inner_size(
            self.flex_items[index].box_,
            SizingAxis::Inline,
            property,
            AvailableSpace {
                inline_size: self.available_space.unwrap().inline_size,
                block_size: AvailableSize::Indefinite,
            },
        )
    }

    fn resolve_inner_block_size(&self, index: usize, value: &ComputedSize, property: SizingProperty) -> CssPixels {
        // NOTE: When the main axis is horizontal, after we've determined the main size, we use that as the
        //       available inline size for any intrinsic sizing layout needed to resolve the block size.
        let available_space = if self.main_axis_is_horizontal() && value.is_intrinsic_sizing_constraint() {
            AvailableSpace {
                inline_size: self.flex_items[index]
                    .main_size
                    .map(|size| AvailableSize::definite(clamp_to_max_dimension_value(size)))
                    .unwrap_or(AvailableSize::Indefinite),
                block_size: AvailableSize::Indefinite,
            }
        } else {
            self.available_space.unwrap()
        };
        self.calculate_inner_size(
            self.flex_items[index].box_,
            SizingAxis::Block,
            property,
            available_space,
        )
    }

    fn resolve_size_for_axis(
        &self,
        index: usize,
        axis_is_horizontal: bool,
        value: &ComputedSize,
        property: SizingProperty,
    ) -> CssPixels {
        if axis_is_horizontal {
            self.resolve_inner_inline_size(index, property)
        } else {
            self.resolve_inner_block_size(index, value, property)
        }
    }

    fn calculate_fit_content_size(&self, index: usize, axis: SizingAxis, space: AvailableSpace) -> CssPixels {
        self.sizing().calculate_fit_content_size(
            self.flex_items[index].box_,
            axis,
            space,
            self.item_containing_block_constraints(),
        )
    }

    fn should_treat_size_as_auto(&self, node: Node, axis: SizingAxis) -> bool {
        self.sizing().should_treat_size_as_auto(
            node,
            axis,
            self.available_space_for_items.unwrap().space,
            self.item_containing_block_constraints(),
        )
    }

    fn should_treat_main_size_as_auto(&self, node: Node) -> bool {
        self.should_treat_size_as_auto(node, self.main_sizing_axis())
    }

    fn should_treat_cross_size_as_auto(&self, node: Node) -> bool {
        self.should_treat_size_as_auto(
            node,
            if self.cross_axis_is_horizontal() {
                SizingAxis::Inline
            } else {
                SizingAxis::Block
            },
        )
    }

    fn should_treat_max_size_as_none(&self, node: Node, cross: bool) -> bool {
        let axis_is_horizontal = if cross {
            self.cross_axis_is_horizontal()
        } else {
            self.main_axis_is_horizontal()
        };
        let available = if cross {
            self.available_space_for_items.unwrap().cross
        } else {
            self.available_space_for_items.unwrap().main
        };
        if axis_is_horizontal {
            self.sizing().should_treat_max_inline_size_as_none(
                node,
                available,
                self.item_containing_block_constraints(),
            )
        } else {
            self.sizing()
                .should_treat_max_block_size_as_none(node, available, self.item_containing_block_constraints())
        }
    }

    fn has_main_min_size(&self, node: Node) -> bool {
        !self.computed_main_min_size(node).0.is_auto()
    }

    fn has_cross_min_size(&self, node: Node) -> bool {
        !self.computed_cross_min_size(node).0.is_auto()
    }

    fn has_main_max_size(&self, node: Node) -> bool {
        !self.should_treat_max_size_as_none(node, false)
    }

    fn has_cross_max_size(&self, node: Node) -> bool {
        !self.should_treat_max_size_as_none(node, true)
    }

    fn specified_main_min_size(&self, index: usize) -> CssPixels {
        let (value, property) = self.computed_main_min_size(self.flex_items[index].box_);
        self.resolve_size_for_axis(index, self.main_axis_is_horizontal(), value, property)
    }

    fn specified_cross_min_size(&self, index: usize) -> CssPixels {
        let (value, property) = self.computed_cross_min_size(self.flex_items[index].box_);
        self.resolve_size_for_axis(index, self.cross_axis_is_horizontal(), value, property)
    }

    fn specified_main_max_size(&self, index: usize) -> CssPixels {
        let (value, property) = self.computed_main_max_size(self.flex_items[index].box_);
        self.resolve_size_for_axis(index, self.main_axis_is_horizontal(), value, property)
    }

    fn specified_cross_max_size(&self, index: usize) -> CssPixels {
        let (value, property) = self.computed_cross_max_size(self.flex_items[index].box_);
        self.resolve_size_for_axis(index, self.cross_axis_is_horizontal(), value, property)
    }

    // https://drafts.csswg.org/css-flexbox-1/#algo-available
    fn determine_available_space_for_items(&mut self, available_space: AvailableSpace) {
        self.available_space_for_items = Some(if self.main_axis_is_horizontal() {
            AxisAgnosticAvailableSpace {
                main: available_space.inline_size,
                cross: available_space.block_size,
                space: available_space,
            }
        } else {
            AxisAgnosticAvailableSpace {
                main: available_space.block_size,
                cross: available_space.inline_size,
                space: available_space,
            }
        });
    }

    fn populate_specified_margins(&mut self, index: usize) {
        let node = self.flex_items[index].box_;
        let style = self.style(node);
        // Percentages on flex item box-model metrics resolve against the flex container's inline size.
        let basis = self.item_percentage_bases.inline_basis();
        let padding_left = style.padding_left().to_px(basis);
        let padding_right = style.padding_right().to_px(basis);
        let padding_top = style.padding_top().to_px(basis);
        let padding_bottom = style.padding_bottom().to_px(basis);
        {
            let used = self.item_used(index);
            used.padding_left.set(padding_left);
            used.padding_right.set(padding_right);
            used.padding_top.set(padding_top);
            used.padding_bottom.set(padding_bottom);
        }

        let main_axis_is_horizontal = self.main_axis_is_horizontal();
        let item = &mut self.flex_items[index];
        if main_axis_is_horizontal {
            item.borders.main_before = style.border_left_width();
            item.borders.main_after = style.border_right_width();
            item.borders.cross_before = style.border_top_width();
            item.borders.cross_after = style.border_bottom_width();
            item.padding.main_before = padding_left;
            item.padding.main_after = padding_right;
            item.padding.cross_before = padding_top;
            item.padding.cross_after = padding_bottom;
            item.margins.main_before = style.margin_left().to_px(basis);
            item.margins.main_after = style.margin_right().to_px(basis);
            item.margins.cross_before = style.margin_top().to_px(basis);
            item.margins.cross_after = style.margin_bottom().to_px(basis);
            item.margins.main_before_is_auto = style.margin_left().is_auto();
            item.margins.main_after_is_auto = style.margin_right().is_auto();
            item.margins.cross_before_is_auto = style.margin_top().is_auto();
            item.margins.cross_after_is_auto = style.margin_bottom().is_auto();
        } else {
            item.borders.main_before = style.border_top_width();
            item.borders.main_after = style.border_bottom_width();
            item.borders.cross_before = style.border_left_width();
            item.borders.cross_after = style.border_right_width();
            item.padding.main_before = padding_top;
            item.padding.main_after = padding_bottom;
            item.padding.cross_before = padding_left;
            item.padding.cross_after = padding_right;
            item.margins.main_before = style.margin_top().to_px(basis);
            item.margins.main_after = style.margin_bottom().to_px(basis);
            item.margins.cross_before = style.margin_left().to_px(basis);
            item.margins.cross_after = style.margin_right().to_px(basis);
            item.margins.main_before_is_auto = style.margin_top().is_auto();
            item.margins.main_after_is_auto = style.margin_bottom().is_auto();
            item.margins.cross_before_is_auto = style.margin_left().is_auto();
            item.margins.cross_after_is_auto = style.margin_right().is_auto();
        }
    }

    // https://www.w3.org/TR/css-flexbox-1/#flex-items
    fn generate_anonymous_flex_items(&mut self) {
        // More like, sift through the already generated items.
        // After this step no items are to be added or removed from flex_items!
        // It holds every item we need to consider and there should be nothing in the following
        // calculations that could change that.
        // This is particularly important since we take references to the items stored in flex_items
        // later, whose addresses won't be stable if we added or removed any items.
        let mut buckets: HashMap<i32, Vec<FlexItem>> = HashMap::default();
        let mut child = self.callbacks.first_child(self.flex_container);
        while !child.is_invalid() {
            let next = self.callbacks.next_sibling(child);
            let facts = self.facts(child);
            if facts.is_box() {
                let skip = self.callbacks.can_skip_is_anonymous_text_run(child);
                // Skip any "out-of-flow" children
                if !skip && !facts.is_absolutely_positioned() {
                    // Flex inhibits floating, so only absolute positioning is out of flow here.
                    self.callbacks.arena().set_node_flag(child, NodeFlag::IsFlexItem, true);
                    self.create_used_values(child);
                    let item = FlexItem::new(child);
                    buckets.entry(self.style(child).order()).or_default().push(item);
                }
            }
            child = next;
        }

        let keys = order_modified_keys(&buckets, self.is_direction_reverse());
        for key in keys {
            self.flex_items.extend(buckets.remove(&key).unwrap());
        }
        for index in 0..self.flex_items.len() {
            self.populate_specified_margins(index);
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#propdef-flex-basis
    fn used_flex_basis_for_item(&self, index: usize) -> UsedFlexBasis<'pass> {
        let node = self.flex_items[index].box_;
        let style = self.style(node);
        if style.flex_basis_is_content() {
            return UsedFlexBasis::Content;
        }
        let mut value = style.flex_basis();
        let mut property = SizingProperty::FlexBasis;
        if value.is_auto() {
            // https://drafts.csswg.org/css-flexbox-1/#valdef-flex-basis-auto
            // When specified on a flex item, the auto keyword retrieves the value of the main size property as the used flex-basis.
            // If that value is itself auto, then the used value is content.
            (value, property) = self.computed_main_size(node);
            if value.is_auto() {
                return UsedFlexBasis::Content;
            }
        }
        // For example, percentage values of flex-basis are resolved against the flex item’s containing block
        // (i.e. its flex container); and if that containing block’s size is indefinite,
        // the used value for flex-basis is content.
        if value.is_percentage() && !self.has_definite_main_size_used(&self.container_used()) {
            return UsedFlexBasis::Content;
        }
        UsedFlexBasis::Size { value, property }
    }

    // This function takes a size in the main axis and adjusts it according to the aspect ratio of the box
    // if the min/max constraints in the cross axis forces us to come up with a new main axis size.
    fn adjust_main_size_through_aspect_ratio(
        &self,
        node: Node,
        mut main_size: CssPixels,
        min_cross_size: &ComputedSize,
        max_cross_size: &ComputedSize,
    ) -> CssPixels {
        let ratio = self.facts(node).preferred_aspect_ratio().unwrap();
        let reference = self.inner_cross_size_used(&self.container_used());
        if !self.should_treat_max_size_as_none(node, true) {
            main_size =
                main_size.min(self.main_size_from_cross_size_and_aspect_ratio(max_cross_size.to_px(reference), ratio));
        }
        if !min_cross_size.is_auto()
            && (!min_cross_size.contains_percentage() || self.has_definite_cross_size_used(&self.container_used()))
        {
            main_size =
                main_size.max(self.main_size_from_cross_size_and_aspect_ratio(min_cross_size.to_px(reference), ratio));
        }
        main_size
    }

    fn adjust_cross_size_through_aspect_ratio(
        &self,
        node: Node,
        mut cross_size: CssPixels,
        min_main_size: &ComputedSize,
        max_main_size: &ComputedSize,
    ) -> CssPixels {
        let ratio = self.facts(node).preferred_aspect_ratio().unwrap();
        let reference = self.inner_main_size_used(&self.container_used());
        if !self.should_treat_max_size_as_none(node, false) {
            cross_size =
                cross_size.min(self.cross_size_from_main_size_and_aspect_ratio(max_main_size.to_px(reference), ratio));
        }
        if !min_main_size.is_auto() {
            cross_size =
                cross_size.max(self.cross_size_from_main_size_and_aspect_ratio(min_main_size.to_px(reference), ratio));
        }
        cross_size
    }

    fn calculate_min_content_inline_size(&self, index: usize) -> CssPixels {
        let sizing = self.sizing();
        let node = self.flex_items[index].box_;
        let constraints = self.item_containing_block_constraints();
        let used = self.item_used(index);
        if used.has_definite_block_size() {
            sizing.calculate_min_content_inline_size_at_definite_block_size(
                node,
                constraints,
                used.content_block_size.get(),
            )
        } else {
            sizing.calculate_min_content_inline_size(node, constraints)
        }
    }

    fn calculate_max_content_inline_size(&self, index: usize) -> CssPixels {
        let sizing = self.sizing();
        let node = self.flex_items[index].box_;
        let constraints = self.item_containing_block_constraints();
        let used = self.item_used(index);
        if used.has_definite_block_size() {
            sizing.calculate_max_content_inline_size_at_definite_block_size(
                node,
                constraints,
                used.content_block_size.get(),
            )
        } else {
            sizing.calculate_max_content_inline_size(node, constraints)
        }
    }

    fn calculate_min_content_block_size(&self, index: usize, inline_size: CssPixels) -> CssPixels {
        self.sizing().calculate_min_content_block_size(
            self.flex_items[index].box_,
            inline_size,
            self.item_containing_block_constraints(),
        )
    }

    fn calculate_max_content_block_size(&self, index: usize, inline_size: CssPixels) -> CssPixels {
        self.sizing().calculate_max_content_block_size(
            self.flex_items[index].box_,
            inline_size,
            self.item_containing_block_constraints(),
        )
    }

    fn calculate_inline_size_for_intrinsic_block_size(&self, index: usize) -> CssPixels {
        let node = self.flex_items[index].box_;
        let style = self.style(node);
        // We can resolve percentage min/max-width if the available inline size is definite.
        let can_resolve_percentages = matches!(
            self.available_space_for_items.unwrap().space.inline_size,
            AvailableSize::Definite(_)
        );
        let min_inline_size =
            if !style.min_width().is_auto() && (!style.min_width().contains_percentage() || can_resolve_percentages) {
                self.resolve_inner_inline_size(index, SizingProperty::MinWidth)
            } else {
                CssPixels::default()
            };
        let max_inline_size = if !self.should_treat_max_size_as_none_for_axis(
            node,
            SizingAxis::Inline,
            self.available_space_for_items.unwrap().space.inline_size,
        ) && (!style.max_width().contains_percentage() || can_resolve_percentages)
        {
            self.resolve_inner_inline_size(index, SizingProperty::MaxWidth)
        } else {
            CssPixels::from_raw(i32::MAX)
        };

        let inline_size = if self.should_treat_size_as_auto(node, SizingAxis::Inline) || style.width().is_fit_content()
        {
            self.calculate_fit_content_size(index, SizingAxis::Inline, self.available_space_for_items.unwrap().space)
        } else if style.width().is_min_content() {
            self.calculate_min_content_inline_size(index)
        } else if style.width().is_max_content() {
            self.calculate_max_content_inline_size(index)
        } else {
            CssPixels::default()
        };
        css_clamp(inline_size, min_inline_size, max_inline_size)
    }

    fn should_treat_max_size_as_none_for_axis(&self, node: Node, axis: SizingAxis, available: AvailableSize) -> bool {
        match axis {
            SizingAxis::Inline => self.sizing().should_treat_max_inline_size_as_none(
                node,
                available,
                self.item_containing_block_constraints(),
            ),
            SizingAxis::Block => self.sizing().should_treat_max_block_size_as_none(
                node,
                available,
                self.item_containing_block_constraints(),
            ),
        }
    }

    fn intrinsic_block_inline_size(&self, index: usize) -> CssPixels {
        let mut available = self
            .item_used(index)
            .available_inner_space_or_constraints_from(self.available_space_for_items.unwrap().space);
        if available.inline_size == AvailableSize::Indefinite {
            available.inline_size = AvailableSize::definite(self.calculate_inline_size_for_intrinsic_block_size(index));
        }
        available.inline_size.to_px_or_zero()
    }

    fn calculate_min_content_main_size(&self, index: usize) -> CssPixels {
        if self.main_axis_is_horizontal() {
            self.calculate_min_content_inline_size(index)
        } else {
            self.calculate_min_content_block_size(index, self.intrinsic_block_inline_size(index))
        }
    }

    fn calculate_max_content_main_size(&self, index: usize) -> CssPixels {
        if self.main_axis_is_horizontal() {
            self.calculate_max_content_inline_size(index)
        } else {
            self.calculate_max_content_block_size(index, self.intrinsic_block_inline_size(index))
        }
    }

    fn calculate_min_content_cross_size(&self, index: usize) -> CssPixels {
        if self.cross_axis_is_horizontal() {
            self.calculate_min_content_inline_size(index)
        } else {
            self.calculate_min_content_block_size(index, self.intrinsic_block_inline_size(index))
        }
    }

    fn calculate_max_content_cross_size(&self, index: usize) -> CssPixels {
        if self.cross_axis_is_horizontal() {
            self.calculate_max_content_inline_size(index)
        } else {
            self.calculate_max_content_block_size(index, self.intrinsic_block_inline_size(index))
        }
    }

    fn calculate_fit_content_main_size(&self, index: usize) -> CssPixels {
        self.calculate_fit_content_size(
            index,
            if self.main_axis_is_horizontal() {
                SizingAxis::Inline
            } else {
                SizingAxis::Block
            },
            self.available_space_for_items.unwrap().space,
        )
    }

    // https://drafts.csswg.org/css-flexbox-1/#algo-main-item
    fn determine_flex_base_size(&mut self, index: usize) {
        let node = self.flex_items[index].box_;
        let basis = self.used_flex_basis_for_item(index);
        self.flex_items[index].used_flex_basis = basis;
        let basis_is_definite = match basis {
            UsedFlexBasis::Content => false,
            UsedFlexBasis::Size { value, .. } => {
                if value.is_auto() || value.is_none() || value.is_intrinsic_sizing_constraint() {
                    false
                } else if value.is_length() {
                    true
                } else if value.kind == ComputedSizeKind::Calculated {
                    !value.contains_percentage() || self.has_definite_main_size_used(&self.container_used())
                } else {
                    debug_assert!(value.is_percentage());
                    self.has_definite_main_size_used(&self.container_used())
                }
            }
        };
        self.flex_items[index].used_flex_basis_is_definite = basis_is_definite;

        let mut flex_base_size = match basis {
            // A. If the item has a definite used flex basis, that’s the flex base size.
            UsedFlexBasis::Size { value, property } if basis_is_definite => {
                self.resolve_size_for_axis(index, self.main_axis_is_horizontal(), value, property)
            }
            // AD-HOC: If we're sizing the flex container under a min-content constraint in the main axis,
            //         non-replaced flex items resolve percentages in the main axis to 0.
            // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
            _ if self.facts(node).is_replaced_box()
                && self.available_space_for_items.unwrap().main == AvailableSize::MinContent
                && self.computed_main_size(node).0.contains_percentage() =>
            {
                CssPixels::default()
            }
            // B. If the flex item has ...
            //    - an intrinsic aspect ratio,
            //    - a used flex basis of content, and
            //    - a definite cross size,
            UsedFlexBasis::Content
                if self.facts(node).preferred_aspect_ratio().is_some() && self.has_definite_cross_size(index) =>
            {
                // flex_base_size is calculated from definite cross size and intrinsic aspect ratio

                // AD-HOC: Mark that we resolved the main size from the aspect ratio,
                //         so that we can mark the item as having definite main size after resolving flexible lengths.
                self.flex_items[index].main_size_was_resolved_from_aspect_ratio = true;
                let (min_cross, _) = self.computed_cross_min_size(node);
                let (max_cross, _) = self.computed_cross_max_size(node);
                self.adjust_main_size_through_aspect_ratio(
                    node,
                    self.main_size_from_cross_size_and_aspect_ratio(
                        self.inner_cross_size(index),
                        self.facts(node).preferred_aspect_ratio().unwrap(),
                    ),
                    min_cross,
                    max_cross,
                )
            }
            // C. If the used flex basis is content or depends on its available space,
            //    and the flex container is being sized under a min-content or max-content constraint
            //    (e.g. when performing automatic table layout [CSS21]), size the item under that constraint.
            //    The flex base size is the item’s resulting main size.
            UsedFlexBasis::Content if self.available_space_for_items.unwrap().main == AvailableSize::MinContent => {
                self.calculate_min_content_main_size(index)
            }
            UsedFlexBasis::Content if self.available_space_for_items.unwrap().main == AvailableSize::MaxContent => {
                self.calculate_max_content_main_size(index)
            }
            // E. Otherwise, size the item into the available space using its used flex basis in place of its main size,
            //    treating a value of content as max-content. If a cross size is needed to determine the main size
            //    (e.g. when the flex item’s main size is in its block axis) and the flex item’s cross size is auto and not definite,
            //    in this calculation use fit-content as the flex item’s cross size.
            //    The flex base size is the item’s resulting main size.
            UsedFlexBasis::Size { value, .. } if value.is_fit_content() => self.calculate_fit_content_main_size(index),
            UsedFlexBasis::Size { value, .. } if value.is_max_content() => self.calculate_max_content_main_size(index),
            UsedFlexBasis::Size { value, .. } if value.is_min_content() => self.calculate_min_content_main_size(index),
            // NOTE: If the flex item has a definite main size, just use that as the flex base size.
            _ if self.has_definite_main_size(index) => self.inner_main_size(index),
            // NOTE: There's a fundamental problem with many CSS specifications in that they neglect to mention
            //       which inline size to provide when calculating the intrinsic block size of a box in various situations.
            //       Spec bug: https://github.com/w3c/csswg-drafts/issues/2890

            // NOTE: This is one of many situations where that causes trouble: if this is a flex column layout,
            //       we may need to calculate the intrinsic block size of a flex item. This requires an inline size,
            //       but an inline size won't be determined until later on in the flex layout algorithm.
            //       In the specific case above (E), the spec mentions using `fit-content` in place of `auto`
            //       if "a cross size is needed to determine the main size", so that's exactly what we do.

            // NOTE: Finding a suitable inline size for intrinsic block-size determination actually happens elsewhere,
            //       in the various helpers that calculate the intrinsic sizes of a flex item,
            //       e.g. calculate_min_content_main_size().
            UsedFlexBasis::Content => self.calculate_max_content_main_size(index),
            UsedFlexBasis::Size { .. } => self.calculate_fit_content_main_size(index),
        };

        // AD-HOC: This is not mentioned in the spec, but if the item has an aspect ratio, we may need
        //         to adjust the main size in these ways:
        //         - using stretch-fit main size if the flex basis is indefinite, there is no
        //           intrinsic size and no cross size to resolve the ratio against.
        //         - in response to cross size min/max constraints.
        let facts = self.facts(node);
        if facts.has_auto_content_aspect_ratio() {
            if !basis_is_definite
                && !facts.has_auto_content_width()
                && !facts.has_auto_content_height()
                && !self.has_definite_cross_size(index)
                && self.has_definite_main_size_used(&self.container_used())
            {
                flex_base_size = self.inner_main_size_used(&self.container_used());
            }
            let (min_cross, _) = self.computed_cross_min_size(node);
            let (max_cross, _) = self.computed_cross_max_size(node);
            flex_base_size = self.adjust_main_size_through_aspect_ratio(node, flex_base_size, min_cross, max_cross);
        }
        // NOTE: We don't clamp the main size until we have them for all items.
        self.flex_items[index].flex_base_size = flex_base_size;
    }

    // https://drafts.csswg.org/css-flexbox-1/#min-size-auto
    fn automatic_minimum_size(&self, index: usize) -> CssPixels {
        // AD-HOC: Items in a legacy webkit box have an automatic minimum size of zero, matching other engines.
        if self.is_legacy_webkit_box() {
            return CssPixels::default();
        }

        // To provide a more reasonable default minimum size for flex items,
        // the used value of a main axis automatic minimum size on a flex item that is not a scroll container is its content-based minimum size;
        // for scroll containers the automatic minimum size is zero, as usual.
        if !self.facts(self.flex_items[index].box_).is_scroll_container() {
            self.content_based_minimum_size(index)
        } else {
            CssPixels::default()
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#specified-size-suggestion
    fn specified_size_suggestion(&self, index: usize) -> Option<CssPixels> {
        let node = self.flex_items[index].box_;
        // If the item’s preferred main size is definite and not automatic,
        // then the specified size suggestion is that size. It is otherwise undefined.
        if self.has_definite_main_size(index) && !self.should_treat_main_size_as_auto(node) {
            // NOTE: We use resolve_inner_{inline,block}_size to ensure that CSS box-sizing is respected.
            let (value, property) = self.computed_main_size(node);
            Some(self.resolve_size_for_axis(index, self.main_axis_is_horizontal(), value, property))
        } else {
            None
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#content-size-suggestion
    fn content_size_suggestion(&self, index: usize) -> CssPixels {
        let node = self.flex_items[index].box_;
        let mut suggestion = self.calculate_min_content_main_size(index);
        if self.facts(node).preferred_aspect_ratio().is_some() {
            suggestion = self.adjust_main_size_through_aspect_ratio(
                node,
                suggestion,
                self.computed_cross_min_size(node).0,
                self.computed_cross_max_size(node).0,
            );
        }
        suggestion
    }

    // https://drafts.csswg.org/css-flexbox-1/#transferred-size-suggestion
    fn transferred_size_suggestion(&self, index: usize) -> Option<CssPixels> {
        let node = self.flex_items[index].box_;
        // If the item has a preferred aspect ratio and its preferred cross size is definite,
        // then the transferred size suggestion is that size
        // (clamped by its minimum and maximum cross sizes if they are definite), converted through the aspect ratio.
        let ratio = self.facts(node).preferred_aspect_ratio()?;
        if !self.has_definite_cross_size(index) {
            // It is otherwise undefined.
            return None;
        }
        Some(self.adjust_main_size_through_aspect_ratio(
            node,
            self.main_size_from_cross_size_and_aspect_ratio(self.inner_cross_size(index), ratio),
            self.computed_cross_min_size(node).0,
            self.computed_cross_max_size(node).0,
        ))
    }

    // https://drafts.csswg.org/css-flexbox-1/#content-based-minimum-size
    fn content_based_minimum_size(&self, index: usize) -> CssPixels {
        let node = self.flex_items[index].box_;
        let content = self.content_size_suggestion(index);
        let transferred = self.transferred_size_suggestion(index);
        let specified = self.specified_size_suggestion(index);
        // The content-based minimum size of a flex item differs depending on whether the flex item is replaced or not:
        // -> For replaced elements
        let mut size = if self.facts(node).is_replaced_box() {
            // Use the smaller of the content size suggestion and the transferred size suggestion (if one exists),
            // capped by the specified size suggestion (if one exists).
            transferred.map_or(content, |value| content.min(value))
        } else {
            // -> For non-replaced elements

            // Use the larger of the content size suggestion and the transferred size suggestion (if one exists),
            // capped by the specified size suggestion (if one exists).
            transferred.map_or(content, |value| content.max(value))
        };
        if let Some(specified) = specified {
            size = size.min(specified);
        }
        // In either case, the size is clamped by the maximum main size if it’s definite.
        if self.has_main_max_size(node) {
            size.min(self.specified_main_max_size(index))
        } else {
            size
        }
    }

    fn main_gap(&self) -> CssPixels {
        let style = self.style(self.flex_container);
        let gap = if self.is_row_layout() {
            style.column_gap()
        } else {
            style.row_gap()
        };
        gap.to_px(self.inner_main_size_used(&self.container_used()))
    }

    fn cross_gap(&self) -> CssPixels {
        let style = self.style(self.flex_container);
        let gap = if self.is_row_layout() {
            style.row_gap()
        } else {
            style.column_gap()
        };
        gap.to_px(self.inner_cross_size_used(&self.container_used()))
    }

    // https://www.w3.org/TR/css-flexbox-1/#algo-line-break
    fn collect_flex_items_into_flex_lines(&mut self) {
        // If the flex container is single-line, collect all the flex items into a single flex line.
        if self.is_single_line() {
            let mut items: Vec<_> = (0..self.flex_items.len()).collect();
            if self.is_direction_reverse() {
                items.reverse();
            }
            self.flex_lines.push(FlexLine {
                items,
                ..Default::default()
            });
            return;
        }

        // Otherwise, starting from the first uncollected item, collect consecutive items one by one
        // until the first time that the next collected item would not fit into the flex container’s inner main size
        // (or until a forced break is encountered, see §10 Fragmenting Flex Layout).
        // If the very first uncollected item wouldn't fit, collect just it into the line.

        // For this step, the size of a flex item is its outer hypothetical main size. (Note: This can be negative.)

        // Repeat until all flex items have been collected into flex lines.

        let mut line = FlexLine::default();
        let mut line_main_size = CssPixels::default();
        for index in 0..self.flex_items.len() {
            let outer = self.flex_items[index].outer_hypothetical_main_size();
            if !line.items.is_empty()
                && self
                    .available_space_for_items
                    .unwrap()
                    .main
                    .pixels_greater_than(line_main_size + outer)
            {
                self.flex_lines.push(line);
                line = FlexLine::default();
                line_main_size = CssPixels::default();
            }
            if self.is_direction_reverse() {
                line.items.insert(0, index);
            } else {
                line.items.push(index);
            }
            line_main_size += outer;
            // CSS-FLEXBOX-2: Account for gap between flex items.
            line_main_size += self.main_gap();
        }
        self.flex_lines.push(line);
    }

    fn remaining_free_space_for_line(
        &self,
        line_index: usize,
        available_main_size: AvailableSize,
    ) -> Option<CssPixels> {
        // AD-HOC: If the container is sized under max-content constraints, then remaining_free_space won't have
        //         a value to avoid leaking an infinite value into layout calculations.
        if available_main_size.is_intrinsic_sizing_constraint() {
            return None;
        }
        let line = &self.flex_lines[line_index];
        // Sum the outer sizes of all items on the line, and subtract this from the flex container’s inner main size.
        // For frozen items, use their outer target main size; for other items, use their outer flex base size.
        let mut sum = CssPixels::default();
        for index in line.items.iter().copied() {
            sum += if self.flex_items[index].frozen {
                self.flex_items[index].outer_target_main_size()
            } else {
                self.flex_items[index].outer_flex_base_size()
            };
        }
        // CSS-FLEXBOX-2: Account for gap between flex items.
        sum += self.main_gap() * line.items.len().wrapping_sub(1);
        // AD-HOC: Note that we're using our own "available main size" explained above
        //         instead of the flex container’s inner main size.
        Some(available_main_size.to_px_or_zero() - sum)
    }

    fn sum_of_unfrozen_flex_factors(&self, line_index: usize) -> f64 {
        self.flex_lines[line_index]
            .items
            .iter()
            .copied()
            .filter(|index| !self.flex_items[*index].frozen)
            .map(|index| self.flex_items[index].flex_factor)
            .sum()
    }

    fn sum_of_unfrozen_scaled_shrink_factors(&self, line_index: usize) -> f64 {
        self.flex_lines[line_index]
            .items
            .iter()
            .copied()
            .filter(|index| !self.flex_items[*index].frozen)
            .map(|index| self.flex_items[index].scaled_flex_shrink_factor)
            .sum()
    }

    // https://drafts.csswg.org/css-flexbox-1/#resolve-flexible-lengths
    fn resolve_flexible_lengths_for_line(&mut self, line_index: usize) {
        // AD-HOC: The spec tells us to use the "flex container’s inner main size" in this algorithm,
        //         but that doesn't work when we're sizing under a max-content constraint.
        //         In that case, there is effectively infinite size available in the main axis,
        //         but the inner main size has not been assigned yet.
        //         We solve this by calculating our own "available main size" here, which is essentially
        //         infinity under max-content, 0 under min-content, and the inner main size otherwise.
        let available_main_size = if self
            .available_space_for_items
            .unwrap()
            .main
            .is_intrinsic_sizing_constraint()
        {
            self.available_space_for_items.unwrap().main
        } else {
            AvailableSize::definite(self.inner_main_size_used(&self.container_used()))
        };
        let item_count = self.flex_lines[line_index].items.len();
        // 1. Determine the used flex factor.

        // Sum the outer hypothetical main sizes of all items on the line.
        // If the sum is less than the flex container’s inner main size,
        // use the flex grow factor for the rest of this algorithm; otherwise, use the flex shrink factor
        let mut hypothetical_sum = CssPixels::default();
        for item_position in 0..item_count {
            let index = self.flex_lines[line_index].items[item_position];
            hypothetical_sum += self.flex_items[index].outer_hypothetical_main_size();
        }
        // CSS-FLEXBOX-2: Account for gap between flex items.
        hypothetical_sum += self.main_gap() * item_count.wrapping_sub(1);
        // AD-HOC: Note that we're using our own "available main size" explained above
        //         instead of the flex container’s inner main size.
        let factor = if available_main_size.pixels_less_than(hypothetical_sum) {
            FlexFactor::Grow
        } else {
            FlexFactor::Shrink
        };
        self.flex_lines[line_index].growth_state = if factor == FlexFactor::Grow {
            formatting_context::FlexLayoutGrowthState::Growing
        } else {
            formatting_context::FlexLayoutGrowthState::Shrinking
        };

        // 2. Each item in the flex line has a target main size, initially set to its flex base size.
        //    Each item is initially unfrozen and may become frozen.
        for item_position in 0..item_count {
            let index = self.flex_lines[line_index].items[item_position];
            self.flex_items[index].target_main_size = self.flex_items[index].flex_base_size;
            self.flex_items[index].frozen = false;
        }
        // 3. Size inflexible items.

        for item_position in 0..item_count {
            let index = self.flex_lines[line_index].items[item_position];
            self.flex_items[index].flex_factor = if factor == FlexFactor::Grow {
                self.style(self.flex_items[index].box_).flex_grow()
            } else {
                self.flex_shrink_factor(self.flex_items[index].box_)
            };
            let item = &mut self.flex_items[index];
            // Freeze, setting its target main size to its hypothetical main size…
            // - any item that has a flex factor of zero
            // - if using the flex grow factor: any item that has a flex base size greater than its hypothetical main size
            // - if using the flex shrink factor: any item that has a flex base size smaller than its hypothetical main size
            if item.flex_factor == 0.0
                || (factor == FlexFactor::Grow && item.flex_base_size > item.hypothetical_main_size)
                || (factor == FlexFactor::Shrink && item.flex_base_size < item.hypothetical_main_size)
            {
                item.frozen = true;
                item.target_main_size = item.hypothetical_main_size;
            }
        }

        // 4. Calculate initial free space
        let initial_free_space = self.remaining_free_space_for_line(line_index, available_main_size);
        // 5. Loop
        while (0..item_count).any(|item_position| {
            let index = self.flex_lines[line_index].items[item_position];
            !self.flex_items[index].frozen
        }) {
            // a. Check for flexible items.
            //    If all the flex items on the line are frozen, exit this loop.

            // b. Calculate the remaining free space as for initial free space, above.
            let mut remaining = self.remaining_free_space_for_line(line_index, available_main_size);
            // If the sum of the unfrozen flex items’ flex factors is less than one, multiply the initial free space by this sum.
            let factor_sum = self.sum_of_unfrozen_flex_factors(line_index);
            if factor_sum < 1.0
                && let Some(initial) = initial_free_space
            {
                let value = CssPixels::nearest_value_for(initial.to_double() * factor_sum);
                // If the magnitude of this value is less than the magnitude of the remaining free space, use this as the remaining free space.
                if remaining.is_none_or(|current| abs(value) < abs(current)) {
                    remaining = Some(value);
                }
            }
            self.flex_lines[line_index].remaining_free_space = remaining;
            // AD-HOC: We allow the remaining free space to be infinite, but we can't let infinity
            //         leak into the layout geometry, so we treat infinity as zero when used in arithmetic.
            let arithmetic_free_space = remaining.unwrap_or_default();

            // c. If the remaining free space is non-zero, distribute it proportional to the flex factors:
            if remaining != Some(CssPixels::default()) {
                // If using the flex grow factor
                if factor == FlexFactor::Grow {
                    // For every unfrozen item on the line,
                    // find the ratio of the item’s flex grow factor to the sum of the flex grow factors of all unfrozen items on the line.
                    let sum = self.sum_of_unfrozen_flex_factors(line_index);
                    for item_position in 0..item_count {
                        let index = self.flex_lines[line_index].items[item_position];
                        if self.flex_items[index].frozen {
                            continue;
                        }
                        let ratio = self.flex_items[index].flex_factor / sum;
                        // Set the item’s target main size to its flex base size plus a fraction of the remaining free space proportional to the ratio.
                        self.flex_items[index].target_main_size =
                            self.flex_items[index].flex_base_size + arithmetic_free_space.scaled(ratio);
                    }
                } else {
                    // If using the flex shrink factor

                    // For every unfrozen item on the line, multiply its flex shrink factor by its inner flex base size, and note this as its scaled flex shrink factor.
                    for item_position in 0..item_count {
                        let index = self.flex_lines[line_index].items[item_position];
                        if self.flex_items[index].frozen {
                            continue;
                        }
                        self.flex_items[index].scaled_flex_shrink_factor =
                            self.flex_items[index].flex_factor * self.flex_items[index].flex_base_size.to_double();
                    }
                    let sum = self.sum_of_unfrozen_scaled_shrink_factors(line_index);
                    for item_position in 0..item_count {
                        let index = self.flex_lines[line_index].items[item_position];
                        if self.flex_items[index].frozen {
                            continue;
                        }
                        let ratio = if sum == 0.0 {
                            1.0
                        } else {
                            self.flex_items[index].scaled_flex_shrink_factor / sum
                        };
                        // Find the ratio of the item’s scaled flex shrink factor to the sum of the scaled flex shrink factors of all unfrozen items on the line.

                        // Set the item’s target main size to its flex base size minus a fraction of the absolute value of the remaining free space proportional to the ratio.
                        // (Note this may result in a negative inner main size; it will be corrected in the next step.)
                        self.flex_items[index].target_main_size =
                            self.flex_items[index].flex_base_size - abs(arithmetic_free_space).scaled(ratio);
                    }
                }
            }

            // d. Fix min/max violations.
            let mut total_violation = CssPixels::default();
            // Clamp each non-frozen item’s target main size by its used min and max main sizes and floor its content-box size at zero.
            for item_position in 0..item_count {
                let index = self.flex_lines[line_index].items[item_position];
                if self.flex_items[index].frozen {
                    continue;
                }
                let node = self.flex_items[index].box_;
                let min_size = if self.has_main_min_size(node) {
                    self.specified_main_min_size(index)
                } else {
                    self.automatic_minimum_size(index)
                };
                let max_size = if self.has_main_max_size(node) {
                    self.specified_main_max_size(index)
                } else {
                    CssPixels::from_raw(i32::MAX)
                };
                let original = self.flex_items[index].target_main_size;
                let target = css_clamp(original, min_size, max_size).max(CssPixels::default());
                self.flex_items[index].target_main_size = target;
                if target < original {
                    // If the item’s target main size was made smaller by this, it’s a max violation.
                    self.flex_items[index].is_max_violation = true;
                }
                if target > original {
                    // If the item’s target main size was made larger by this, it’s a min violation.
                    self.flex_items[index].is_min_violation = true;
                }
                total_violation += target - original;
            }

            // e. Freeze over-flexed items.
            //    The total violation is the sum of the adjustments from the previous step ∑(clamped size - unclamped size).

            // If the total violation is:
            // Zero
            //   Freeze all items.

            // Positive
            //   Freeze all the items with min violations.

            // Negative
            //   Freeze all the items with max violations.
            for item_position in 0..item_count {
                let index = self.flex_lines[line_index].items[item_position];
                if self.flex_items[index].frozen {
                    continue;
                }
                if total_violation == CssPixels::default()
                    || (total_violation > CssPixels::default() && self.flex_items[index].is_min_violation)
                    || (total_violation < CssPixels::default() && self.flex_items[index].is_max_violation)
                {
                    self.flex_items[index].frozen = true;
                }
            }
            // NOTE: This freezes at least one item, ensuring that the loop makes progress and eventually terminates.

            // f. Return to the start of this loop.
        }

        // NOTE: Calculate the remaining free space once again here, since it's needed later when aligning items.
        self.flex_lines[line_index].remaining_free_space =
            self.remaining_free_space_for_line(line_index, available_main_size);
        // 6. Set each item’s used main size to its target main size.
        for item_position in 0..item_count {
            let index = self.flex_lines[line_index].items[item_position];
            let target = self.flex_items[index].target_main_size;
            self.flex_items[index].main_size = Some(target);
            self.set_main_size(index, target);
            // https://drafts.csswg.org/css-flexbox-1/#definite-sizes
            // 1. If the flex container has a definite main size, then the post-flexing main sizes of its flex items are treated as definite.
            // 2. If a flex item’s flex basis is definite, then its post-flexing main size is also definite.
            // AD-HOC: 3. If a flex item’s main size was resolved from its intrinsic aspect ratio, then its post-flexing main size is also definite.
            let item_is_orthogonal = self.inline_axis_is_horizontal(self.flex_items[index].box_)
                != self.inline_axis_is_horizontal(self.flex_container);
            let container_has_vertical_inline_main_axis =
                self.is_row_layout() && !self.inline_axis_is_horizontal(self.flex_container);
            if self.has_definite_main_size_used(&self.container_used())
                || self.flex_items[index].used_flex_basis_is_definite
                || self.flex_items[index].main_size_was_resolved_from_aspect_ratio
                || container_has_vertical_inline_main_axis
                || (item_is_orthogonal && self.main_axis_is_parallel_to_inline_axis(self.flex_items[index].box_))
            {
                self.set_has_definite_main_size(index);
            }
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#resolve-flexible-lengths
    fn resolve_flexible_lengths(&mut self) {
        for line_index in 0..self.flex_lines.len() {
            self.resolve_flexible_lengths_for_line(line_index);
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#stretched
    fn flex_item_is_stretched(&self, index: usize) -> bool {
        let alignment = self.alignment_for_item(self.flex_items[index].box_);
        if !matches!(alignment, align_items::STRETCH | align_items::NORMAL) {
            return false;
        }
        // If the cross size property of the flex item computes to auto, and neither of the cross-axis margins are auto, the flex item is stretched.
        self.computed_cross_size(self.flex_items[index].box_).0.is_auto()
            && !self.flex_items[index].margins.cross_before_is_auto
            && !self.flex_items[index].margins.cross_after_is_auto
    }

    fn alignment_for_item(&self, node: Node) -> u8 {
        match self.style(node).align_self() {
            align_self::AUTO => self.style(self.flex_container).align_items(),
            align_self::END => align_items::END,
            align_self::NORMAL => align_items::NORMAL,
            align_self::SELF_START => align_items::SELF_START,
            align_self::SELF_END => align_items::SELF_END,
            align_self::FLEX_START => align_items::FLEX_START,
            align_self::FLEX_END => align_items::FLEX_END,
            align_self::CENTER => align_items::CENTER,
            align_self::BASELINE => align_items::BASELINE,
            align_self::START => align_items::START,
            align_self::STRETCH => align_items::STRETCH,
            align_self::SAFE => align_items::SAFE,
            align_self::UNSAFE => align_items::UNSAFE,
            _ => unreachable!("invalid align-self"),
        }
    }

    // https://drafts.csswg.org/css-flexbox/#hypothetical-cross-size
    fn determine_hypothetical_cross_size_of_item(&mut self, index: usize) {
        // Determine the hypothetical cross size of each item by performing layout as if it were an in-flow block-level box
        // with the used main size and the given available space, treating auto as fit-content.

        let node = self.flex_items[index].box_;
        let min = self.computed_cross_min_size(node).0;
        let clamp_min = if min.is_auto() {
            CssPixels::default()
        } else {
            self.specified_cross_min_size(index)
        };
        let clamp_max = if self.should_treat_max_size_as_none(node, true) {
            CssPixels::from_raw(i32::MAX)
        } else {
            self.specified_cross_max_size(index)
        };

        // If we have a definite cross size, this is easy! No need to perform layout, we can just use it as-is.
        if self.has_definite_cross_size(index) {
            self.flex_items[index].hypothetical_cross_size =
                css_clamp(self.inner_cross_size(index), clamp_min, clamp_max);
            return;
        }

        if let Some(ratio) = self.facts(node).preferred_aspect_ratio() {
            let facts = self.facts(node);
            // AD-HOC: For a replaced box whose only sizing information is a natural aspect ratio (e.g an SVG with a
            //         viewBox but no natural width or height) and an indefinite flex basis, we stretch-fit the cross size
            //         to the flex container instead of transferring the used main size through the aspect ratio.
            let replaced_with_only_natural_ratio =
                facts.is_replaced_box() && !(facts.has_auto_content_width() && facts.has_auto_content_height());
            if replaced_with_only_natural_ratio
                && !self.flex_items[index].used_flex_basis_is_definite
                && self.has_definite_cross_size_used(&self.container_used())
            {
                self.flex_items[index].hypothetical_cross_size =
                    css_clamp(self.inner_cross_size_used(&self.container_used()), clamp_min, clamp_max);
                return;
            }
            // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
            // When a box has a preferred aspect ratio, its automatic sizes are calculated the same as for a
            // replaced element with a natural aspect ratio and no natural size in that axis, see e.g. CSS2 § 10
            // and CSS Flexible Box Model Level 1 § 9.2. The axis in which the preferred size calculation depends
            // on this aspect ratio is called the ratio-dependent axis, and the resulting size is definite if its
            // input sizes are also definite.
            self.flex_items[index].hypothetical_cross_size = css_clamp(
                self.cross_size_from_main_size_and_aspect_ratio(self.flex_items[index].main_size.unwrap(), ratio),
                clamp_min,
                clamp_max,
            );
            self.flex_items[index].cross_size_was_resolved_from_aspect_ratio = self.has_definite_main_size(index);
            return;
        }

        let computed = self.computed_cross_size(node).0;
        if computed.is_min_content() {
            self.flex_items[index].hypothetical_cross_size =
                css_clamp(self.calculate_min_content_cross_size(index), clamp_min, clamp_max);
            return;
        }
        if computed.is_max_content() {
            self.flex_items[index].hypothetical_cross_size =
                css_clamp(self.calculate_max_content_cross_size(index), clamp_min, clamp_max);
            return;
        }

        // "... treating auto as fit-content"
        let fit_content = if !self.cross_axis_is_horizontal() {
            self.calculate_fit_content_size(
                index,
                SizingAxis::Block,
                AvailableSpace {
                    inline_size: self.flex_items[index]
                        .main_size
                        .map(|size| AvailableSize::definite(clamp_to_max_dimension_value(size)))
                        .unwrap_or(AvailableSize::Indefinite),
                    block_size: AvailableSize::Indefinite,
                },
            )
        } else {
            self.calculate_fit_content_size(index, SizingAxis::Inline, self.available_space_for_items.unwrap().space)
        };
        self.flex_items[index].hypothetical_cross_size = css_clamp(fit_content, clamp_min, clamp_max);
    }

    fn calculate_inner_container_cross_size(&self, property: SizingProperty) -> CssPixels {
        let axis = self.cross_sizing_axis();
        self.calculate_inner_size_with_constraints(
            self.flex_container,
            axis,
            property,
            self.available_space.unwrap(),
            self.layout_input.unwrap().containing_block_constraints,
        )
    }

    // https://www.w3.org/TR/css-flexbox-1/#algo-cross-line
    fn calculate_cross_size_of_each_flex_line(&mut self) {
        // If the flex container is single-line and has a definite cross size, the cross size of the flex line is the flex container’s inner cross size.
        if self.is_single_line() && self.has_definite_cross_size_used(&self.container_used()) {
            self.flex_lines[0].cross_size = self.inner_cross_size_used(&self.container_used());
            return;
        }
        // Otherwise, for each flex line:
        for line_index in 0..self.flex_lines.len() {
            // FIXME: 1. Collect all the flex items whose inline-axis is parallel to the main-axis, whose align-self is baseline,
            //           and whose cross-axis margins are both non-auto. Find the largest of the distances between each item’s baseline
            //           and its hypothetical outer cross-start edge, and the largest of the distances between each item’s baseline
            //           and its hypothetical outer cross-end edge, and sum these two values.

            // 2. Among all the items not collected by the previous step, find the largest outer hypothetical cross size.
            let largest = self.flex_lines[line_index]
                .items
                .iter()
                .copied()
                .map(|index| self.flex_items[index].hypothetical_cross_size_with_margins())
                .max()
                .unwrap_or_default();
            // 3. The used cross-size of the flex line is the largest of the numbers found in the previous two steps and zero.
            self.flex_lines[line_index].cross_size = largest.max(CssPixels::default());
        }
        // If the flex container is single-line, then clamp the line’s cross-size to be within the container’s computed min and max cross sizes.
        // Note that if CSS 2.1’s definition of min/max-width/height applied more generally, this behavior would fall out automatically.
        // AD-HOC: We don't do this when the flex container is being sized under a min-content or max-content constraint.
        if self.is_single_line()
            && !self
                .available_space_for_items
                .unwrap()
                .cross
                .is_intrinsic_sizing_constraint()
        {
            let min = self.computed_cross_min_size(self.flex_container).0;
            let cross_min = if min.is_auto() {
                CssPixels::default()
            } else {
                self.calculate_inner_container_cross_size(self.computed_cross_min_size(self.flex_container).1)
            };
            let cross_max = if self.should_treat_max_size_as_none(self.flex_container, true) {
                CssPixels::from_raw(i32::MAX)
            } else {
                self.calculate_inner_container_cross_size(self.computed_cross_max_size(self.flex_container).1)
            };
            self.flex_lines[0].cross_size = css_clamp(self.flex_lines[0].cross_size, cross_min, cross_max);
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#algo-line-stretch
    fn handle_align_content_stretch(&mut self) {
        // If the flex container has a definite cross size,
        if !self.has_definite_cross_size_used(&self.container_used())
            // align-content is stretch,
            || !matches!(
                self.style(self.flex_container).align_content(),
                align_content::STRETCH | align_content::NORMAL
            )
        {
            return;
        }
        let mut sum = self
            .flex_lines
            .iter()
            .fold(CssPixels::default(), |sum, line| sum + line.cross_size);
        // CSS-FLEXBOX-2: Account for gap between flex lines.
        sum += self.cross_gap() * self.flex_lines.len().wrapping_sub(1);
        let container_size = self.inner_cross_size_used(&self.container_used());
        // and the sum of the flex lines' cross sizes is less than the flex container’s inner cross size,
        if sum >= container_size {
            return;
        }
        // increase the cross size of each flex line by equal amounts
        // such that the sum of their cross sizes exactly equals the flex container’s inner cross size.
        let extra = (container_size - sum) / self.flex_lines.len();
        for line in &mut self.flex_lines {
            line.cross_size += extra;
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#algo-stretch
    fn determine_used_cross_size_of_each_flex_item(&mut self) {
        for line_index in 0..self.flex_lines.len() {
            for item_position in 0..self.flex_lines[line_index].items.len() {
                let index = self.flex_lines[line_index].items[item_position];
                // If a flex item’s cross size depends on the available space in the cross axis, recalculate its cross
                // size using the flex line’s cross size (rather than the flex container’s) as the available space.
                if self.flex_item_is_stretched(index) {
                    let item = &self.flex_items[index];
                    let unclamped = self.flex_lines[line_index].cross_size
                        - item.margins.cross_before
                        - item.margins.cross_after
                        - item.padding.cross_before
                        - item.padding.cross_after
                        - item.borders.cross_before
                        - item.borders.cross_after;
                    let node = item.box_;
                    let min = self.computed_cross_min_size(node).0;
                    // https://drafts.csswg.org/css-flexbox-1/#definite-sizes
                    // Once the cross size of a flex line has been determined, the cross sizes of items in auto-sized flex
                    // containers are also considered definite for the purpose of layout.
                    let cross_min = if min.is_auto() {
                        CssPixels::default()
                    } else {
                        self.specified_cross_min_size(index)
                    };
                    let cross_max = if self.should_treat_max_size_as_none(node, true) {
                        CssPixels::from_raw(i32::MAX)
                    } else {
                        self.specified_cross_max_size(index)
                    };
                    let cross_size = css_clamp(unclamped, cross_min, cross_max);
                    self.flex_items[index].cross_size = Some(cross_size);
                    // If the flex item’s cross size changed as a result, redo layout for its contents, treating this used
                    // size as its definite cross size so that percentage-sized children can be resolved.
                    self.set_cross_size(index, cross_size);
                    self.set_has_definite_cross_size(index);
                } else {
                    // Otherwise, the used cross size is the item’s hypothetical cross size.
                    let size = self.flex_items[index].hypothetical_cross_size;
                    self.flex_items[index].cross_size = Some(size);
                    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
                    // The axis in which the preferred size calculation depends on this aspect ratio is called the
                    // ratio-dependent axis, and the resulting size is definite if its input sizes are also definite.
                    if self.flex_items[index].cross_size_was_resolved_from_aspect_ratio {
                        self.set_cross_size(index, size);
                        self.set_has_definite_cross_size(index);
                    }
                }
            }
        }
    }

    fn set_main_axis_first_margin(&mut self, index: usize, margin: CssPixels) {
        self.flex_items[index].margins.main_before = margin;
        let used = self.item_used(index);
        self.select_main(&used.margin_left, &used.margin_top).set(margin);
    }

    fn set_main_axis_second_margin(&mut self, index: usize, margin: CssPixels) {
        self.flex_items[index].margins.main_after = margin;
        let used = self.item_used(index);
        self.select_main(&used.margin_right, &used.margin_bottom).set(margin);
    }

    // https://www.w3.org/TR/css-flexbox-1/#algo-main-align
    fn distribute_any_remaining_free_space(&mut self) {
        for line_index in 0..self.flex_lines.len() {
            // 12.1.
            let item_count = self.flex_lines[line_index].items.len();
            let mut used_main_space = CssPixels::default();
            let mut auto_margins = 0usize;
            for item_position in 0..item_count {
                let index = self.flex_lines[line_index].items[item_position];
                let item = &self.flex_items[index];
                used_main_space += item.main_size.unwrap();
                auto_margins += item.margins.main_before_is_auto as usize;
                auto_margins += item.margins.main_after_is_auto as usize;
                used_main_space += item.margins.main_before
                    + item.margins.main_after
                    + item.borders.main_before
                    + item.borders.main_after
                    + item.padding.main_before
                    + item.padding.main_after;
            }
            // CSS-FLEXBOX-2: Account for gap between flex items.
            used_main_space += self.main_gap() * item_count.wrapping_sub(1);

            let remaining = self.flex_lines[line_index].remaining_free_space;
            if remaining.is_some_and(|value| value > CssPixels::default()) && auto_margins > 0 {
                let size = remaining.unwrap() / auto_margins;
                for item_position in 0..item_count {
                    let index = self.flex_lines[line_index].items[item_position];
                    if self.flex_items[index].margins.main_before_is_auto {
                        self.set_main_axis_first_margin(index, size);
                    }
                    if self.flex_items[index].margins.main_after_is_auto {
                        self.set_main_axis_second_margin(index, size);
                    }
                }
            } else {
                for item_position in 0..item_count {
                    let index = self.flex_lines[line_index].items[item_position];
                    if self.flex_items[index].margins.main_before_is_auto {
                        self.set_main_axis_first_margin(index, CssPixels::default());
                    }
                    if self.flex_items[index].margins.main_after_is_auto {
                        self.set_main_axis_second_margin(index, CssPixels::default());
                    }
                }
            }

            // 12.2.
            let mut space_between_items = CssPixels::default();
            let mut initial_offset = CssPixels::default();
            let number_of_items = item_count;
            let justify = self.style(self.flex_container).justify_content();
            if auto_margins == 0 && number_of_items > 0 {
                match justify {
                    justify_content::START | justify_content::LEFT => {}
                    justify_content::STRETCH | justify_content::NORMAL | justify_content::FLEX_START => {
                        if self.is_direction_reverse() {
                            initial_offset = self.inner_main_size_used(&self.container_used());
                        }
                    }
                    justify_content::END => {
                        initial_offset = self.inner_main_size_used(&self.container_used());
                    }
                    justify_content::RIGHT => {
                        if self.is_row_layout() {
                            initial_offset = self.inner_main_size_used(&self.container_used());
                        }
                    }
                    justify_content::FLEX_END => {
                        if !self.is_direction_reverse() {
                            initial_offset = self.inner_main_size_used(&self.container_used());
                        }
                    }
                    justify_content::CENTER => {
                        initial_offset = (self.inner_main_size_used(&self.container_used()) - used_main_space) / 2;
                        if self.is_direction_reverse() {
                            initial_offset = self.inner_main_size_used(&self.container_used()) - initial_offset;
                        }
                    }
                    justify_content::SPACE_BETWEEN => {
                        if self.is_direction_reverse() {
                            initial_offset = self.inner_main_size_used(&self.container_used());
                        }
                        if let Some(free_space) = remaining
                            && number_of_items > 1
                        {
                            space_between_items = (free_space / (number_of_items - 1)).max(CssPixels::default());
                        }
                    }
                    justify_content::SPACE_AROUND => {
                        if let Some(free_space) = remaining {
                            space_between_items = (free_space / number_of_items).max(CssPixels::default());
                        }
                        initial_offset = if self.is_direction_reverse() {
                            self.inner_main_size_used(&self.container_used()) - space_between_items / 2
                        } else {
                            space_between_items / 2
                        };
                    }
                    justify_content::SPACE_EVENLY => {
                        if let Some(free_space) = remaining {
                            space_between_items = (free_space / (number_of_items + 1)).max(CssPixels::default());
                        }
                        initial_offset = if self.is_direction_reverse() {
                            self.inner_main_size_used(&self.container_used()) - space_between_items
                        } else {
                            space_between_items
                        };
                    }
                    _ => unreachable!("invalid justify-content"),
                }
            }

            // For reverse, we use FlexRegionRenderCursor::Right
            // to indicate the cursor offset is the end and render backwards
            // Otherwise the cursor offset is the 'start' of the region or initial offset
            let cursor_right = if auto_margins == 0 {
                match justify {
                    justify_content::START | justify_content::LEFT => false,
                    justify_content::NORMAL
                    | justify_content::FLEX_START
                    | justify_content::CENTER
                    | justify_content::SPACE_AROUND
                    | justify_content::SPACE_BETWEEN
                    | justify_content::SPACE_EVENLY
                    | justify_content::STRETCH => self.is_direction_reverse(),
                    justify_content::END => true,
                    justify_content::RIGHT => self.is_row_layout(),
                    justify_content::FLEX_END => !self.is_direction_reverse(),
                    _ => false,
                }
            } else {
                false
            };

            let direction_reverse = self.is_direction_reverse();
            let main_gap = self.main_gap();
            let mut cursor = initial_offset;
            for iteration_position in 0..item_count {
                let position = if cursor_right {
                    item_count - 1 - iteration_position
                } else {
                    iteration_position
                };
                let index = self.flex_lines[line_index].items[position];
                let item = &self.flex_items[index];
                // CSS-FLEXBOX-2: Account for gap between items.
                let mut amount = item.main_size.unwrap()
                    + item.margins.main_before
                    + item.borders.main_before
                    + item.padding.main_before
                    + item.margins.main_after
                    + item.borders.main_after
                    + item.padding.main_after
                    + space_between_items;
                if !direction_reverse && cursor_right {
                    if position < item_count - 1 {
                        amount += main_gap;
                    }
                } else {
                    amount += main_gap;
                }
                if direction_reverse && cursor_right {
                    self.flex_items[index].main_offset = cursor
                        - self.flex_items[index].main_size.unwrap()
                        - self.flex_items[index].margins.main_after
                        - self.flex_items[index].borders.main_after
                        - self.flex_items[index].padding.main_after;
                    cursor -= amount;
                } else if cursor_right {
                    cursor -= amount;
                    self.flex_items[index].main_offset = cursor
                        + self.flex_items[index].margins.main_before
                        + self.flex_items[index].borders.main_before
                        + self.flex_items[index].padding.main_before;
                } else {
                    self.flex_items[index].main_offset = cursor
                        + self.flex_items[index].margins.main_before
                        + self.flex_items[index].borders.main_before
                        + self.flex_items[index].padding.main_before;
                    cursor += amount;
                }
            }
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#algo-cross-margins
    fn resolve_cross_axis_auto_margins(&mut self) {
        for line_index in 0..self.flex_lines.len() {
            let line_size = self.flex_lines[line_index].cross_size;
            for item_position in 0..self.flex_lines[line_index].items.len() {
                let index = self.flex_lines[line_index].items[item_position];
                let item = &self.flex_items[index];
                //  If a flex item has auto cross-axis margins:
                if !item.margins.cross_before_is_auto && !item.margins.cross_after_is_auto {
                    continue;
                }
                let outer = item.cross_size.unwrap()
                    + item.padding.cross_before
                    + item.padding.cross_after
                    + item.borders.cross_before
                    + item.borders.cross_after
                    + item.margins.cross_before
                    + item.margins.cross_after;
                // If its outer cross size (treating those auto margins as zero) is less than the cross size of its flex line,
                // distribute the difference in those sizes equally to the auto margins.
                // NB: Auto cross-axis margins are stored as 0, so including them here treats them as zero while keeping
                //     non-auto cross-axis margins in the outer cross size.
                if outer < line_size {
                    let remainder = line_size - outer;
                    if item.margins.cross_before_is_auto && item.margins.cross_after_is_auto {
                        self.flex_items[index].margins.cross_before = remainder / 2;
                        self.flex_items[index].margins.cross_after = remainder / 2;
                    } else if item.margins.cross_before_is_auto {
                        self.flex_items[index].margins.cross_before = remainder;
                    } else {
                        self.flex_items[index].margins.cross_after = remainder;
                    }
                }
                // FIXME: Otherwise, if the block-start or inline-start margin (whichever is in the cross axis) is auto, set it to zero.
                //        Set the opposite margin so that the outer cross size of the item equals the cross size of its flex line.
            }
        }
    }

    fn align_all_flex_items_along_the_cross_axis(&mut self) {
        // FIXME: Take better care of margins
        let wrap_reverse = self.used_flex_wrap() == flex_wrap::WRAP_REVERSE;
        for line_index in 0..self.flex_lines.len() {
            let half_line = self.flex_lines[line_index].cross_size / 2;
            for item_position in 0..self.flex_lines[line_index].items.len() {
                let index = self.flex_lines[line_index].items[item_position];
                let alignment = self.alignment_for_item(self.flex_items[index].box_);
                let item = &self.flex_items[index];
                let offset = match alignment {
                    // https://drafts.csswg.org/css-flexbox/#flex-wrap-property
                    // When flex-wrap is wrap-reverse, the cross-start and cross-end directions are swapped.
                    align_items::NORMAL if wrap_reverse => {
                        half_line
                            - item.cross_size.unwrap()
                            - item.margins.cross_after
                            - item.borders.cross_after
                            - item.padding.cross_after
                    }
                    align_items::NORMAL => {
                        -half_line + item.margins.cross_before + item.borders.cross_before + item.padding.cross_before
                    }
                    align_items::BASELINE => {
                        // https://drafts.csswg.org/css-flexbox-1/#valdef-align-items-baseline
                        // NB: Baseline-aligned items are initially placed at the cross-start edge (like flex-start). Their
                        //     positions are adjusted after layout in resolve_baseline_aligned_items().
                        self.flex_lines[line_index].has_baseline_aligned_items = true;
                        -half_line + item.margins.cross_before + item.borders.cross_before + item.padding.cross_before
                    }
                    align_items::START | align_items::FLEX_START | align_items::SELF_START | align_items::STRETCH => {
                        // FIXME: 'start', 'flex-start' and 'self-start' have subtly different behavior.
                        //        The same goes for the end values.
                        -half_line + item.margins.cross_before + item.borders.cross_before + item.padding.cross_before
                    }
                    align_items::END | align_items::FLEX_END | align_items::SELF_END => {
                        half_line
                            - item.cross_size.unwrap()
                            - item.margins.cross_after
                            - item.borders.cross_after
                            - item.padding.cross_after
                    }
                    align_items::CENTER => {
                        // https://drafts.csswg.org/css-flexbox/#align-items-property
                        // The flex item’s margin box is centered in the cross axis within the line
                        (-item.cross_size.unwrap()
                            + item.margins.cross_before
                            + item.borders.cross_before
                            + item.padding.cross_before
                            - item.margins.cross_after
                            - item.borders.cross_after
                            - item.padding.cross_after)
                            / 2
                    }
                    _ => item.cross_offset,
                };
                self.flex_items[index].cross_offset = offset;
            }
        }
    }

    // https://drafts.csswg.org/css-flexbox-1/#algo-line-align
    fn align_all_flex_lines(&mut self) {
        // Align all flex lines per align-content.

        if self.flex_lines.is_empty() {
            return;
        }
        let reverse_cross_axis = self.cross_axis_is_reverse();
        let container_cross_size = self.inner_cross_size_used(&self.container_used());
        if self.is_single_line() {
            // https://drafts.csswg.org/css-flexbox-1/#flex-lines
            // 'align-content' does not apply to single-line flex containers, so place the line at cross-start.
            let center = self.flex_lines[0].cross_size / 2;
            for item_position in 0..self.flex_lines[0].items.len() {
                let index = self.flex_lines[0].items[item_position];
                self.flex_items[index].cross_offset += center;
            }
            return;
        }

        let mut sum = self
            .flex_lines
            .iter()
            .fold(CssPixels::default(), |sum, line| sum + line.cross_size);
        // CSS-FLEXBOX-2: Account for gap between flex lines.
        sum += self.cross_gap() * self.flex_lines.len().wrapping_sub(1);
        let mut start = CssPixels::default();
        let mut gap_size = CssPixels::default();
        let mut place_backwards = false;
        let mut iterate_backwards = false;
        match self.style(self.flex_container).align_content() {
            align_content::START => {
                iterate_backwards = reverse_cross_axis;
            }
            align_content::END => {
                start = container_cross_size;
                place_backwards = true;
                iterate_backwards = !reverse_cross_axis;
            }
            align_content::FLEX_START => {
                if reverse_cross_axis {
                    start = container_cross_size;
                    place_backwards = true;
                }
            }
            align_content::FLEX_END => {
                iterate_backwards = true;
                if !reverse_cross_axis {
                    start = container_cross_size;
                    place_backwards = true;
                }
            }
            align_content::CENTER => {
                iterate_backwards = reverse_cross_axis;
                start = container_cross_size / 2 - sum / 2;
            }
            align_content::SPACE_BETWEEN => {
                if reverse_cross_axis {
                    start = container_cross_size;
                    place_backwards = true;
                }
                let leftover = container_cross_size - sum;
                if leftover >= CssPixels::default() && self.flex_lines.len() > 1 {
                    gap_size = leftover / (self.flex_lines.len() - 1);
                }
            }
            align_content::SPACE_AROUND => {
                iterate_backwards = reverse_cross_axis;
                let leftover = container_cross_size - sum;
                if leftover < CssPixels::default() {
                    // If the leftover free-space is negative this value is identical to center.
                    start = container_cross_size / 2 - sum / 2;
                } else {
                    gap_size = leftover / self.flex_lines.len();
                    // The spacing between the first/last lines and the flex container edges is half the size of the spacing between flex lines.
                    start = gap_size / 2;
                }
            }
            align_content::SPACE_EVENLY => {
                iterate_backwards = reverse_cross_axis;
                let leftover = container_cross_size - sum;
                if leftover < CssPixels::default() {
                    // If the leftover free-space is negative this value is identical to center.
                    start = container_cross_size / 2 - sum / 2;
                } else {
                    gap_size = leftover / (self.flex_lines.len() + 1);
                    // The spacing between the first/last lines and the flex container edges is the size of the spacing between flex lines.
                    start = gap_size;
                }
            }
            align_content::NORMAL | align_content::STRETCH => {
                if reverse_cross_axis {
                    start = container_cross_size;
                    place_backwards = true;
                }
            }
            _ => unreachable!("invalid align-content"),
        }

        let cross_gap = self.cross_gap();
        for iteration_index in 0..self.flex_lines.len() {
            let line_index = if iterate_backwards {
                self.flex_lines.len() - 1 - iteration_index
            } else {
                iteration_index
            };
            let center = if place_backwards {
                start - self.flex_lines[line_index].cross_size / 2
            } else {
                start + self.flex_lines[line_index].cross_size / 2
            };
            for item_position in 0..self.flex_lines[line_index].items.len() {
                let index = self.flex_lines[line_index].items[item_position];
                self.flex_items[index].cross_offset += center;
            }
            // CSS-FLEXBOX-2: Account for gap between flex lines.
            let amount = self.flex_lines[line_index].cross_size + gap_size + cross_gap;
            if place_backwards {
                start -= amount;
            } else {
                start += amount;
            }
        }
    }

    fn item_box_baseline(&self, index: usize) -> CssPixels {
        let item = &self.flex_items[index];
        formatting_context::box_baseline_with_content_baselines(
            &self.callbacks,
            item.box_,
            &self.item_used(index),
            formatting_context::BaselineSet::First,
            item.content_baselines,
        )
    }

    // https://drafts.csswg.org/css-flexbox-1/#valdef-align-items-baseline
    fn resolve_baseline_aligned_items(&mut self) {
        // all participating flex items on the line are aligned such that their baselines align, and the item with the
        // largest distance between its baseline and its cross-start margin edge is placed flush against the cross-start
        // edge of the line.

        // NB: This runs after layout_inside() so that line boxes are available for baseline computation.
        for line_index in 0..self.flex_lines.len() {
            if !self.flex_lines[line_index].has_baseline_aligned_items {
                continue;
            }
            // FIXME: box_baseline() only understands horizontal-tb line box geometry, so baseline-aligning items with
            //        other writing modes would shift them by physically meaningless amounts. Skip them for now.
            let participates = |context: &Self, index: usize| {
                context.alignment_for_item(context.flex_items[index].box_) == align_items::BASELINE
                    && context.style(context.flex_items[index].box_).writing_mode() == writing_mode::HORIZONTAL_TB
            };
            let mut max_baseline = CssPixels::default();
            for index in self.flex_lines[line_index].items.iter().copied() {
                if participates(self, index) {
                    max_baseline = max_baseline.max(self.item_box_baseline(index));
                }
            }
            for item_position in 0..self.flex_lines[line_index].items.len() {
                let index = self.flex_lines[line_index].items[item_position];
                if participates(self, index) {
                    let baseline = self.item_box_baseline(index);
                    self.flex_items[index].cross_offset += max_baseline - baseline;
                }
            }
        }
    }

    fn copy_dimensions_from_flex_items_to_boxes(&mut self) {
        let reference = self.container_used().content_inline_size.get();
        for index in 0..self.flex_items.len() {
            let style = self.style(self.flex_items[index].box_);
            {
                let used = self.item_used(index);
                used.margin_left.set(style.margin_left().to_px(reference));
                used.margin_right.set(style.margin_right().to_px(reference));
                used.margin_top.set(style.margin_top().to_px(reference));
                used.margin_bottom.set(style.margin_bottom().to_px(reference));
                used.border_left.set(style.border_left_width());
                used.border_right.set(style.border_right_width());
                used.border_top.set(style.border_top_width());
                used.border_bottom.set(style.border_bottom_width());
            }
            self.set_main_size(index, self.flex_items[index].main_size.unwrap());
            self.set_cross_size(index, self.flex_items[index].cross_size.unwrap());
        }
    }

    fn layout_inside_item(&mut self, run: &FormattingContextRun<'pass>, index: usize) {
        let node = self.flex_items[index].box_;
        let mut input = LayoutInput {
            available_space: self
                .item_used(index)
                .available_inner_space_or_constraints_from(self.available_space_for_items.unwrap().space),
            containing_block_constraints: self.item_containing_block_constraints(),
            content_box_position_in_bfc_root: None,
            sizing: RootSizingDirectives::default(),
            participation: ParticipationInParentFormattingContext::Item,
        };
        // https://drafts.csswg.org/css-flexbox-1/#flex-items
        // In the case of flex items with display: table, the table wrapper box becomes the flex item,
        // so the align-self property applies to it.
        // The contents of any caption boxes contribute to the calculation of
        // the table wrapper box's min-content and max-content sizes.
        // However, like width and height, the flex longhands apply to the table box as follows:
        // the flex item’s final size is calculated
        // by performing layout as if the distance between
        // the table wrapper box's edges and the table box's content edges
        // were all part of the table box's border+padding area,
        // and the table box were the flex item.
        if self.facts(node).is_table_wrapper() && !self.cross_axis_is_horizontal() && self.flex_item_is_stretched(index)
        {
            let mut intrinsic_space = input.available_space;
            intrinsic_space.block_size = AvailableSize::Indefinite;
            let intrinsic_size = self.sizing().compute_table_box_block_size_inside_wrapper(
                node,
                intrinsic_space,
                input.containing_block_constraints,
            );
            let extra = (self.flex_items[index].cross_size.unwrap() - self.flex_items[index].hypothetical_cross_size)
                .max(CssPixels::default());
            input.sizing.forced_min_border_box_block_size = Some(intrinsic_size + extra);
        }

        self.flex_items[index].content_baselines =
            match formatting_context::layout_inside_child(run, None, None, node, LayoutMode::Normal, input, false) {
                ChildLayoutOutcome::Created(result) => result.baselines,
                ChildLayoutOutcome::ReenterCurrent => {
                    self.run(run, input);
                    self.item_used(index).content_baselines_from_cells()
                }
                ChildLayoutOutcome::Skipped => self.item_used(index).content_baselines_from_cells(),
            };

        let container_inline_size = self.container_used().content_inline_size.get();
        let container_block_size = self.container_used().content_block_size.get();
        abspos_engine::compute_inset_native(run, node, container_inline_size, container_block_size);
    }

    // https://drafts.csswg.org/css-flexbox-1/#abspos-items
    fn calculate_static_position_rect(&self, node: Node) -> abspos_inputs::StaticPositionRect {
        // The cross-axis edges of the static-position rectangle of an absolutely-positioned child
        // of a flex container are the content edges of the flex container.

        let cross_alignment = match self.alignment_for_item(node) {
            // FIXME: Implement this
            //  Fallthrough
            align_items::BASELINE | align_items::FLEX_START | align_items::STRETCH | align_items::NORMAL => {
                if self.cross_axis_is_reverse() {
                    StaticPositionAlignment::End
                } else {
                    StaticPositionAlignment::Start
                }
            }
            align_items::FLEX_END => {
                if self.cross_axis_is_reverse() {
                    StaticPositionAlignment::Start
                } else {
                    StaticPositionAlignment::End
                }
            }
            align_items::START | align_items::SELF_START => StaticPositionAlignment::Start,
            align_items::END | align_items::SELF_END => StaticPositionAlignment::End,
            align_items::CENTER => StaticPositionAlignment::Center,
            _ => StaticPositionAlignment::Start,
        };
        // The main-axis edges of the static-position rectangle are where the margin edges of the child
        // would be positioned if it were the sole flex item in the flex container,
        // assuming both the child and the flex container were fixed-size boxes of their used size.
        // (For this purpose, auto margins are treated as zero.

        let main_alignment = match self.style(self.flex_container).justify_content() {
            justify_content::START | justify_content::LEFT => StaticPositionAlignment::Start,
            justify_content::STRETCH
            | justify_content::NORMAL
            | justify_content::FLEX_START
            | justify_content::SPACE_BETWEEN => {
                if self.is_direction_reverse() {
                    StaticPositionAlignment::End
                } else {
                    StaticPositionAlignment::Start
                }
            }
            justify_content::END | justify_content::RIGHT => StaticPositionAlignment::End,
            justify_content::FLEX_END => {
                if self.is_direction_reverse() {
                    StaticPositionAlignment::Start
                } else {
                    StaticPositionAlignment::End
                }
            }
            justify_content::CENTER | justify_content::SPACE_AROUND | justify_content::SPACE_EVENLY => {
                StaticPositionAlignment::Center
            }
            _ => unreachable!("invalid justify-content"),
        };
        let main_size = self.inner_main_size_used(&self.container_used());
        let cross_size = self.inner_cross_size_used(&self.container_used());
        let (logical_inline_size, logical_block_size) = if self.is_row_layout() {
            (main_size, cross_size)
        } else {
            (cross_size, main_size)
        };
        let (inline_size, block_size) = geometry::to_physical(
            self.style(self.flex_container).writing_mode(),
            logical_inline_size,
            logical_block_size,
        );
        let (logical_inline_alignment, logical_block_alignment) = if self.is_row_layout() {
            (main_alignment, cross_alignment)
        } else {
            (cross_alignment, main_alignment)
        };
        let (inline_alignment, block_alignment) = geometry::to_physical(
            self.style(self.flex_container).writing_mode(),
            logical_inline_alignment,
            logical_block_alignment,
        );
        abspos_inputs::StaticPositionRect {
            rect: geometry::LogicalRect {
                offset: Default::default(),
                size: geometry::LogicalSize {
                    inline_size,
                    block_size,
                },
            },
            inline_alignment,
            block_alignment,
            // alignment_for_item() consulted the box's own align-self.
            alignment_derives_from_own_computed_values: true,
        }
    }

    fn axis_direction(is_horizontal: bool, is_reverse: bool) -> AxisDirection {
        match (is_horizontal, is_reverse) {
            (true, false) => AxisDirection::HorizontalLr,
            (true, true) => AxisDirection::HorizontalRl,
            (false, false) => AxisDirection::VerticalTb,
            (false, true) => AxisDirection::VerticalBt,
        }
    }

    fn save_flex_layout_data(&self) {
        let mut lines = Vec::with_capacity(self.flex_lines.len());
        for line in &self.flex_lines {
            let mut items = Vec::with_capacity(line.items.len());
            for index in line.items.iter().copied() {
                let item = &self.flex_items[index];
                let main_size = item.main_size.unwrap_or(item.target_main_size);
                let cross_size = item.cross_size.unwrap_or(item.hypothetical_cross_size);
                let (logical_inline_offset, logical_block_offset, logical_inline_size, logical_block_size) =
                    if self.is_row_layout() {
                        (item.main_offset, item.cross_offset, main_size, cross_size)
                    } else {
                        (item.cross_offset, item.main_offset, cross_size, main_size)
                    };
                let writing_mode = self.style(self.flex_container).writing_mode();
                let (x, y) = geometry::to_physical(writing_mode, logical_inline_offset, logical_block_offset);
                let (width, height) = geometry::to_physical(writing_mode, logical_inline_size, logical_block_size);
                let rect = formatting_context::FlexLayoutItemRect { x, y, width, height };
                let node = item.box_;
                let style = self.style(node);
                let main_size_property = self.select_main(style.width(), style.height());
                let main_min_size_property = self.select_main(style.min_width(), style.min_height());
                let main_max_size_property = self.select_main(style.max_width(), style.max_height());
                let node_id = unsafe { (self.callbacks.host.node_unique_id)(self.callbacks.shell(node)) };
                items.push(formatting_context::FlexLayoutItem {
                    node_id: (node_id >= 0).then_some(node_id),
                    rect,
                    main_base_size: item.flex_base_size,
                    main_delta_size: item.target_main_size - item.flex_base_size,
                    main_min_size: if self.has_main_min_size(node) {
                        self.specified_main_min_size(index)
                    } else {
                        self.automatic_minimum_size(index)
                    },
                    main_max_size: if self.has_main_max_size(node) {
                        self.specified_main_max_size(index)
                    } else {
                        item.target_main_size
                    },
                    cross_min_size: if self.has_cross_min_size(node) {
                        self.specified_cross_min_size(index)
                    } else {
                        CssPixels::default()
                    },
                    cross_max_size: if self.has_cross_max_size(node) {
                        self.specified_cross_max_size(index)
                    } else {
                        item.cross_size.unwrap_or(item.hypothetical_cross_size)
                    },
                    clamp_state: if item.is_min_violation {
                        formatting_context::FlexLayoutClampState::ClampedToMin
                    } else if item.is_max_violation {
                        formatting_context::FlexLayoutClampState::ClampedToMax
                    } else {
                        formatting_context::FlexLayoutClampState::Unclamped
                    },
                    flex_basis: if style.flex_basis_is_content() {
                        "content".to_string()
                    } else {
                        crate::css::serialize::serialize_computed_size(style.flex_basis())
                    },
                    main_size_property: crate::css::serialize::serialize_computed_size(main_size_property),
                    main_min_size_property: crate::css::serialize::serialize_computed_size(main_min_size_property),
                    main_max_size_property: crate::css::serialize::serialize_computed_size(main_max_size_property),
                    flex_grow: style.flex_grow(),
                    flex_shrink: self.flex_shrink_factor(node),
                });
            }
            let cross_start = line
                .items
                .iter()
                .copied()
                .map(|index| self.flex_items[index].cross_offset)
                .min()
                .unwrap_or_default();
            lines.push(formatting_context::FlexLayoutLine {
                growth_state: line.growth_state,
                cross_start,
                cross_size: line.cross_size,
                items,
            });
        }
        let style = self.style(self.flex_container);
        let data = formatting_context::FlexLayoutData {
            align_content: style.align_content(),
            align_items: style.align_items(),
            flex_direction: style.effective_flex_direction(),
            flex_wrap: style.flex_wrap(),
            justify_content: style.justify_content(),
            main_axis_direction: Self::axis_direction(self.main_axis_is_horizontal(), self.is_direction_reverse())
                as u8,
            cross_axis_direction: Self::axis_direction(self.cross_axis_is_horizontal(), self.cross_axis_is_reverse())
                as u8,
            lines,
        };
        self.container_used().rare_data_mut().flex_layout_data = Some(std::rc::Rc::new(data));
    }

    // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
    fn cross_size_transferred_from_definite_main_size(&self, index: usize) -> Option<CssPixels> {
        // When a box has a preferred aspect ratio, its automatic sizes are calculated the same as for a
        // replaced element with a natural aspect ratio and no natural size in that axis [...] The axis in
        // which the preferred size calculation depends on this aspect ratio is called the ratio-dependent
        // axis [...] The opposite axis (on which the ratio-dependent axis size depends) is the
        // ratio-determining axis.
        // NB: Only a transfer to an automatic inline cross size is performed here. When the cross axis is the
        //     block axis, the block layout performed by the content cross size calculations already applies
        //     the aspect ratio.
        let node = self.flex_items[index].box_;
        if !self.cross_axis_is_horizontal() || !self.has_definite_main_size(index) {
            return None;
        }
        let ratio = self.facts(node).preferred_aspect_ratio()?;
        let main_size = if self.has_definite_main_size_used(&self.container_used()) {
            self.flex_items[index]
                .main_size
                .unwrap_or(self.flex_items[index].flex_base_size)
        } else {
            self.flex_items[index].flex_base_size
        };
        Some(self.cross_size_from_main_size_and_aspect_ratio(main_size, ratio))
    }

    fn calculate_cross_content_contribution(
        &self,
        index: usize,
        resolve_percentage_min_max_sizes: bool,
        max_content: bool,
    ) -> CssPixels {
        let node = self.flex_items[index].box_;
        let cross_size_auto = self.should_treat_cross_size_as_auto(node);
        let mut size = if cross_size_auto {
            self.cross_size_transferred_from_definite_main_size(index)
                .unwrap_or_else(|| {
                    if max_content {
                        self.calculate_max_content_cross_size(index)
                    } else {
                        self.calculate_min_content_cross_size(index)
                    }
                })
        } else {
            let (value, property) = self.computed_cross_size(node);
            self.resolve_size_for_axis(index, self.cross_axis_is_horizontal(), value, property)
        };
        if cross_size_auto && self.facts(node).preferred_aspect_ratio().is_some() {
            size = self.adjust_cross_size_through_aspect_ratio(
                node,
                size,
                self.computed_main_min_size(node).0,
                self.computed_main_max_size(node).0,
            );
        }
        let min = self.computed_cross_min_size(node).0;
        let max = self.computed_cross_max_size(node).0;
        let clamp_min = if !min.is_auto() && (resolve_percentage_min_max_sizes || !min.contains_percentage()) {
            self.specified_cross_min_size(index)
        } else {
            CssPixels::default()
        };
        let clamp_max = if !self.should_treat_max_size_as_none(node, true)
            && (resolve_percentage_min_max_sizes || !max.contains_percentage())
        {
            self.specified_cross_max_size(index)
        } else {
            CssPixels::from_raw(i32::MAX)
        };
        self.flex_items[index].add_cross_margin_box_sizes(css_clamp(size, clamp_min, clamp_max))
    }

    fn specified_main_max_size_for_intrinsic_contribution(
        &self,
        index: usize,
        available_size: AvailableSize,
    ) -> CssPixels {
        let node = self.flex_items[index].box_;
        let max = self.computed_main_max_size(node).0;
        if self.should_treat_max_size_as_none(node, false) {
            return CssPixels::from_raw(i32::MAX);
        }
        if !max.contains_percentage() {
            return self.specified_main_max_size(index);
        }
        if available_size == AvailableSize::MinContent {
            if self.facts(node).is_replaced_box() {
                return CssPixels::default();
            }
            return CssPixels::from_raw(i32::MAX);
        }
        if available_size == AvailableSize::MaxContent {
            return CssPixels::from_raw(i32::MAX);
        }
        self.specified_main_max_size(index)
    }

    // https://drafts.csswg.org/css-flexbox-1/#intrinsic-item-contributions
    fn calculate_main_content_contribution(&self, index: usize, max_content: bool) -> CssPixels {
        let node = self.flex_items[index].box_;
        // The main-size max-content contribution of a flex item is
        // the larger of its outer max-content size and outer preferred size if that is not auto,
        // clamped by its min/max main size.

        // The main-size min-content contribution of a flex item is the larger of its outer min-content size and outer
        // preferred size if that is not auto, clamped by its min/max main size.
        let intrinsic = if max_content {
            self.calculate_max_content_main_size(index)
        } else {
            self.calculate_min_content_main_size(index)
        };
        let preferred = self.computed_main_size(node);
        let larger = if preferred.0.is_auto() {
            intrinsic
        } else {
            intrinsic.max(self.resolve_size_for_axis(index, self.main_axis_is_horizontal(), preferred.0, preferred.1))
        };
        let clamp_min = if self.has_main_min_size(node) {
            self.specified_main_min_size(index)
        } else {
            self.automatic_minimum_size(index)
        };
        let clamp_max = self.specified_main_max_size_for_intrinsic_contribution(
            index,
            if max_content {
                AvailableSize::MaxContent
            } else {
                AvailableSize::MinContent
            },
        );
        self.flex_items[index].add_main_margin_box_sizes(css_clamp(larger, clamp_min, clamp_max))
    }

    // https://drafts.csswg.org/css-flexbox-1/#intrinsic-sizes
    fn determine_intrinsic_size_of_flex_container(&mut self) {
        let available_space = self.available_space_for_items.unwrap();
        if available_space.main.is_intrinsic_sizing_constraint() {
            let flex_lines = self.flex_lines.clone();
            let size = self.calculate_intrinsic_main_size_of_flex_container();
            self.flex_lines = flex_lines;
            self.set_container_main_size(size);
        }
        if available_space.cross.is_intrinsic_sizing_constraint() {
            let size = self.calculate_intrinsic_cross_size_of_flex_container();
            self.set_container_cross_size(size);
        }

        self.resolve_own_auto_block_size_for_intrinsic_inline_measurement();
    }

    fn resolve_own_auto_block_size_for_intrinsic_inline_measurement(&mut self) {
        // Intrinsic inline measurement also caches an inline-level root's block size and baseline. Resolve its
        // indefinite block axis before alignment so those cached values are derived from the box's actual used size.
        let available_space = self.available_space_for_items.unwrap().space;
        if !available_space.inline_size.is_intrinsic_sizing_constraint()
            || available_space.block_size != AvailableSize::Indefinite
            || !self.facts(self.flex_container).display().is_inline_outside()
        {
            return;
        }

        let resolution_space = AvailableSpace {
            inline_size: AvailableSize::definite(self.container_used().content_inline_size.get()),
            block_size: AvailableSize::Indefinite,
        };
        let constraints = self.layout_input.unwrap().containing_block_constraints;
        if !self
            .sizing()
            .should_treat_block_size_as_auto(self.flex_container, resolution_space, constraints)
        {
            return;
        }

        let automatic_block_size = if self.main_axis_is_horizontal() {
            self.automatic_block_size_from_line_cross_sizes()
        } else {
            let flex_lines = self.flex_lines.clone();
            let size = self.calculate_intrinsic_main_size_of_flex_container();
            self.flex_lines = flex_lines;
            size
        };
        self.resolve_own_auto_block_size(automatic_block_size, resolution_space);
    }

    fn resolve_own_auto_block_size_from_max_content_main_size(&mut self) {
        let Some(resolution_space) = self.layout_input.unwrap().sizing.flex_self_block_size_resolution_space else {
            return;
        };
        if self.main_axis_is_horizontal() {
            return;
        }
        // https://drafts.csswg.org/css-flexbox-1/#algo-main-item
        // If the used flex basis is content or depends on its available space, and the flex container
        // is being sized under a min-content or max-content constraint (e.g. when performing automatic
        // table layout [CSS2]), size the item under that constraint.
        let real_space_for_items = self.available_space_for_items.unwrap().space;
        self.determine_available_space_for_items(AvailableSpace {
            inline_size: real_space_for_items.inline_size,
            block_size: AvailableSize::MaxContent,
        });
        // https://drafts.csswg.org/css-align-3/#gap-percent
        // In Flex Layout: Cyclic percentage sizes resolve against zero in all cases.
        self.container_used().set_content_block_size(CssPixels::default());
        for index in 0..self.flex_items.len() {
            self.determine_flex_base_size(index);
        }
        let max_content_main_size = self.calculate_intrinsic_main_size_of_flex_container();
        self.flex_lines.clear();
        self.determine_available_space_for_items(real_space_for_items);
        self.resolve_own_auto_block_size(max_content_main_size, resolution_space);
    }

    fn resolve_own_auto_block_size_from_line_cross_sizes(&mut self) {
        let Some(resolution_space) = self.layout_input.unwrap().sizing.flex_self_block_size_resolution_space else {
            return;
        };
        if self.cross_axis_is_horizontal() {
            return;
        }
        self.resolve_own_auto_block_size(self.automatic_block_size_from_line_cross_sizes(), resolution_space);
    }

    fn automatic_block_size_from_line_cross_sizes(&self) -> CssPixels {
        // https://drafts.csswg.org/css-align-3/#gap-percent
        // In Flex Layout: Cyclic percentage sizes resolve against zero in all cases.
        let style = self.style(self.flex_container);
        let gap = if self.is_row_layout() {
            style.row_gap()
        } else {
            style.column_gap()
        };
        let cross_gap_resolved_against_zero = gap.to_px(CssPixels::default());
        let mut line_cross_size_sum = self
            .flex_lines
            .iter()
            .fold(CssPixels::default(), |sum, line| sum + line.cross_size);
        line_cross_size_sum += cross_gap_resolved_against_zero * self.flex_lines.len().saturating_sub(1);
        line_cross_size_sum
    }

    fn resolve_own_auto_block_size(&self, automatic_block_size: CssPixels, resolution_space: AvailableSpace) {
        self.sizing().resolve_used_block_size_if_treated_as_auto(
            self.flex_container,
            resolution_space,
            self.layout_input.unwrap().containing_block_constraints,
            Some(automatic_block_size),
            || automatic_block_size,
        );
    }

    // https://drafts.csswg.org/css-flexbox-1/#intrinsic-main-sizes
    fn calculate_intrinsic_main_size_of_flex_container(&mut self) -> CssPixels {
        // The min-content main size of a single-line flex container is calculated identically to the max-content main size,
        // except that the flex items’ min-content contributions are used instead of their max-content contributions.
        // However, for a multi-line container, it is simply the largest min-content contribution of all the non-collapsed flex items in the flex container.
        if !self.is_single_line() && self.available_space_for_items.unwrap().main == AvailableSize::MinContent {
            return (0..self.flex_items.len())
                // FIXME: Skip collapsed flex items.
                .map(|index| self.calculate_main_content_contribution(index, false))
                .max()
                .unwrap_or_default();
        }

        // The max-content main size of a flex container is, fundamentally, the smallest size the flex container
        // can take such that when flex layout is run with that container size, each flex item ends up at least
        // as large as its max-content contribution, to the extent allowed by the items’ flexibility.
        // It is calculated, considering only non-collapsed flex items, by:

        // 1. For each flex item, subtract its outer flex base size from its max-content contribution size.
        //    If that result is positive, divide it by the item’s flex grow factor if the flex grow factor is ≥ 1,
        //    or multiply it by the flex grow factor if the flex grow factor is < 1; if the result is negative,
        //    divide it by the item’s scaled flex shrink factor (if dividing by zero, treat the result as negative infinity).
        //    This is the item’s desired flex fraction.
        let min_content = self.available_space_for_items.unwrap().main == AvailableSize::MinContent;
        for index in 0..self.flex_items.len() {
            let contribution = self.calculate_main_content_contribution(index, !min_content);
            let result = contribution - self.flex_items[index].outer_flex_base_size();
            let mut adjusted = result;
            let style = self.style(self.flex_items[index].box_);
            if result > CssPixels::default() {
                adjusted = if style.flex_grow() >= 1.0 {
                    result.scaled(1.0 / style.flex_grow())
                } else {
                    result.scaled(style.flex_grow())
                };
            } else if result < CssPixels::default() {
                adjusted = if self.flex_items[index].scaled_flex_shrink_factor == 0.0 {
                    CssPixels::from_raw(i32::MIN)
                } else {
                    result.scaled(1.0 / self.flex_items[index].scaled_flex_shrink_factor)
                };
            }
            self.flex_items[index].desired_flex_fraction = adjusted.to_double();
        }

        // 2. Place all flex items into lines of infinite length.
        self.flex_lines.clear();
        if !self.flex_items.is_empty() {
            // FIXME: Honor breaking requests.
            self.flex_lines.push(FlexLine {
                items: (0..self.flex_items.len()).collect(),
                ..Default::default()
            });
        }
        //    Within each line, find the greatest (most positive) desired flex fraction among all the flex items.
        //    This is the line’s chosen flex fraction.
        for line_index in 0..self.flex_lines.len() {
            let mut greatest = 0.0f32;
            let mut grow_sum = 0.0f32;
            let mut shrink_sum = 0.0f32;
            for index in self.flex_lines[line_index].items.iter().copied() {
                greatest = greatest.max(self.flex_items[index].desired_flex_fraction as f32);
                let node = self.flex_items[index].box_;
                grow_sum += self.style(node).flex_grow() as f32;
                shrink_sum += self.flex_shrink_factor(node) as f32;
            }
            let mut chosen = greatest;
            // 3. If the chosen flex fraction is positive, and the sum of the line’s flex grow factors is less than 1,
            //    divide the chosen flex fraction by that sum.
            if chosen > 0.0 && grow_sum < 1.0 {
                chosen /= grow_sum;
            }
            // If the chosen flex fraction is negative, and the sum of the line’s flex shrink factors is less than 1,
            // multiply the chosen flex fraction by that sum.
            if chosen < 0.0 && shrink_sum < 1.0 {
                chosen *= shrink_sum;
            }
            self.flex_lines[line_index].chosen_flex_fraction = chosen as f64;
        }

        let mut largest_sum = CssPixels::default();
        for line_index in 0..self.flex_lines.len() {
            // 4. Add each item’s flex base size to the product of its flex grow factor (scaled flex shrink factor, if shrinking)
            //    and the chosen flex fraction, then clamp that result by the max main size floored by the min main size.
            let mut sum = CssPixels::default();
            for index in self.flex_lines[line_index].items.iter().copied() {
                let desired = self.flex_items[index].desired_flex_fraction;
                let style = self.style(self.flex_items[index].box_);
                let product = if desired > 0.0 {
                    self.flex_lines[line_index].chosen_flex_fraction * style.flex_grow()
                } else if desired < 0.0 {
                    self.flex_lines[line_index].chosen_flex_fraction * self.flex_items[index].scaled_flex_shrink_factor
                } else {
                    0.0
                };
                let result = self.flex_items[index].flex_base_size + CssPixels::nearest_value_for(product);
                let min = self.computed_main_min_size(self.flex_items[index].box_).0;
                let clamp_min = if !min.is_auto() && !min.contains_percentage() {
                    self.specified_main_min_size(index)
                } else {
                    self.automatic_minimum_size(index)
                };
                let clamp_max = self.specified_main_max_size_for_intrinsic_contribution(
                    index,
                    self.available_space_for_items.unwrap().main,
                );
                // NOTE: The spec doesn't mention anything about the *outer* size here, but if we don't add the margin box,
                //       flex items with non-zero padding/border/margin in the main axis end up overflowing the container.
                sum += self.flex_items[index].add_main_margin_box_sizes(css_clamp(result, clamp_min, clamp_max));
            }
            // CSS-FLEXBOX-2: Account for gap between flex items.
            sum += self.main_gap() * self.flex_lines[line_index].items.len().wrapping_sub(1);
            largest_sum = largest_sum.max(sum);
        }
        // 5. The flex container’s max-content size is the largest sum (among all the lines) of the afore-calculated sizes of all items within a single line.
        self.set_container_main_size(largest_sum);
        largest_sum
    }

    // https://drafts.csswg.org/css-flexbox-1/#intrinsic-cross-sizes
    fn calculate_intrinsic_cross_size_of_flex_container(&mut self) -> CssPixels {
        let min_content = self.available_space_for_items.unwrap().cross == AvailableSize::MinContent;
        // The min-content/max-content cross size of a single-line flex container
        // is the largest min-content contribution/max-content contribution (respectively) of its flex items.
        if self.is_single_line() {
            let calculate_largest = |context: &Self, resolve_percentages: bool| {
                (0..context.flex_items.len())
                    .map(|index| context.calculate_cross_content_contribution(index, resolve_percentages, !min_content))
                    .max()
                    .unwrap_or_default()
            };
            let first = calculate_largest(self, false);
            self.set_container_cross_size(first);
            return calculate_largest(self, true);
        }
        // row multi-line flex container cross-size

        // The min-content/max-content cross size is the sum of the flex line cross sizes resulting from
        // sizing the flex container under a cross-axis min-content constraint/max-content constraint (respectively).

        // NOTE: We fall through to the ad-hoc section below.
        if !self.is_row_layout() && min_content {
            // column multi-line flex container cross-size

            // The min-content cross size is the largest min-content contribution among all of its flex items.
            let calculate_largest = |context: &Self, resolve_percentages: bool| {
                (0..context.flex_items.len())
                    .map(|index| context.calculate_cross_content_contribution(index, resolve_percentages, false))
                    .max()
                    .unwrap_or_default()
            };
            let first = calculate_largest(self, false);
            self.set_container_cross_size(first);
            return calculate_largest(self, true);
        }
        // The max-content cross size is the sum of the flex line cross sizes resulting from
        // sizing the flex container under a cross-axis max-content constraint,
        // using the largest max-content cross-size contribution among the flex items
        // as the available space in the cross axis for each of the flex items during layout.

        // NOTE: We fall through to the ad-hoc section below.
        let mut sum = self
            .flex_lines
            .iter()
            .fold(CssPixels::default(), |sum, line| sum + line.cross_size);
        // CSS-FLEXBOX-2: Account for gap between flex lines.
        sum += self.cross_gap() * self.flex_lines.len().wrapping_sub(1);
        sum
    }

    pub(super) fn run(&mut self, run: &FormattingContextRun<'pass>, layout_input: LayoutInput) {
        let available_space = layout_input.available_space;
        // This implements https://www.w3.org/TR/css-flexbox-1/#layout-algorithm

        // OPTIMIZATION: If we're in intrinsic sizing layout, but the flex container is not the
        //               box being measured, we can skip everything here.
        //               The parent formatting context has already figured out our size anyway.
        //               However, an inline-level container must still lay out its items, since the
        //               parent inline formatting context derives the fragment's baseline from them.
        if self.layout_mode == LayoutMode::IntrinsicSizing
            && !available_space.inline_size.is_intrinsic_sizing_constraint()
            && !available_space.block_size.is_intrinsic_sizing_constraint()
            && !self.facts(self.flex_container).display().is_inline_outside()
        {
            return;
        }

        self.available_space = Some(available_space);
        self.layout_input = Some(layout_input);
        self.item_percentage_bases =
            self.constraints_for_child_context(self.flex_container, layout_input.containing_block_constraints);
        // 1. Generate anonymous flex items
        self.generate_anonymous_flex_items();
        // 2. Determine the available main and cross space for the flex items
        self.determine_available_space_for_items(available_space);

        // https://drafts.csswg.org/css-flexbox-1/#definite-sizes
        // 3. If a single-line flex container has a definite cross size,
        //    the automatic preferred outer cross size of any stretched flex items is the flex container’s inner cross size
        //    (clamped to the flex item’s min and max cross size) and is considered definite.
        if self.is_single_line() && self.has_definite_cross_size_used(&self.container_used()) {
            let container_cross_size = self.inner_cross_size_used(&self.container_used());
            for index in 0..self.flex_items.len() {
                if !self.flex_item_is_stretched(index) {
                    continue;
                }
                let node = self.flex_items[index].box_;
                let min_size = if self.has_cross_min_size(node) {
                    self.specified_cross_min_size(index)
                } else {
                    CssPixels::default()
                };
                let max_size = if self.has_cross_max_size(node) {
                    self.specified_cross_max_size(index)
                } else {
                    CssPixels::from_raw(i32::MAX)
                };
                let outer = css_clamp(container_cross_size, min_size, max_size);
                let item = &self.flex_items[index];
                let inner = outer
                    - item.margins.cross_before
                    - item.margins.cross_after
                    - item.padding.cross_before
                    - item.padding.cross_after
                    - item.borders.cross_before
                    - item.borders.cross_after;
                self.set_cross_size(index, inner);
                self.set_has_definite_cross_size(index);
            }
        }

        // 4. Determine the main size of the flex container
        // Determine the main size of the flex container using the rules of the formatting context in which it participates.
        // NOTE: The automatic block size of a block-level flex container is its max-content size.
        self.resolve_own_auto_block_size_from_max_content_main_size();

        // 3. Determine the flex base size and hypothetical main size of each item
        for index in 0..self.flex_items.len() {
            self.determine_flex_base_size(index);
        }

        // OPTIMIZATION: We try to avoid calculating the "automatic minimum size" if possible,
        //               since it may require expensive intrinsic sizing layout.
        //               If this is a single-line flex container and the sum of flex bases sizes
        //               won't exceed the available space in the main axis, we know the layout
        //               algorithm won't have to shrink anything, thus not needing the minimum size.
        let should_skip_automatic_minimum_size_clamp = self.layout_mode != LayoutMode::IntrinsicSizing
            && self.is_single_line()
            && self.has_definite_main_size_used(&self.container_used())
            && self
                .flex_items
                .iter()
                .enumerate()
                .all(|(index, _)| self.has_definite_main_size(index))
            && self
                .flex_items
                .iter()
                .fold(CssPixels::default(), |sum, item| sum + item.flex_base_size)
                <= self.available_space_for_items.unwrap().main.to_px_or_zero();

        // OPTIMIZATION: Skip automatic_minimum_size calculation when we can prove it won't affect the result.
        //               We compute:
        //                   hypothetical_main_size = clamp(flex_base_size, automatic_minimum_size, max_size)
        //               For non-replaced elements without aspect ratio, the spec defines:
        //                   automatic_minimum_size = content_based_minimum_size
        //                   content_based_minimum_size = min(content_size_suggestion, specified_size_suggestion)
        //               This means:
        //                   automatic_minimum_size <= specified_size_suggestion (when specified exists).
        //.              So if flex_base_size == specified_size_suggestion, then automatic_minimum_size <= flex_base_size,
        //               and the lower clamp becomes a no-op. We can substitute 0 and get the same result.
        //.              We exclude: scroll containers (content can overflow, different behavior), replaced elements (use
        //               different formula with intrinsic sizes), and items with aspect-ratio (transferred_size_suggestion
        //               complicates the calculation).
        for index in 0..self.flex_items.len() {
            let node = self.flex_items[index].box_;
            let clamp_max = if self.has_main_max_size(node) {
                self.specified_main_max_size(index)
            } else {
                CssPixels::from_raw(i32::MAX)
            };
            let facts = self.facts(node);
            let can_skip_for_item = !facts.is_scroll_container()
                && !facts.is_replaced_box()
                && !facts.has_preferred_aspect_ratio()
                && self.flex_items[index].used_flex_basis_is_definite
                && self.specified_size_suggestion(index) == Some(self.flex_items[index].flex_base_size);
            let clamp_min = if self.has_main_min_size(node) {
                self.specified_main_min_size(index)
            } else if should_skip_automatic_minimum_size_clamp || can_skip_for_item {
                CssPixels::default()
            } else {
                self.automatic_minimum_size(index)
            };
            // The hypothetical main size is the item’s flex base size clamped according to its used min and max main sizes (and flooring the content box size at zero).
            self.flex_items[index].hypothetical_main_size =
                css_clamp(self.flex_items[index].flex_base_size, clamp_min, clamp_max).max(CssPixels::default());
        }

        // 5. Collect flex items into flex lines:
        // After this step no additional items are to be added to flex_lines or any of its items!
        self.collect_flex_items_into_flex_lines();
        // 6. Resolve the flexible lengths
        self.resolve_flexible_lengths();
        // Cross Size Determination
        // 7. Determine the hypothetical cross size of each item
        for index in 0..self.flex_items.len() {
            self.determine_hypothetical_cross_size_of_item(index);
        }
        // 8. Calculate the cross size of each flex line.
        self.calculate_cross_size_of_each_flex_line();
        // 9. Handle 'align-content: stretch'.
        self.handle_align_content_stretch();
        // 10. Collapse visibility:collapse items.
        // FIXME: This

        // 11. Determine the used cross size of each flex item.
        self.determine_used_cross_size_of_each_flex_item();
        let is_intrinsic_sizing = available_space.inline_size.is_intrinsic_sizing_constraint()
            || available_space.block_size.is_intrinsic_sizing_constraint();
        if is_intrinsic_sizing {
            // Alignment offsets depend on the container's used size, so intrinsic sizing must resolve that size before
            // the item-placement phases below.
            self.determine_intrinsic_size_of_flex_container();
        }
        // 12. Distribute any remaining free space.
        self.distribute_any_remaining_free_space();
        // 13. Resolve cross-axis auto margins.
        self.resolve_cross_axis_auto_margins();
        // 14. Align all flex items along the cross-axis
        self.align_all_flex_items_along_the_cross_axis();

        // 15. Determine the flex container’s used cross size
        // Determine the flex container's used cross size using the rules of the formatting context
        // in which it participates. If a content-based cross size is needed, use the sum of the
        // flex lines' cross sizes.
        self.resolve_own_auto_block_size_from_line_cross_sizes();

        // https://drafts.csswg.org/css-flexbox-1/#definite-sizes
        // 4. Once the cross size of a flex line has been determined,
        //    the cross sizes of items in auto-sized flex containers are also considered definite for the purpose of layout.
        if self.should_treat_cross_size_as_auto(self.flex_container) {
            for index in 0..self.flex_items.len() {
                let size = self.flex_items[index].cross_size.unwrap();
                self.set_cross_size(index, size);
                self.set_has_definite_cross_size(index);
            }
        }
        // 16. Align all flex lines (per align-content)
        self.align_all_flex_lines();

        if is_intrinsic_sizing {
            // We're computing intrinsic size for the flex container. This happens at the end of run().
            if self.facts(self.flex_container).display().is_inline_outside() {
                // The parent inline formatting context needs the container's content-derived baseline.
                self.layout_items_and_derive_baselines(run);
            }
        } else {
            self.layout_items_and_derive_baselines(run);
        }

        if self.should_collect_devtools_layout_data {
            self.save_flex_layout_data();
        }
    }

    // AD-HOC: Layout the inside of all flex items after resolving their dimensions.
    fn layout_items_and_derive_baselines(&mut self, run: &FormattingContextRun<'pass>) {
        self.copy_dimensions_from_flex_items_to_boxes();
        for index in 0..self.flex_items.len() {
            self.layout_inside_item(run, index);
        }
        self.resolve_baseline_aligned_items();
        for index in 0..self.flex_items.len() {
            let item = &self.flex_items[index];
            let offset = if self.main_axis_is_horizontal() {
                FfiCssPixelPoint {
                    x: item.main_offset,
                    y: item.cross_offset,
                }
            } else {
                FfiCssPixelPoint {
                    x: item.cross_offset,
                    y: item.main_offset,
                }
            };
            formatting_context::place_child(&self.formatting_context_run(), item.box_, offset, None);
        }
        self.derived_baselines_of_root_box =
            formatting_context::derive_baselines(self.records, &self.callbacks, self.flex_container, true);
    }

    pub(super) fn parent_did_dimension(&self) {
        if self.layout_mode != LayoutMode::Normal {
            return;
        }
        let mut child = self.callbacks.first_child(self.flex_container);
        while !child.is_invalid() {
            let next = self.callbacks.next_sibling(child);
            let facts = self.facts(child);
            if facts.is_box() && facts.is_absolutely_positioned() {
                formatting_context::register_contained_abspos_child(
                    &self.callbacks,
                    self.fragments.as_deref(),
                    self.flex_container,
                    child,
                    self.calculate_static_position_rect(child),
                    None,
                );
            }
            child = next;
        }
    }

    pub(super) fn derived_baselines_of_root_box(&self) -> DerivedBaselines {
        self.derived_baselines_of_root_box
    }

    pub(super) fn automatic_content_inline_size(&self) -> CssPixels {
        self.container_used().content_inline_size.get()
    }

    pub(super) fn automatic_content_block_size(&self) -> CssPixels {
        self.container_used().content_block_size.get()
    }
}

fn abs(value: CssPixels) -> CssPixels {
    if value < CssPixels::default() { -value } else { value }
}

pub(crate) fn order_modified_keys<T>(buckets: &HashMap<i32, Vec<T>>, reverse: bool) -> Vec<i32> {
    let mut keys: Vec<_> = buckets.keys().copied().collect();
    if reverse {
        keys.sort_unstable_by(|left, right| right.cmp(left));
    } else {
        keys.sort_unstable();
    }
    keys
}
