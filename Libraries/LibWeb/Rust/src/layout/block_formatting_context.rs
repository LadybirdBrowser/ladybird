/*
 * Copyright (c) 2020-2022, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2025-2026, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct BlockCssPixelRect {
    x: CssPixels,
    y: CssPixels,
    width: CssPixels,
    height: CssPixels,
}

impl BlockCssPixelRect {
    fn right(self) -> CssPixels {
        self.x + self.width
    }

    fn bottom(self) -> CssPixels {
        self.y + self.height
    }

    fn translated(self, x: CssPixels, y: CssPixels) -> Self {
        Self {
            x: self.x + x,
            y: self.y + y,
            ..self
        }
    }

    fn intersects(self, other: Self) -> bool {
        self.width > CssPixels::default()
            && self.height > CssPixels::default()
            && other.width > CssPixels::default()
            && other.height > CssPixels::default()
            && self.x < other.right()
            && self.right() > other.x
            && self.y < other.bottom()
            && self.bottom() > other.y
    }
}

impl From<BlockCssPixelRect> for FfiCssPixelRect {
    fn from(rect: BlockCssPixelRect) -> Self {
        Self {
            x: rect.x,
            y: rect.y,
            width: rect.width,
            height: rect.height,
        }
    }
}

fn round_css_pixels(value: CssPixels) -> CssPixels {
    let half = CssPixels::from_raw(crate::layout::FIXED_POINT_DENOMINATOR >> 1);
    if value > CssPixels::default() {
        (value + half).floor()
    } else {
        (value - half).ceil()
    }
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct PendingTopMarginGroup {
    pub(crate) box_: Node,
    pub(crate) collapsed_margin_at_open: CssPixels,
    pub(crate) collapsed_margin: CssPixels,
    pub(crate) open: bool,
    pub(crate) pinned_by_clearance: bool,
}

#[derive(Debug, Default)]
pub(crate) struct BlockMarginState {
    current_positive_collapsible_margin: CssPixels,
    current_negative_collapsible_margin: CssPixels,
    pending_top_margin_groups: Vec<PendingTopMarginGroup>,
    box_last_in_flow_child_margin_bottom_collapsed: bool,
}

impl BlockMarginState {
    pub(crate) fn add_margin(&mut self, margin: CssPixels) {
        if margin < CssPixels::default() {
            self.current_negative_collapsible_margin = margin.min(self.current_negative_collapsible_margin);
        } else {
            self.current_positive_collapsible_margin = margin.max(self.current_positive_collapsible_margin);
        }
    }

    pub(crate) fn current_collapsed_margin(&self) -> CssPixels {
        self.current_positive_collapsible_margin + self.current_negative_collapsible_margin
    }

    // Several ancestor groups may await placement, but only the last can remain open
    // and continue accumulating collapsed margins.
    pub(crate) fn open_top_margin_group(&mut self, box_: Node, pinned_by_clearance: bool) {
        let collapsed = self.current_collapsed_margin();
        self.pending_top_margin_groups.push(PendingTopMarginGroup {
            box_,
            collapsed_margin_at_open: collapsed,
            collapsed_margin: collapsed,
            open: true,
            pinned_by_clearance,
        });
    }

    pub(crate) fn take_pending_top_margin(&mut self) -> CssPixels {
        self.pending_top_margin_groups
            .pop()
            .expect("an open top-margin group exists")
            .collapsed_margin
    }

    pub(crate) fn has_open_top_margin_group(&self) -> bool {
        self.pending_top_margin_groups.last().is_some_and(|group| group.open)
    }

    pub(crate) fn update_open_top_margin_group(&mut self) {
        if self.has_open_top_margin_group() {
            let collapsed = self.current_collapsed_margin();
            self.pending_top_margin_groups.last_mut().unwrap().collapsed_margin = collapsed;
        }
    }

    pub(crate) fn pending_top_margin_groups(&self) -> &[PendingTopMarginGroup] {
        &self.pending_top_margin_groups
    }

    pub(crate) fn reset(&mut self) {
        if self.has_open_top_margin_group() {
            self.pending_top_margin_groups.last_mut().unwrap().open = false;
        }
        self.current_negative_collapsible_margin = CssPixels::default();
        self.current_positive_collapsible_margin = CssPixels::default();
    }
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum FloatSide {
    Left,
    Right,
}

#[derive(Clone, Copy)]
struct FloatingBox<'pass> {
    box_: Node,
    used_values: &'pass UsedValues,
    side: FloatSide,

    // Offset from left/right edge to the left content edge of `box`.
    offset_from_edge: CssPixels,

    // Top margin edge of `box`.
    top_margin_edge: CssPixels,

    // Bottom margin edge of `box`.
    bottom_margin_edge: CssPixels,

    margin_box_rect_in_root_coordinate_space: BlockCssPixelRect,
    containing_block_rect_in_root_coordinate_space: BlockCssPixelRect,
    percentage_basis_inline_size: Option<CssPixels>,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct FloatBand {
    pub(crate) block_start: CssPixels,
    pub(crate) left_intrusion: CssPixels,
    pub(crate) right_intrusion: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
struct FloatPlacement {
    block_start: CssPixels,
    offset_from_edge: CssPixels,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct CaptionLayoutResult {
    pub(crate) margin_box_block_size: CssPixels,
    pub(crate) pending_table_block_offset: CssPixels,
}

// https://www.w3.org/TR/css-display/#block-formatting-context
pub(crate) struct BlockFormattingContext<'pass> {
    state: &'pass LayoutState,
    root: Node,
    layout_mode: LayoutMode,
    callbacks: FfiLayoutFcCallbacks,
    block_offset_of_current_block_container: Cell<Option<CssPixels>>,
    pending_legend_flow_position: Cell<Option<LogicalOffset>>,
    pending_table_box_content_offset_in_wrapper: Cell<Option<LogicalOffset>>,
    margin_state: RefCell<BlockMarginState>,
    floats: RefCell<Vec<FloatingBox<'pass>>>,
    bands: RefCell<Vec<FloatBand>>,
    lowest_left_margin_edge: Cell<CssPixels>,
    lowest_right_margin_edge: Cell<CssPixels>,
    was_notified_after_parent_dimensioned_my_root_box: Cell<bool>,
}

impl<'pass> BlockFormattingContext<'pass> {
    pub(crate) fn new(state: &'pass LayoutState, root: Node, layout_mode: LayoutMode, callbacks: FfiLayoutFcCallbacks) -> Self {
        Self {
            state,
            root,
            layout_mode,
            callbacks,
            block_offset_of_current_block_container: Cell::new(None),
            pending_legend_flow_position: Cell::new(None),
            pending_table_box_content_offset_in_wrapper: Cell::new(None),
            margin_state: RefCell::new(BlockMarginState::default()),
            floats: RefCell::new(Vec::new()),
            bands: RefCell::new(vec![FloatBand::default()]),
            lowest_left_margin_edge: Cell::new(CssPixels::default()),
            lowest_right_margin_edge: Cell::new(CssPixels::default()),
            was_notified_after_parent_dimensioned_my_root_box: Cell::new(false),
        }
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        self.state.node_facts(&self.callbacks, node)
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        self.state.style_facts(&self.callbacks, node)
    }

    fn used_pointer(&self, node: Node) -> &'pass UsedValues {
        self.state.used_values(&self.callbacks, node)
    }

    fn try_used_pointer(&self, node: Node) -> Option<&'pass UsedValues> {
        self.state.try_used_values(&self.callbacks, node)
    }

    fn used(&self, node: Node) -> &'pass UsedValues {
        self.used_pointer(node)
    }

    fn used_mut(&self, node: Node) -> &'pass UsedValues {
        self.used_pointer(node)
    }

    fn create_used_values(&self, node: Node, constraints: ContainingBlockConstraints) -> &'pass UsedValues {
        self.state.create_used_values(&self.callbacks, node, constraints)
    }

    fn first_child(&self, node: Node) -> Node {
        self.callbacks.first_child(node)
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.callbacks.next_sibling(node)
    }

    fn containing_block(&self, node: Node) -> Node {
        self.callbacks.containing_block(node)
    }

    fn children(&self, node: Node) -> Vec<Node> {
        let mut children = Vec::new();
        let mut child = self.first_child(node);
        while !child.is_invalid() {
            if self.facts(child).is_box() {
                children.push(child);
            }
            child = self.next_sibling(child);
        }
        children
    }

    fn is_ancestor_of(&self, ancestor: Node, node: Node) -> bool {
        self.callbacks.is_ancestor(ancestor, node)
    }

    fn is_inclusive_ancestor_of(&self, ancestor: Node, node: Node) -> bool {
        ancestor == node || self.is_ancestor_of(ancestor, node)
    }

    fn sizing(&self) -> SizingContext<'_> {
        SizingContext::new(self.state, self.callbacks)
    }

    fn place_child(&self, node: Node, offset: FfiCssPixelPoint) {
        crate::layout::place_child(self.state, &self.callbacks, node, offset);
    }

    fn register_contained_abspos_child(&self, node: Node, block_offset: CssPixels) {
        let static_position = StaticPositionRect {
            rect: LogicalRect {
                offset: LogicalOffset {
                    inline_offset: CssPixels::default(),
                    block_offset,
                },
                size: LogicalSize::default(),
            },
            inline_alignment: StaticPositionAlignment::Start,
            block_alignment: StaticPositionAlignment::Start,
            alignment_derives_from_own_computed_values: false,
        };
        crate::layout::register_contained_abspos_child(self.state, &self.callbacks, node, static_position);
    }

    fn compute_and_store_baselines(&self, node: Node) {
        crate::layout::compute_and_store_baselines(self.state, &self.callbacks, node, false);
    }

    fn compute_inset(&self, node: Node, containing_block_size: LogicalSize) {
        crate::layout::compute_inset_native(
            self.state,
            self.callbacks,
            node,
            containing_block_size.inline_size,
            containing_block_size.block_size,
        );
    }

    fn containing_block_rect(&self, node: Node, position: FfiCssPixelPoint) -> BlockCssPixelRect {
        let used = self.used(node);
        BlockCssPixelRect {
            x: position.x,
            y: position.y,
            width: used.content_inline_size.get(),
            height: used.content_block_size.get(),
        }
    }

    fn margin_box_rect(used: &UsedValues) -> BlockCssPixelRect {
        let left = (used.margin_left.get() + used.border_box_left(false)).max(CssPixels::default());
        let right = (used.margin_right.get() + used.border_box_right(false)).max(CssPixels::default());
        let top = used.margin_box_top(false).max(CssPixels::default());
        let bottom = used.margin_box_bottom(false).max(CssPixels::default());
        BlockCssPixelRect {
            x: -left,
            y: -top,
            width: left + used.content_inline_size.get() + right,
            height: top + used.content_block_size.get() + bottom,
        }
    }

    fn resolve_vertical_box_model_metrics(&self, node: Node, containing_block_inline_size: CssPixels) {
        let style = self.style(node);
        let used = self.used_mut(node);
        used.margin_top
            .set(style.margin_top().to_px(containing_block_inline_size));
        used.margin_bottom
            .set(style.margin_bottom().to_px(containing_block_inline_size));
        used.border_top.set(style.border_top_width());
        used.border_bottom.set(style.border_bottom_width());
        used.padding_top
            .set(style.padding_top().to_px(containing_block_inline_size));
        used.padding_bottom
            .set(style.padding_bottom().to_px(containing_block_inline_size));
    }

    fn box_should_avoid_floats_because_it_establishes_fc(&self, node: Node) -> bool {
        // https://drafts.csswg.org/css2/#floats
        // The border box of a table, a block-level replaced element, or an element in the normal flow that establishes
        // a new block formatting context (such as an element with 'overflow' other than 'visible') must not overlap the
        // margin box of any floats in the same block formatting context as the element itself. If necessary,
        // implementations should clear the said element by placing it below any preceding floats, but may place it
        // adjacent to such floats if there is sufficient space. They may even make the border box of said element
        // narrower than defined by section 10.3.3. CSS2 does not define when a UA may put said element next to the
        // float or by how much said element may become narrower.

        // https://drafts.csswg.org/css-flexbox/#flex-containers
        // A flex container establishes a new flex formatting context for its contents. This is the same as establishing
        // a block formatting context, except that flex layout is used instead of block layout. For example, floats do
        // not intrude into the flex container, and the flex container’s margins do not collapse with the margins of its
        // contents.

        // https://drafts.csswg.org/css-grid/#grid-containers
        // A grid container that is not a subgrid establishes an independent grid formatting context for its contents.
        // This is the same as establishing an independent block formatting context, except that grid layout is used
        // instead of block layout: floats do not intrude into the grid container, and the grid container’s margins do
        // not collapse with the margins of its contents.
        matches!(
            formatting_context_type_created_by_box(self.facts(node)),
            Some(FfiFormattingContextType::Block | FfiFormattingContextType::Flex | FfiFormattingContextType::Grid)
        )
    }

    fn compute_inline_size(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        content_position_in_root: FfiCssPixelPoint,
    ) {
        let mut remaining_available_space = available_space;
        let facts = self.facts(node);
        let style = self.style(node);

        // Certain formatting contexts do not allow float intrusions, so reduce the available space for them.
        if matches!(available_space.inline_size, AvailableSize::Definite(_)) && self.box_should_avoid_floats_because_it_establishes_fc(node) {
            let available_inline_size = available_space.inline_size.to_px_or_zero();
            let box_in_root_rect = BlockCssPixelRect {
                x: content_position_in_root.x,
                y: content_position_in_root.y,
                width: available_inline_size,
                height: self.used(node).content_block_size.get(),
            };
            let intrusion = self.intrusions_for_band_into_rect(self.band_at(box_in_root_rect.y), box_in_root_rect);
            let mut remaining_inline_size = available_inline_size - intrusion.left - intrusion.right;
            if intrusion.left > CssPixels::default() || intrusion.right > CssPixels::default() {
                // Negative margins do not create additional space next to a float. Reduce the space available for
                // resolving an automatic inline size by any negative margins, so that the resulting border box is no
                // larger than the space next to the float in the inline axis.
                let margin_left = style.margin_left().to_px(available_inline_size);
                let margin_right = style.margin_right().to_px(available_inline_size);
                let negative_margin_sum =
                    margin_left.min(CssPixels::default()) + margin_right.min(CssPixels::default());
                remaining_inline_size = (remaining_inline_size + negative_margin_sum).max(CssPixels::default());
            }
            remaining_available_space.inline_size = AvailableSize::definite(remaining_inline_size);
        }

        let sizing = self.sizing();
        let sized_as_replaced = sizing.box_is_sized_as_replaced_element(node, available_space, constraints);
        if sized_as_replaced {
            self.compute_inline_size_for_block_level_replaced_element_in_normal_flow(
                node,
                available_space,
                constraints,
            );
            if facts.is_floating() {
                // 10.3.6 Floating, replaced elements:
                // https://www.w3.org/TR/CSS22/visudet.html#float-replaced-width
                return;
            }
        }

        if facts.is_floating() {
            // 10.3.5 Floating, non-replaced elements:
            // https://www.w3.org/TR/CSS22/visudet.html#float-width
            self.compute_inline_size_for_floating_box(node, available_space, constraints);
            return;
        }

        let available_inline_size = available_space.inline_size.to_px_or_zero();
        let mut margin_left_is_auto = style.margin_left().is_auto();
        let mut margin_right_is_auto = style.margin_right().is_auto();
        let mut margin_left = style.margin_left().to_px(available_inline_size);
        let mut margin_right = style.margin_right().to_px(available_inline_size);
        let padding_left = style.padding_left().to_px(available_inline_size);
        let padding_right = style.padding_right().to_px(available_inline_size);
        let border_left_width = style.border_left_width();
        let border_right_width = style.border_right_width();
        {
            let used = self.used_mut(node);
            used.margin_left.set(margin_left);
            used.margin_right.set(margin_right);
            used.border_left.set(border_left_width);
            used.border_right.set(border_right_width);
            used.padding_left.set(padding_left);
            used.padding_right.set(padding_right);
        }

        // NOTE: If we are calculating the min-content or max-content inline size of this box,
        //       and the inline size should be treated as auto, then we can simply return here,
        //       as the preferred inline size and min/max constraints are irrelevant for intrinsic sizing.
        if self.used(node).inline_size_constraint.get() != SizeConstraint::None {
            return;
        }

        let remaining_inline_size = remaining_available_space.inline_size.to_px_or_zero();
        let computed_margin_left = style.margin_left();
        let computed_margin_right = style.margin_right();
        let compute = |input: Option<CssPixels>,
                       margin_left: &mut CssPixels,
                       margin_right: &mut CssPixels,
                       margin_left_is_auto: &mut bool,
                       margin_right_is_auto: &mut bool|
         -> Option<CssPixels> {
            *margin_left = computed_margin_left.to_px(available_inline_size);
            *margin_right = computed_margin_right.to_px(available_inline_size);
            *margin_left_is_auto = computed_margin_left.is_auto();
            *margin_right_is_auto = computed_margin_right.is_auto();
            let mut inline_size = input;
            let mut total = border_left_width
                + border_right_width
                + *margin_left
                + padding_left
                + inline_size.unwrap_or_default()
                + padding_right
                + *margin_right;

            if !facts.is_inline() {
                // 10.3.3 Block-level, non-replaced elements in normal flow
                // If 'width' is not 'auto' and 'border-left-width' + 'padding-left' + 'width' + 'padding-right' +
                // 'border-right-width' (plus any of 'margin-left' or 'margin-right' that are not 'auto') is larger than the
                // width of the containing block, then any 'auto' values for 'margin-left' or 'margin-right' are, for the
                // following rules, treated as zero.
                if inline_size.is_some() && total > remaining_inline_size {
                    if *margin_left_is_auto {
                        *margin_left = CssPixels::default();
                        *margin_left_is_auto = false;
                    }
                    if *margin_right_is_auto {
                        *margin_right = CssPixels::default();
                        *margin_right_is_auto = false;
                    }
                    total = border_left_width
                        + border_right_width
                        + *margin_left
                        + padding_left
                        + inline_size.unwrap_or_default()
                        + padding_right
                        + *margin_right;
                }

                // 10.3.3 cont'd.
                let mut underflow = remaining_inline_size - total;
                if available_space.inline_size.is_intrinsic_sizing_constraint() {
                    underflow = CssPixels::default();
                }
                if inline_size.is_none() {
                    if *margin_left_is_auto {
                        *margin_left = CssPixels::default();
                        *margin_left_is_auto = false;
                    }
                    if *margin_right_is_auto {
                        *margin_right = CssPixels::default();
                        *margin_right_is_auto = false;
                    }
                    if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                        inline_size = Some(underflow.max(CssPixels::default()));
                    } else if available_space.inline_size == AvailableSize::MinContent {
                        if formatting_context_type_created_by_box(facts).is_some() {
                            inline_size = Some(sizing.calculate_min_content_inline_size(node, constraints));
                        }
                    } else if available_space.inline_size == AvailableSize::MaxContent {
                        if formatting_context_type_created_by_box(facts).is_some() {
                            inline_size = Some(sizing.calculate_max_content_inline_size(node, constraints));
                        }
                    } else {
                        unreachable!();
                    }
                } else if !*margin_left_is_auto && !*margin_right_is_auto {
                    *margin_right += underflow;
                } else if !*margin_left_is_auto && *margin_right_is_auto {
                    *margin_right = underflow;
                    *margin_right_is_auto = false;
                } else if *margin_left_is_auto && !*margin_right_is_auto {
                    *margin_left = underflow;
                    *margin_left_is_auto = false;
                } else {
                    let half = underflow / 2;
                    *margin_left = half;
                    *margin_right = half;
                    *margin_left_is_auto = false;
                    *margin_right_is_auto = false;
                }
            }
            inline_size
        };

        let input_inline_size = if sized_as_replaced {
            // NOTE: Replaced elements had their inline size calculated independently above.
            //       We use that inline size as the input here to ensure that margins get resolved.
            Some(self.used(node).content_inline_size.get())
        } else if facts.is_table_wrapper() {
            Some(sizing.compute_table_box_inline_size_inside_wrapper(
                node,
                remaining_available_space,
                constraints,
                None,
                crate::layout::TableWrapperInlineSizeMode::ClampToAvailableInlineSize,
            ))
        } else if facts.uses_button_layout() && style.width().is_auto() {
            // https://html.spec.whatwg.org/multipage/rendering.html#button-layout
            // If the computed value of 'inline-size' is 'auto', then the used value is the fit-content inline size.
            Some(sizing.calculate_fit_content_size(node, crate::layout::SizingAxis::Inline, available_space, constraints))
        } else if sizing.should_treat_inline_size_as_auto(node, available_space) {
            None
        } else {
            Some(sizing.calculate_inner_inline_size(node, available_space.inline_size, style.width(), constraints))
        };

        // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
        let mut used_inline_size = compute(
            input_inline_size,
            &mut margin_left,
            &mut margin_right,
            &mut margin_left_is_auto,
            &mut margin_right_is_auto,
        );

        // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
        //    but this time using the computed value of 'max-width' as the computed value for 'width'.
        if !sizing.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let max_inline_size =
                sizing.calculate_inner_inline_size(node, available_space.inline_size, style.max_width(), constraints);
            if used_inline_size.unwrap_or_default() > max_inline_size {
                used_inline_size = compute(
                    Some(max_inline_size),
                    &mut margin_left,
                    &mut margin_right,
                    &mut margin_left_is_auto,
                    &mut margin_right_is_auto,
                );
            }
        }

        // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
        //    but this time using the value of 'min-width' as the computed value for 'width'.
        let min_width = style.min_width();
        if !min_width.is_auto()
            && let Some(value) = used_inline_size
        {
            let min_inline_size =
                sizing.calculate_inner_inline_size(node, available_space.inline_size, min_width, constraints);
            if value < min_inline_size {
                used_inline_size = compute(
                    Some(min_inline_size),
                    &mut margin_left,
                    &mut margin_right,
                    &mut margin_left_is_auto,
                    &mut margin_right_is_auto,
                );
            }
        }

        if !sized_as_replaced && let Some(value) = used_inline_size {
            self.used_mut(node).set_content_inline_size(value);
        }
        let used = self.used_mut(node);
        used.margin_left.set(margin_left);
        used.margin_right.set(margin_right);
    }

    fn compute_inline_size_for_floating_box(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) {
        // 10.3.5 Floating, non-replaced elements
        let style = self.style(node);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();

        // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
        let margin_left = style.margin_left().to_px(containing_block_inline_size);
        let margin_right = style.margin_right().to_px(containing_block_inline_size);
        {
            let used = self.used_mut(node);
            used.padding_left
                .set(style.padding_left().to_px(containing_block_inline_size));
            used.padding_right
                .set(style.padding_right().to_px(containing_block_inline_size));
            used.margin_left.set(margin_left);
            used.margin_right.set(margin_right);
            used.border_left.set(style.border_left_width());
            used.border_right.set(style.border_right_width());
        }
        let sizing = self.sizing();
        let compute = |input: Option<CssPixels>| -> CssPixels {
            if let Some(value) = input {
                return value;
            }
            // If 'width' is computed as 'auto', the used value is the "shrink-to-fit" width.
            if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                // Find the available inline size: in this case, this is the inline size of the containing
                // block minus the used values of 'margin-left', 'border-left-width', 'padding-left',
                // 'padding-right', 'border-right-width', 'margin-right', and the widths of any relevant scroll bars.
                let used = self.used(node);
                let available_inline_size = available_space.inline_size.to_px_or_zero()
                    - margin_left
                    - style.border_left_width()
                    - used.padding_left.get()
                    - used.padding_right.get()
                    - style.border_right_width()
                    - margin_right;
                // Then the shrink-to-fit inline size is:
                // min(max(preferred minimum inline size, available inline size), preferred inline size).
                let preferred = sizing.calculate_max_content_inline_size(node, constraints);
                if preferred <= available_inline_size {
                    preferred
                } else {
                    sizing
                        .calculate_min_content_inline_size(node, constraints)
                        .max(available_inline_size)
                        .min(preferred)
                }
            } else if matches!(
                available_space.inline_size,
                AvailableSize::Indefinite | AvailableSize::MaxContent
            ) {
                // Fold the shrink-to-fit formula for an indefinite or max-content available inline size.
                sizing.calculate_max_content_inline_size(node, constraints)
            } else {
                // Fold the shrink-to-fit formula for a min-content available inline size.
                sizing.calculate_min_content_inline_size(node, constraints)
            }
        };

        let input = if sizing.should_treat_inline_size_as_auto(node, available_space) {
            None
        } else {
            Some(sizing.calculate_inner_inline_size(node, available_space.inline_size, style.width(), constraints))
        };
        // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
        let mut inline_size = compute(input);
        // 2. The tentative used width is greater than 'max-width', the rules above are applied again,
        //    but this time using the computed value of 'max-width' as the computed value for 'width'.
        if !sizing.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let maximum =
                sizing.calculate_inner_inline_size(node, available_space.inline_size, style.max_width(), constraints);
            if inline_size > maximum {
                inline_size = compute(Some(maximum));
            }
        }
        // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
        //    but this time using the value of 'min-width' as the computed value for 'width'.
        if !style.min_width().is_auto() {
            let minimum =
                sizing.calculate_inner_inline_size(node, available_space.inline_size, style.min_width(), constraints);
            if inline_size < minimum {
                inline_size = compute(Some(minimum));
            }
        }
        self.used_mut(node).set_content_inline_size(inline_size);
    }

    fn compute_inline_size_for_block_level_replaced_element_in_normal_flow(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) {
        // 10.3.6 Floating, replaced elements
        let style = self.style(node);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        // 10.3.4 Block-level, replaced elements in normal flow
        // The used value of 'width' is determined as for inline replaced elements. Then the rules for
        // non-replaced block-level elements are applied to determine the margins.
        // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
        let used = self.used_mut(node);
        used.margin_left
            .set(style.margin_left().to_px(containing_block_inline_size));
        used.margin_right
            .set(style.margin_right().to_px(containing_block_inline_size));
        used.border_left.set(style.border_left_width());
        used.border_right.set(style.border_right_width());
        used.padding_left
            .set(style.padding_left().to_px(containing_block_inline_size));
        used.padding_right
            .set(style.padding_right().to_px(containing_block_inline_size));
        let inline_size = self
            .sizing()
            .compute_inline_size_for_replaced_element(node, available_space, constraints);
        self.used_mut(node).set_content_inline_size(inline_size);
    }

    pub(crate) fn resolve_used_block_size_if_not_treated_as_auto(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) {
        let sizing = self.sizing();
        if sizing.should_treat_block_size_as_auto(node, available_space, constraints) {
            return;
        }
        let style = self.style(node);
        let mut block_size = sizing.calculate_inner_block_size(node, available_space, style.height(), constraints);
        if !sizing.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            block_size = block_size.min(sizing.calculate_inner_block_size(
                node,
                available_space,
                style.max_height(),
                constraints,
            ));
        }
        if !style.min_height().is_auto() {
            block_size = block_size.max(sizing.calculate_inner_block_size(
                node,
                available_space,
                style.min_height(),
                constraints,
            ));
        }
        let used = self.used_mut(node);
        used.set_content_block_size(block_size);
        // A resolved used block size is not always a definite containing block size.
        // Intrinsic sizing keywords like fit-content still depend on child layout,
        // so percentage-sized descendants must continue to treat it as indefinite.
        if !style.height().is_intrinsic_sizing_constraint() {
            used.has_definite_block_size.set(true);
        }
    }

    pub(crate) fn resolve_used_block_size_if_treated_as_auto(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        child_automatic_block_size: Option<CssPixels>,
    ) {
        let sizing = self.sizing();
        if !sizing.should_treat_block_size_as_auto(node, available_space, constraints) {
            return;
        }
        let style = self.style(node);
        let facts = self.facts(node);
        let mut block_size = if sizing.box_is_sized_as_replaced_element(node, available_space, constraints) {
            sizing.compute_block_size_for_replaced_element(node, available_space, constraints)
        } else {
            child_automatic_block_size.unwrap_or_else(|| {
                self.compute_automatic_block_size_for_block_level_element(
                    node,
                    self.used(node)
                        .available_inner_space_or_constraints_from(available_space),
                    constraints,
                )
            })
        };
        if !sizing.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            block_size = block_size.min(sizing.calculate_inner_block_size(
                node,
                available_space,
                style.max_height(),
                constraints,
            ));
        }
        if !style.min_height().is_auto() {
            block_size = block_size.max(sizing.calculate_inner_block_size(
                node,
                available_space,
                style.min_height(),
                constraints,
            ));
        }

        if facts.document_in_quirks_mode() && facts.is_html_html_element() && style.height().is_auto() {
            // 3.6. The html element fills the viewport quirk
            // https://quirks.spec.whatwg.org/#the-html-element-fills-the-viewport-quirk
            // FIXME: Handle vertical writing mode.

            // 1. Let margins be sum of the used values of the margin-left and margin-right properties of element
            //    if element has a vertical writing mode, otherwise let margins be the sum of the used values of
            //    the margin-top and margin-bottom properties of element.
            let used = self.used(node);
            let margins = used.margin_top.get() + used.margin_bottom.get();
            // 2. Let size be the size of the initial containing block in the block flow direction minus margins.
            let size = constraints.block_basis() - margins;
            // 3. Return the bigger value of size and the normal border box size the element would have
            //    according to the CSS specification.
            block_size = block_size.max(size);
            // NOTE: The block size of the root element when affected by this quirk is considered to be definite.
            self.used_mut(node).has_definite_block_size.set(true);
        }

        if facts.document_in_quirks_mode() && facts.is_html_body_element() && style.height().is_auto() {
            // 3.7. The body element fills the html element quirk
            // https://quirks.spec.whatwg.org/#the-body-element-fills-the-html-element-quirk
            // FIXME: Handle vertical writing mode.

            // The element body must additionally meet the following conditions:
            // - The computed value of the 'position' property of element is neither 'absolute' nor 'fixed'.
            // - The computed value of the 'float' property of element is 'none'.
            // - Element is not an inline-level element.
            // - Element is not a multi-column spanning element.
            // NON-STANDARD: We don't check column-span since no browser actually excludes it.
            if !facts.is_absolutely_positioned() && !facts.is_floating() && !facts.is_inline() {
                // 1. Let margins be sum of the used values of the margin-left and margin-right properties of element
                //    if element has a vertical writing mode, otherwise let margins be the sum of the used values of
                //    the margin-top and margin-bottom properties of element.
                let used = self.used(node);
                let margins = used.margin_top.get() + used.margin_bottom.get();
                // 2. Let size be the size of element's parent element's content box in the block flow direction minus margins.
                let size = constraints.block_basis() - margins;
                // 3. Return the bigger value of size and the normal border box size the element would have
                //    according to the CSS specification.
                block_size = block_size.max(size);
            }
        }
        self.used_mut(node).set_content_block_size(block_size);
    }

    fn band_index_at(&self, block_offset: CssPixels) -> usize {
        let bands = self.bands.borrow();
        assert!(!bands.is_empty());
        let mut index = 0;
        for (candidate, band) in bands.iter().enumerate().skip(1) {
            if band.block_start > block_offset {
                break;
            }
            index = candidate;
        }
        index
    }

    fn band_at(&self, block_offset: CssPixels) -> FloatBand {
        self.bands.borrow()[self.band_index_at(block_offset)]
    }

    pub(crate) fn next_float_band_block_start_after(&self, block_offset_in_root: CssPixels) -> Option<CssPixels> {
        self.bands
            .borrow()
            .iter()
            .find(|band| band.block_start > block_offset_in_root)
            .map(|band| band.block_start)
    }

    pub(crate) fn available_inline_space(
        &self,
        block_start_in_root: CssPixels,
        block_end_in_root: CssPixels,
    ) -> SpaceUsedByFloats {
        let bands = self.bands.borrow();
        assert!(!bands.is_empty());
        let mut intrusions = SpaceUsedByFloats::default();
        if block_end_in_root <= block_start_in_root {
            let band = bands[self.band_index_at(block_start_in_root)];
            intrusions.left = band.left_intrusion;
            intrusions.right = band.right_intrusion;
            return intrusions;
        }
        for band in bands.iter().skip(self.band_index_at(block_start_in_root)) {
            if band.block_start >= block_end_in_root {
                break;
            }
            intrusions.left = intrusions.left.max(band.left_intrusion);
            intrusions.right = intrusions.right.max(band.right_intrusion);
        }
        intrusions
    }

    fn intrusions_for_band_into_rect(&self, band: FloatBand, rect_in_root: BlockCssPixelRect) -> SpaceUsedByFloats {
        // Deliberately read the root inline size at query time. It can still be stale
        // while intrinsic sizing is in progress.
        let root_content_inline_size = self.used(self.root).content_inline_size.get();
        SpaceUsedByFloats {
            left: if band.left_intrusion == CssPixels::default() {
                CssPixels::default()
            } else {
                (band.left_intrusion - rect_in_root.x).max(CssPixels::default())
            },
            right: if band.right_intrusion == CssPixels::default() {
                CssPixels::default()
            } else {
                (band.right_intrusion - (root_content_inline_size - rect_in_root.right())).max(CssPixels::default())
            },
        }
    }

    pub(crate) fn intrusion_by_floats_into_rect(
        &self,
        box_in_root_rect: FfiCssPixelRect,
        block_start_in_box: CssPixels,
        block_end_in_box: CssPixels,
    ) -> SpaceUsedByFloats {
        let rect = BlockCssPixelRect {
            x: box_in_root_rect.x,
            y: box_in_root_rect.y,
            width: box_in_root_rect.width,
            height: box_in_root_rect.height,
        };
        let intrusions = self.available_inline_space(rect.y + block_start_in_box, rect.y + block_end_in_box);
        // Deliberately read the root inline size at query time.
        let root_content_inline_size = self.used(self.root).content_inline_size.get();
        SpaceUsedByFloats {
            left: if intrusions.left == CssPixels::default() {
                CssPixels::default()
            } else {
                (intrusions.left - rect.x).max(CssPixels::default())
            },
            right: if intrusions.right == CssPixels::default() {
                CssPixels::default()
            } else {
                (intrusions.right - (root_content_inline_size - rect.right())).max(CssPixels::default())
            },
        }
    }

    fn place_float(
        &self,
        side: FloatSide,
        used: &UsedValues,
        available_space: AvailableSpace,
        containing_block_rect_in_root: BlockCssPixelRect,
        ceiling_in_root: CssPixels,
    ) -> FloatPlacement {
        let margin_box_inline_size = used.margin_box_inline_size(false);
        let mut candidate_block_start = ceiling_in_root;
        loop {
            let band = self.band_at(candidate_block_start);
            let intrusions = self.intrusions_for_band_into_rect(band, containing_block_rect_in_root);
            let available_inline_size =
                available_space.inline_size.to_px_or_zero() - intrusions.left - intrusions.right;
            let has_floats_present =
                band.left_intrusion > CssPixels::default() || band.right_intrusion > CssPixels::default();
            if matches!(
                available_space.inline_size,
                AvailableSize::MaxContent | AvailableSize::Indefinite
            )
                || margin_box_inline_size <= available_inline_size
                || !has_floats_present
            {
                return FloatPlacement {
                    block_start: candidate_block_start,
                    offset_from_edge: if side == FloatSide::Left {
                        intrusions.left + used.margin_left.get() + used.border_box_left(false)
                    } else {
                        intrusions.right
                            + used.content_inline_size.get()
                            + used.margin_right.get()
                            + used.border_box_right(false)
                    },
                };
            }
            let Some(next) = self.next_float_band_block_start_after(candidate_block_start) else {
                return FloatPlacement {
                    block_start: candidate_block_start,
                    offset_from_edge: if side == FloatSide::Left {
                        intrusions.left + used.margin_left.get() + used.border_box_left(false)
                    } else {
                        intrusions.right
                            + used.content_inline_size.get()
                            + used.margin_right.get()
                            + used.border_box_right(false)
                    },
                };
            };
            candidate_block_start = next;
        }
    }

    fn ensure_band_boundary(&self, block_start: CssPixels) {
        let mut bands = self.bands.borrow_mut();
        assert!(!bands.is_empty());
        for index in 0..bands.len() {
            if bands[index].block_start == block_start {
                return;
            }
            if bands[index].block_start > block_start {
                let mut band = if index == 0 {
                    FloatBand::default()
                } else {
                    bands[index - 1]
                };
                band.block_start = block_start;
                bands.insert(index, band);
                return;
            }
        }
        let mut band = *bands.last().unwrap();
        band.block_start = block_start;
        bands.push(band);
    }

    fn add_float_to_bands(&self, floating_box: FloatingBox, mut containing_block_rect_in_root: BlockCssPixelRect) {
        let pending_adjustment =
            self.block_offset_adjustment_from_pending_ancestor_block_start_margins(floating_box.box_);
        containing_block_rect_in_root.y += pending_adjustment;
        // SAFETY: The float record points to a stable state-owned entry.
        let used = floating_box.used_values;
        let root_content_inline_size = self.used(self.root).content_inline_size.get();
        let margin_box_rect = floating_box
            .margin_box_rect_in_root_coordinate_space
            .translated(CssPixels::default(), pending_adjustment);
        let block_start = margin_box_rect.y;
        let block_end = margin_box_rect.bottom();
        if floating_box.side == FloatSide::Left {
            self.lowest_left_margin_edge
                .set(self.lowest_left_margin_edge.get().max(block_end));
        } else {
            self.lowest_right_margin_edge
                .set(self.lowest_right_margin_edge.get().max(block_end));
        }
        if block_end <= block_start {
            return;
        }
        self.ensure_band_boundary(block_start);
        self.ensure_band_boundary(block_end);
        let intrusion = if floating_box.side == FloatSide::Left {
            containing_block_rect_in_root.x
                + floating_box.offset_from_edge
                + used.content_inline_size.get()
                + used.margin_right.get()
                + used.border_box_right(false)
        } else {
            (root_content_inline_size - containing_block_rect_in_root.right())
                + floating_box.offset_from_edge
                + used.margin_left.get()
                + used.border_box_left(false)
        };
        for band in self.bands.borrow_mut().iter_mut() {
            if band.block_start < block_start {
                continue;
            }
            if band.block_start >= block_end {
                break;
            }
            if floating_box.side == FloatSide::Left {
                band.left_intrusion = band.left_intrusion.max(intrusion);
            } else {
                band.right_intrusion = band.right_intrusion.max(intrusion);
            }
        }
    }

    fn rebuild_float_bands(&self) {
        *self.bands.borrow_mut() = vec![FloatBand::default()];
        self.lowest_left_margin_edge.set(CssPixels::default());
        self.lowest_right_margin_edge.set(CssPixels::default());
        let floats = self.floats.borrow();
        for &floating_box in floats.iter() {
            self.add_float_to_bands(
                floating_box,
                floating_box.containing_block_rect_in_root_coordinate_space,
            );
        }
    }

    fn update_lowest_floating_descendant_bottom_margin_edge(&self) {
        let lowest = self
            .floats
            .borrow()
            .iter()
            .map(|floating_box| floating_box.margin_box_rect_in_root_coordinate_space.bottom())
            .max();
        let root_slot_index = self.callbacks.slot_index(self.root);
        self.state
            .block_rare_data_mut(root_slot_index)
            .lowest_floating_descendant_bottom_margin_edge = lowest;
    }

    fn translate_floats_in_subtree(&self, ancestor: Node, delta: FfiCssPixelPoint) {
        if (delta.x == CssPixels::default() && delta.y == CssPixels::default()) || self.floats.borrow().is_empty() {
            return;
        }
        let mut any_float_moved = false;
        for floating_box in self.floats.borrow_mut().iter_mut() {
            if !self.is_ancestor_of(ancestor, floating_box.box_) {
                continue;
            }
            floating_box.margin_box_rect_in_root_coordinate_space = floating_box
                .margin_box_rect_in_root_coordinate_space
                .translated(delta.x, delta.y);
            floating_box.containing_block_rect_in_root_coordinate_space = floating_box
                .containing_block_rect_in_root_coordinate_space
                .translated(delta.x, delta.y);
            any_float_moved = true;
        }
        if !any_float_moved {
            return;
        }
        self.update_lowest_floating_descendant_bottom_margin_edge();
        self.rebuild_float_bands();
    }

    fn margin_box_left_of_float_in_root(
        floating_box: FloatingBox,
        containing_block_rect_in_root: BlockCssPixelRect,
    ) -> CssPixels {
        // SAFETY: The state owns the float's used-values entry.
        let used = floating_box.used_values;
        if floating_box.side == FloatSide::Left {
            containing_block_rect_in_root.x + floating_box.offset_from_edge
                - used.margin_left.get()
                - used.border_box_left(false)
        } else {
            containing_block_rect_in_root.right()
                - floating_box.offset_from_edge
                - used.margin_left.get()
                - used.border_box_left(false)
        }
    }

    fn border_box_left_of_box_avoiding_floats(
        &self,
        node: Node,
        used: &UsedValues,
        space_used_by_floats: SpaceUsedByFloats,
    ) -> CssPixels {
        if self.style(node).margin_left().is_auto() {
            return space_used_by_floats.left + used.margin_left.get();
        }
        if used.margin_left.get() >= CssPixels::default() {
            return space_used_by_floats.left.max(used.margin_left.get());
        }
        if space_used_by_floats.left > CssPixels::default() || space_used_by_floats.right > CssPixels::default() {
            return space_used_by_floats.left;
        }
        space_used_by_floats.left + used.margin_left.get()
    }

    fn avoid_float_intrusions(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        mut content_block_offset: CssPixels,
        containing_block_rect_in_root: BlockCssPixelRect,
    ) -> CssPixels {
        if !matches!(available_space.inline_size, AvailableSize::Definite(_)) || !self.box_should_avoid_floats_because_it_establishes_fc(node) {
            return content_block_offset;
        }
        // https://drafts.csswg.org/css2/#floats
        // If necessary, implementations should clear the said element by placing it below any preceding floats, but may
        // place it adjacent to such floats if there is sufficient space.
        loop {
            let used = self.used(node);
            let border_box_block_offset_in_root =
                containing_block_rect_in_root.y + content_block_offset - used.border_box_top(false);
            let band_rect = BlockCssPixelRect {
                y: border_box_block_offset_in_root,
                height: used.border_box_block_size(false),
                ..containing_block_rect_in_root
            };
            let space = self.intrusions_for_band_into_rect(self.band_at(border_box_block_offset_in_root), band_rect);
            let constrained = space.left > CssPixels::default() || space.right > CssPixels::default();
            let border_box_left = self.border_box_left_of_box_avoiding_floats(node, used, space);
            let mut must_clear = constrained
                && border_box_left + used.border_box_inline_size(false)
                    > available_space.inline_size.to_px_or_zero() - space.right;
            if !must_clear {
                let border_rect = BlockCssPixelRect {
                    x: band_rect.x + border_box_left,
                    y: border_box_block_offset_in_root,
                    width: used.border_box_inline_size(false),
                    height: used.border_box_block_size(false),
                };
                must_clear = self.floats.borrow().iter().any(|floating_box| {
                    let adjustment =
                        self.block_offset_adjustment_from_pending_ancestor_block_start_margins(floating_box.box_);
                    let margin_rect = floating_box
                        .margin_box_rect_in_root_coordinate_space
                        .translated(CssPixels::default(), adjustment);
                    // A zero-area overlap deliberately does not clear.
                    margin_rect.intersects(border_rect)
                });
            }
            if !must_clear {
                break;
            }
            let Some(next) = self.next_float_band_block_start_after(border_box_block_offset_in_root) else {
                break;
            };
            content_block_offset += next - border_box_block_offset_in_root;
            let position = FfiCssPixelPoint {
                x: containing_block_rect_in_root.x,
                y: containing_block_rect_in_root.y + content_block_offset,
            };
            // Deliberately re-run inline sizing after every band descent.
            self.compute_inline_size(node, available_space, constraints, position);
        }
        content_block_offset
    }

    pub(crate) fn block_offset_adjustment_from_pending_ancestor_block_start_margins(&self, node: Node) -> CssPixels {
        let margin_state = self.margin_state.borrow();
        let mut adjustment = CssPixels::default();
        for group in margin_state.pending_top_margin_groups() {
            if group.pinned_by_clearance {
                continue;
            }
            if self.is_inclusive_ancestor_of(group.box_, node) {
                adjustment += group.collapsed_margin - group.collapsed_margin_at_open;
            }
        }
        adjustment
    }

    fn margins_collapse_through(&self, node: Node) -> bool {
        let facts = self.facts(node);
        let style = self.style(node);
        // https://drafts.csswg.org/css2/#adjoining-margins
        // Two margins are adjoining if and only if:
        // - both belong to in-flow block-level boxes that participate in the same block formatting context
        //   NB: Yes, we're dealing with one and the same box here.

        // - no line boxes, no clearance, no padding and no border separate them (Note that certain zero-height line boxes
        //   (see 9.4.2) are ignored for this purpose.)
        // NB: Border and padding are handled further down.
        if style.clear() != clear::NONE {
            return false;
        }
        // - both belong to vertically-adjacent box edges, i.e. form one of the following pairs:
        //   - top and bottom margins of a box that does not establish a new block formatting context and that has zero
        //     computed 'min-height', zero or 'auto' computed 'height', and no in-flow children
        if facts.creates_block_formatting_context() {
            return false;
        }
        // https://drafts.csswg.org/css-display-3/#independent-formatting-context
        // NOTE: [..] margins do not collapse across formatting context boundaries.
        if formatting_context_type_created_by_box(facts).is_some() {
            return false;
        }
        // NB: This should take care of the height and min-height constraints.
        //     ( also see https://github.com/w3c/csswg-drafts/pull/13699#issuecomment-4103045370 for spec ambiguity )
        if self.used(node).border_box_block_size(false) != CssPixels::default() {
            return false;
        }
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-margin-collapse
        // FIXME: For the purpose of margin collapsing (CSS 2 §8.3.1 Collapsing margins), if the block axis is the
        //        ratio-dependent axis, it is not considered to have a computed block-size of auto.

        // AD-HOC: The "and no in-flow children" above is wrong. (see https://github.com/w3c/csswg-drafts/pull/13699 )
        for child in self.children(node) {
            let child_facts = self.facts(child);
            if child_facts.is_absolutely_positioned() || child_facts.is_floating() {
                continue;
            }
            if !self.margins_collapse_through(child) {
                return false;
            }
        }
        true
    }

    pub(crate) fn commit_pending_margin_before_inline_content(&self) -> CssPixels {
        let mut margin_state = self.margin_state.borrow_mut();
        let has_open = margin_state.has_open_top_margin_group();
        let collapsed = margin_state.current_collapsed_margin();
        margin_state.update_open_top_margin_group();
        margin_state.reset();
        if has_open { CssPixels::default() } else { collapsed }
    }

    pub(crate) fn reset_margin_state(&self) {
        self.margin_state.borrow_mut().reset();
    }

    pub(crate) fn clear_floating_boxes(
        &self,
        node: Node,
        inline_context: Option<&InlineFormattingContext>,
        containing_block_position_in_root: FfiCssPixelPoint,
    ) -> bool {
        let style = self.style(node);
        let mut result = false;
        let clear_to = |clearance_block_offset_in_root: CssPixels, result: &mut bool| {
            if clearance_block_offset_in_root == CssPixels::default() {
                return;
            }
            // NOTE: Floating boxes are globally relevant within this BFC, *but* their offset coordinates
            //       are relative to their containing block.
            //       This means that we have to first convert to a root-space block offset before clearing,
            //       and then convert back to a local block offset when assigning the cleared offset to
            //       the `child_box` layout state.
            let clearance = clearance_block_offset_in_root
                - containing_block_position_in_root.y
                - self.block_offset_adjustment_from_pending_ancestor_block_start_margins(node);
            if let Some(inline_context) = inline_context {
                if clearance > inline_context.block_axis_float_clearance() {
                    *result = true;
                    inline_context.set_block_axis_float_clearance(clearance);
                }
            } else if clearance
                > self
                    .block_offset_of_current_block_container
                    .get()
                    .expect("a block container flow cursor is active")
            {
                *result = true;
                self.block_offset_of_current_block_container.set(Some(clearance));
            }
        };

        // FIXME: Honor writing-mode, direction and text-orientation.
        if matches!(style.clear(), clear::LEFT | clear::BOTH | clear::INLINE_START) {
            clear_to(self.lowest_left_margin_edge.get(), &mut result);
        }
        if matches!(style.clear(), clear::RIGHT | clear::BOTH | clear::INLINE_END) {
            clear_to(self.lowest_right_margin_edge.get(), &mut result);
        }
        result
    }

    fn compute_normal_flow_inline_offset(
        &self,
        node: Node,
        available_space: AvailableSpace,
        content_position_in_root: FfiCssPixelPoint,
    ) -> CssPixels {
        let used = self.used(node);
        let mut inline_offset = CssPixels::default();
        let mut available_inline_size_within_containing_block = available_space.inline_size.to_px_or_zero();
        if self.box_should_avoid_floats_because_it_establishes_fc(node) {
            let space = self.intrusion_by_floats_into_rect(
                BlockCssPixelRect {
                    x: content_position_in_root.x,
                    y: content_position_in_root.y,
                    width: used.content_inline_size.get(),
                    height: used.content_block_size.get(),
                }
                .into(),
                CssPixels::default(),
                CssPixels::default(),
            );
            available_inline_size_within_containing_block -= space.left + space.right;
            // Subtracting the left margin here because it is applied again when the margin box offset is added below.
            inline_offset = self.border_box_left_of_box_avoiding_floats(node, used, space) - used.margin_left.get();
        }

        let containing_block = self.containing_block(node);
        let containing_text_align = self.style(containing_block).text_align();
        if containing_text_align == text_align::_LIBWEB_CENTER {
            inline_offset += available_inline_size_within_containing_block / 2 - used.content_inline_size.get() / 2;
        } else if containing_text_align == text_align::_LIBWEB_RIGHT {
            // Subtracting the left margin here because left and right margins need to be swapped when aligning to the right
            inline_offset += available_inline_size_within_containing_block
                - used.content_inline_size.get()
                - used.margin_left.get()
                - used.border_box_left(false);
        } else {
            inline_offset += used.margin_left.get() + used.border_box_left(false);
        }
        inline_offset
    }

    pub(crate) fn dimension_list_item_marker(&self, marker: Node) {
        let facts = self.facts(marker);
        let used = self.used_mut(marker);
        used.set_content_inline_size(facts.marker_content_inline_size());
        used.set_content_block_size(facts.marker_content_block_size());
    }

    pub(crate) fn distance_between_marker_and_list_item(&self, marker: Node) -> CssPixels {
        self.facts(marker).marker_distance()
    }

    fn layout_list_item_marker(
        &self,
        list_item: Node,
        inline_space_used_before_list_item_elements_formatted: SpaceUsedByFloats,
    ) {
        let marker = self.facts(list_item).list_item_marker();
        if marker.is_invalid() {
            return;
        }
        let marker_facts = self.facts(marker);
        let marker_style = self.style(marker);
        let marker_distance = self.distance_between_marker_and_list_item(marker);
        let marker_used = self.used(marker);
        let marker_block_size = marker_used.content_block_size.get();
        let marker_inline_size = marker_used.content_inline_size.get();
        let list_item_style = self.style(list_item);
        let list_item_used = self.used(list_item);
        let marker_inline_offset = if list_item_style.direction() == direction::LTR {
            inline_space_used_before_list_item_elements_formatted.left - marker_distance - marker_inline_size
        } else {
            list_item_used.content_inline_size.get()
                - (inline_space_used_before_list_item_elements_formatted.right - marker_distance)
        };
        let marker_block_offset = ((marker_style.line_height() - marker_block_size) / 2).max(CssPixels::default());

        if marker_facts.marker_list_style_position() == list_style_position::INSIDE {
            // FIXME: Just adjusting the content inline size for an inside marker is wrong, as it will still position
            //        the marker outside of the box, instead of treating it more like an inline child on the first line.
            self.used_mut(list_item).set_content_inline_size(
                self.used(list_item).content_inline_size.get() - marker_inline_size - marker_distance,
            );
        }

        // Animations can make `float` or `position` apply to ::marker.
        if !marker_facts.is_floating() && !marker_facts.is_absolutely_positioned() {
            self.place_child(
                marker,
                FfiCssPixelPoint {
                    x: round_css_pixels(marker_inline_offset),
                    y: round_css_pixels(marker_block_offset),
                },
            );
        }
        if marker_style.line_height() > self.used(list_item).content_block_size.get() {
            self.used_mut(list_item)
                .set_content_block_size(marker_style.line_height());
        }
    }

    fn layout_inside(
        &self,
        frame: &mut FcFrame<'pass>,
        node: Node,
        input: LayoutInput,
        force_independent_context_run: bool,
    ) -> Option<PendingChildLayout<'pass>> {
        let used = self.used(node);
        let facts = self.facts(node);
        // OPTIMIZATION: If we're doing intrinsic sizing and `child_box` has definite size in both axes,
        //               we don't need to layout its insides. The size is resolvable without learning
        //               the metrics of whatever's inside the box.
        //
        // https://drafts.csswg.org/css2/#propdef-vertical-align
        // The baseline of an inline-block is the baseline of its last line box in the normal flow, unless it has
        // either no in-flow line boxes or if its 'overflow' property has a computed value other than visible, in which
        // case the baseline is the bottom margin edge.
        //
        // Inline-level boxes can contribute a baseline to their parent line box, so they still need their contents
        // laid out even when their own intrinsic size is already definite.
        if !force_independent_context_run
            && self.layout_mode == LayoutMode::IntrinsicSizing
            && !facts.is_inline()
            && used.inline_size_constraint.get() == SizeConstraint::None
            && used.block_size_constraint.get() == SizeConstraint::None
            && used.has_definite_inline_size()
            && used.has_definite_block_size()
        {
            return None;
        }
        let creates_replaced_context = matches!(
            formatting_context_type_created_by_box(facts),
            Some(FfiFormattingContextType::InternalReplaced | FfiFormattingContextType::ReplacedWithChildren)
        );
        if !facts.can_have_children() && !creates_replaced_context {
            return None;
        }
        match crate::layout::layout_inside_child(
            frame,
            Some(self),
            None,
            node,
            self.layout_mode,
            input,
            force_independent_context_run,
        ) {
            crate::layout::ChildLayoutOutcome::Skipped => None,
            crate::layout::ChildLayoutOutcome::Created(child_layout) => Some(child_layout),
            crate::layout::ChildLayoutOutcome::ReenterCurrent => {
                self.run(frame, input);
                None
            }
        }
    }

    fn child_layout_input(
        &self,
        containing_block: Node,
        containing_input: LayoutInput,
        available_space: AvailableSpace,
    ) -> LayoutInput {
        LayoutInput {
            available_space,
            containing_block_constraints: self
                .sizing()
                .constraints_for_child_context(containing_block, containing_input.containing_block_constraints),
            content_box_position_in_bfc_root: containing_input.content_box_position_in_bfc_root,
            table_grid_min_border_box_block_size: None,
        }
    }

    fn layout_block_level_box(
        &self,
        frame: &mut FcFrame<'pass>,
        node: Node,
        block_container: Node,
        bottom_of_lowest_margin_box: &mut CssPixels,
        input: LayoutInput,
    ) {
        let available_space = input.available_space;
        let facts = self.facts(node);

        if facts.is_absolutely_positioned() {
            if self.layout_mode == LayoutMode::Normal {
                // NB: An originally-inline absolutely positioned box never reaches this path; the tree
                //     builder keeps out-of-flow boxes in inline context, where static position markers
                //     pin them at their exact flow position.
                self.register_contained_abspos_child(
                    node,
                    self.block_offset_of_current_block_container
                        .get()
                        .expect("a block container flow cursor is active"),
                );
            }
            return;
        }

        // NOTE: ListItemMarkerBoxes are placed by their corresponding ListItemBox, and their used
        //       values were created together with its own.
        if facts.is_list_item_marker_box() {
            return;
        }

        // NOTE: It is possible to encounter SVGMaskBox and SVGClipBox nodes while doing layout of the
        //       formatting context established by a <foreignObject> that references them. Skip them
        //       before creating any used values; SVGFormattingContext lays them out on behalf of the
        //       referencing element.
        if facts.is_svg_mask_box() || facts.is_svg_clip_box() {
            return;
        }

        let block_container_inline_size = self.used(block_container).content_inline_size.get();
        if self.try_used_pointer(node).is_some() {
            assert!(
                self.state
                    .may_reuse_precreated_used_values(self.callbacks.slot_index(node)),
                "block layout visited a box whose used values were already created"
            );
        } else {
            self.create_used_values(node, input.containing_block_constraints);
        }

        self.resolve_vertical_box_model_metrics(node, block_container_inline_size);
        assert_eq!(self.containing_block(node), block_container);
        let containing_block_position_in_root = input
            .content_box_position_in_bfc_root
            .expect("block layout requires its containing block position in the BFC root");

        if facts.is_floating() {
            let block_offset = self
                .block_offset_of_current_block_container
                .get()
                .expect("a block container flow cursor is active");
            let margin_top = {
                let margin_state = self.margin_state.borrow();
                if margin_state.has_open_top_margin_group() {
                    CssPixels::default()
                } else {
                    margin_state.current_collapsed_margin()
                }
            };
            self.layout_floating_box(frame, node, block_container, input, margin_top + block_offset, None);
            if let Some(floating_box) = self.floats.borrow().last() {
                *bottom_of_lowest_margin_box = (*bottom_of_lowest_margin_box).max(floating_box.bottom_margin_edge);
            }
            return;
        }

        self.margin_state
            .borrow_mut()
            .add_margin(self.used(node).margin_top.get());
        let introduced_clearance = self.clear_floating_boxes(node, None, containing_block_position_in_root);
        if introduced_clearance {
            self.margin_state.borrow_mut().reset();
        }
        self.margin_state.borrow_mut().update_open_top_margin_group();

        let block_offset = self
            .block_offset_of_current_block_container
            .get()
            .expect("a block container flow cursor is active");

        let style = self.style(node);
        let box_is_html_element_in_quirks_mode =
            facts.document_in_quirks_mode() && facts.is_html_html_element() && style.height().is_auto();

        // NOTE: In quirks mode, the html element's block size matches the viewport so it can be treated as definite.
        if self.used(node).has_definite_block_size() || box_is_html_element_in_quirks_mode {
            self.resolve_used_block_size_if_treated_as_auto(
                node,
                available_space,
                input.containing_block_constraints,
                None,
            );
        }

        let independent_type = formatting_context_type_created_by_box(facts);
        let has_independent_formatting_context = independent_type.is_some();

        if !has_independent_formatting_context && !facts.is_block_container() {
            // Keep the C++ behavior: diagnose and skip this unsupported box after its
            // vertical metrics have been resolved.
            eprintln!("FIXME: Block-level box is not BlockContainer but does not create formatting context");
            return;
        }

        let mut margin_top = self.margin_state.borrow().current_collapsed_margin();
        if self.margin_state.borrow().has_open_top_margin_group() {
            // If first child margin top will collapse with margin-top of containing block then margin-top of child is 0
            margin_top = CssPixels::default();
        }

        let box_opens_top_margin_group = !has_independent_formatting_context
            && self.used(node).border_top.get() == CssPixels::default()
            && self.used(node).padding_top.get() == CssPixels::default()
            && !self.margin_state.borrow().has_open_top_margin_group();

        let container_facts = self.facts(block_container);
        let box_is_positioned_by_fieldset_layout =
            container_facts.is_fieldset_box() && container_facts.rendered_legend() == node;

        // Earlier sibling placement may have invalidated cached float bands.
        self.rebuild_float_bands();

        let mut content_block_offset = block_offset + margin_top + self.used(node).border_box_top(false);
        let containing_block_rect_in_root =
            self.containing_block_rect(block_container, containing_block_position_in_root);
        let containing_block_rect_in_root_now = containing_block_rect_in_root.translated(
            CssPixels::default(),
            self.block_offset_adjustment_from_pending_ancestor_block_start_margins(node),
        );
        let content_position_in_root_now = |content_block_offset: CssPixels| FfiCssPixelPoint {
            x: containing_block_rect_in_root_now.x,
            y: containing_block_rect_in_root_now.y + content_block_offset,
        };

        self.compute_inline_size(
            node,
            available_space,
            input.containing_block_constraints,
            content_position_in_root_now(content_block_offset),
        );
        content_block_offset = self.avoid_float_intrusions(
            node,
            available_space,
            input.containing_block_constraints,
            content_block_offset,
            containing_block_rect_in_root_now,
        );
        let mut content_inline_offset = self.compute_normal_flow_inline_offset(
            node,
            available_space,
            content_position_in_root_now(content_block_offset),
        );

        // FIXME: We currently do not support ListItemBoxes generated by pseudo-elements. We will need to, eventually.
        let is_list_item_box_without_css_content = facts.is_list_item_box() && !facts.has_css_marker_content();
        let marker = facts.list_item_marker();
        if is_list_item_box_without_css_content && !marker.is_invalid() {
            self.dimension_list_item_marker(marker);
            let marker_facts = self.facts(marker);
            if marker_facts.marker_list_style_position() == list_style_position::INSIDE && style.direction() == direction::LTR
            {
                content_inline_offset +=
                    self.used(marker).content_inline_size.get() + self.distance_between_marker_and_list_item(marker);
            }
        }

        let is_table_formatting_context = independent_type == Some(FfiFormattingContextType::Table);
        let mut pending_position = None;
        if box_is_positioned_by_fieldset_layout {
            self.pending_legend_flow_position.set(Some(LogicalOffset {
                inline_offset: content_inline_offset,
                block_offset: content_block_offset,
            }));
        } else if is_table_formatting_context {
            self.pending_table_box_content_offset_in_wrapper
                .set(Some(LogicalOffset {
                    inline_offset: content_inline_offset,
                    block_offset: content_block_offset,
                }));
        } else if !box_opens_top_margin_group {
            pending_position = Some(FfiCssPixelPoint {
                x: content_inline_offset,
                y: content_block_offset,
            });
        }

        let mut available_space_for_block_size_resolution = available_space;
        let is_table_box = facts.is_table_row()
            || facts.is_table_row_group()
            || facts.is_table_header_group()
            || facts.is_table_footer_group()
            || facts.is_table_cell()
            || facts.is_table_caption();
        // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
        if facts.document_in_quirks_mode()
            && style.height().is_percentage()
            && !is_table_box
            && !facts.is_in_user_agent_shadow_tree()
        {
            available_space_for_block_size_resolution.block_size = AvailableSize::definite(
                input
                    .containing_block_constraints
                    .quirks_mode_percentage_basis_block_size
                    .unwrap_or_default(),
            );
        }

        self.resolve_used_block_size_if_not_treated_as_auto(
            node,
            available_space_for_block_size_resolution,
            input.containing_block_constraints,
        );
        // NOTE: Flex containers with an automatic block size are treated as max-content, so resolve it early.
        if facts.has_auto_content_box_size() || style.display().is_flex_inside() {
            self.resolve_used_block_size_if_treated_as_auto(
                node,
                available_space_for_block_size_resolution,
                input.containing_block_constraints,
                None,
            );
        }

        // Before we insert the children of a list item we need to know the location of the marker.
        // If we do not do this then left-floating elements inside the list item will push the marker to the right,
        // in some cases even causing it to overlap with the non-floating content of the list.
        let mut inline_space_used_before_children_formatted = SpaceUsedByFloats::default();
        if is_list_item_box_without_css_content && !marker.is_invalid() {
            let marker_block_offset = ((self.style(marker).line_height() - self.used(marker).content_block_size.get())
                / 2)
            .max(CssPixels::default());
            let list_item_used = self.used(node);
            inline_space_used_before_children_formatted = self.intrusion_by_floats_into_rect(
                BlockCssPixelRect {
                    x: content_position_in_root_now(content_block_offset).x + content_inline_offset,
                    y: content_position_in_root_now(content_block_offset).y,
                    width: list_item_used.content_inline_size.get(),
                    height: list_item_used.content_block_size.get(),
                }
                .into(),
                marker_block_offset,
                marker_block_offset,
            );
        }

        let child_layout = if has_independent_formatting_context {
            // Margins of elements that establish new formatting contexts do not collapse with their in-flow children
            self.margin_state.borrow_mut().reset();

            // This box establishes a new formatting context. Pass control to it.
            let mut inner_available_space = self
                .used(node)
                .available_inner_space_or_constraints_from(available_space);
            // For boxes with an automatic block size but non-auto min-height, determine whether the content block size is
            // less than min-height. If so, run layout with min-height as the available block size.
            let mut measured_content_block_size = None;
            let sizing = self.sizing();
            if sizing.should_treat_block_size_as_auto(node, available_space, input.containing_block_constraints)
                && !style.min_height().is_auto()
            {
                let content_block_size = sizing.measure_automatic_content_block_size(
                    node,
                    self.layout_mode,
                    inner_available_space,
                    input.containing_block_constraints,
                );
                measured_content_block_size = Some(content_block_size);
                let min_block_size = sizing.calculate_inner_block_size(
                    node,
                    available_space,
                    style.min_height(),
                    input.containing_block_constraints,
                );
                if content_block_size < min_block_size {
                    inner_available_space.block_size = AvailableSize::definite(min_block_size);
                }
            }
            self.sizing().make_button_content_box_definite(
                node,
                self.layout_mode,
                available_space,
                input.containing_block_constraints,
                measured_content_block_size,
            );
            let inside_layout_input = LayoutInput {
                available_space: inner_available_space,
                containing_block_constraints: input.containing_block_constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: if is_table_formatting_context {
                    input.table_grid_min_border_box_block_size
                } else {
                    None
                },
            };
            let child_layout = self.layout_inside(frame, node, inside_layout_input, true);
            if is_table_formatting_context {
                let pending = self
                    .take_pending_table_box_content_offset_in_wrapper()
                    .expect("table layout did not publish its wrapper content offset");
                pending_position = Some(FfiCssPixelPoint {
                    x: pending.inline_offset,
                    y: pending.block_offset,
                });
            }
            if container_facts.is_table_wrapper() && style.display().is_table_inside() {
                let used = self.used_mut(node);
                used.margin_left.set(used.margin_left.get().max(CssPixels::default()));
                used.margin_right.set(used.margin_right.get().max(CssPixels::default()));
            }
            if facts.is_table_wrapper()
                && !facts.is_grid_item()
                && let Some(child_layout) = child_layout.as_ref()
            {
                self.used_mut(node)
                    .set_content_inline_size(child_layout.result().automatic_content_inline_size);
            }
            child_layout
        } else {
            // This box participates in the current block container's flow.
            let space_available_for_children = if facts.is_anonymous() {
                available_space
            } else {
                self.used(node)
                    .available_inner_space_or_constraints_from(available_space)
            };
            if self.used(node).border_top.get() > CssPixels::default()
                || self.used(node).padding_top.get() > CssPixels::default()
            {
                // margin-top of block container can't collapse with its children if it has non-zero border or padding.
                self.margin_state.borrow_mut().reset();
            } else if box_opens_top_margin_group {
                self.margin_state
                    .borrow_mut()
                    .open_top_margin_group(node, introduced_clearance);
            }
            let inside_layout_input = LayoutInput {
                content_box_position_in_bfc_root: Some(FfiCssPixelPoint {
                    x: containing_block_position_in_root.x + content_inline_offset,
                    y: containing_block_position_in_root.y + content_block_offset,
                }),
                ..input
            };
            if facts.children_are_inline() {
                self.layout_inline_children(frame, node, inside_layout_input, space_available_for_children);
            } else {
                self.layout_block_level_children(frame, node, inside_layout_input, space_available_for_children);
            }
            if box_opens_top_margin_group {
                let resolved_margin_top = self.margin_state.borrow_mut().take_pending_top_margin();
                let final_content_block_offset = if introduced_clearance {
                    content_block_offset
                } else {
                    block_offset + resolved_margin_top + self.used(node).border_box_top(false)
                };
                if box_is_positioned_by_fieldset_layout {
                    self.pending_legend_flow_position.set(Some(LogicalOffset {
                        inline_offset: content_inline_offset,
                        block_offset: final_content_block_offset,
                    }));
                } else {
                    pending_position = Some(FfiCssPixelPoint {
                        x: content_inline_offset,
                        y: final_content_block_offset,
                    });
                }
                self.translate_floats_in_subtree(
                    node,
                    FfiCssPixelPoint {
                        x: CssPixels::default(),
                        y: final_content_block_offset - content_block_offset,
                    },
                );
            }
            None
        };

        // Tables already set their block size during the independent formatting context run. With multi-line text cells,
        // using different available space here can produce different line breaks and therefore a different block size.
        if !style.display().is_table_inside() {
            self.resolve_used_block_size_if_treated_as_auto(
                node,
                available_space_for_block_size_resolution,
                input.containing_block_constraints,
                child_layout
                    .as_ref()
                    .map(|child_layout| child_layout.result().automatic_content_block_size),
            );
        }

        // Now that our children are formatted we place the ListItemBox with the left space we remembered.
        if is_list_item_box_without_css_content {
            // The marker pseudo-element will be created from a ListItemMarkerBox
            self.layout_list_item_marker(node, inline_space_used_before_children_formatted);
        }
        // Otherwise, it will be dealt with as a generic pseudo-element with the content of the ::marker pseudo-element.

        if let Some(position) = pending_position {
            self.place_child(node, position);
        }

        if has_independent_formatting_context || !self.margins_collapse_through(node) {
            let mut margin_state = self.margin_state.borrow_mut();
            if !margin_state.box_last_in_flow_child_margin_bottom_collapsed {
                margin_state.reset();
            }
            drop(margin_state);
            let used = self.used(node);
            self.block_offset_of_current_block_container.set(Some(
                used.content_offset.get().y + used.content_block_size.get() + used.border_box_bottom(false),
            ));
        }
        self.margin_state
            .borrow_mut()
            .box_last_in_flow_child_margin_bottom_collapsed = false;
        self.margin_state
            .borrow_mut()
            .add_margin(self.used(node).margin_bottom.get());
        self.margin_state.borrow_mut().update_open_top_margin_group();

        let block_container_used = self.used(block_container);
        self.compute_inset(
            node,
            LogicalSize {
                inline_size: block_container_used.content_inline_size.get(),
                block_size: block_container_used.content_block_size.get(),
            },
        );
        let used = self.used(node);
        *bottom_of_lowest_margin_box = (*bottom_of_lowest_margin_box)
            .max(used.content_offset.get().y + used.content_block_size.get() + used.margin_box_bottom(false));

        if let Some(child_layout) = child_layout {
            child_layout.finish();
        }
    }

    fn layout_block_level_children(
        &self,
        frame: &mut FcFrame<'pass>,
        block_container: Node,
        input: LayoutInput,
        available_space_for_children: AvailableSpace,
    ) {
        assert!(!self.facts(block_container).children_are_inline());
        let available_space = available_space_for_children;
        // The table wrapper is invisible to percentage resolution: percentages on the table root
        // resolve against the wrapper's containing block, so the wrapper's own constraints pass
        // through to the table box unchanged.
        let child_input = if self.facts(block_container).is_table_wrapper() {
            LayoutInput {
                available_space: available_space_for_children,
                ..input
            }
        } else {
            self.child_layout_input(block_container, input, available_space_for_children)
        };
        let mut bottom_of_lowest_margin_box = CssPixels::default();
        let saved = self
            .block_offset_of_current_block_container
            .replace(Some(CssPixels::default()));
        for child in self.children(block_container) {
            self.layout_block_level_box(
                frame,
                child,
                block_container,
                &mut bottom_of_lowest_margin_box,
                child_input,
            );
        }
        self.block_offset_of_current_block_container.set(saved);

        if self.layout_mode == LayoutMode::IntrinsicSizing && !self.used(block_container).has_definite_inline_size() {
            let mut inline_size = self.greatest_child_inline_size_including_floats(block_container);
            let style = self.style(block_container);
            // NOTE: Min and max constraints are not applied to a box that is being sized as intrinsic because
            //       according to css-sizing-3 spec:
            //       The min-content size of a box in each axis is the size it would have if it was a float given an
            //       auto size in that axis (and no minimum or maximum size in that axis) and if its containing block
            //       was zero-sized in that axis.
            if self.used(block_container).inline_size_constraint.get() == SizeConstraint::None {
                let sizing = self.sizing();
                if !sizing.should_treat_max_inline_size_as_none(
                    block_container,
                    available_space.inline_size,
                    input.containing_block_constraints,
                ) {
                    inline_size = inline_size.min(sizing.calculate_inner_inline_size(
                        block_container,
                        available_space.inline_size,
                        style.max_width(),
                        input.containing_block_constraints,
                    ));
                }
                if !style.min_width().is_auto() {
                    inline_size = inline_size.max(sizing.calculate_inner_inline_size(
                        block_container,
                        available_space.inline_size,
                        style.min_width(),
                        input.containing_block_constraints,
                    ));
                }
            }
            let used = self.used_mut(block_container);
            used.set_content_inline_size(inline_size);
            used.set_content_block_size(bottom_of_lowest_margin_box);
        }
        self.compute_and_store_baselines(block_container);
    }

    // https://html.spec.whatwg.org/multipage/rendering.html#the-fieldset-and-legend-elements
    fn layout_fieldset_with_rendered_legend(&self, frame: &mut FcFrame<'pass>, fieldset: Node, input: LayoutInput) {
        let available_space = input.available_space;
        let child_input = self.child_layout_input(fieldset, input, available_space);
        let legend = self.facts(fieldset).rendered_legend();
        assert!(!legend.is_invalid());

        // Lay out the legend to determine its dimensions.
        {
            let saved = self
                .block_offset_of_current_block_container
                .replace(Some(CssPixels::default()));
            let mut dummy_bottom = CssPixels::default();
            self.layout_block_level_box(frame, legend, fieldset, &mut dummy_bottom, child_input);
            self.block_offset_of_current_block_container.set(saved);
        }

        // If the computed value of 'inline-size' is 'auto', then the used value is the fit-content inline size.
        if self.style(legend).width().is_auto() {
            let inline_size = self.sizing().calculate_fit_content_size(
                legend,
                SizingAxis::Inline,
                available_space,
                child_input.containing_block_constraints,
            );
            self.used_mut(legend).set_content_inline_size(inline_size);
        }

        // The space allocated for the element's border on the block-start side is expected to be the element's
        // 'border-block-start-width' or the rendered legend's margin box size in the fieldset's block-flow direction,
        // whichever is greater.
        let effective_border = self
            .used(fieldset)
            .border_top
            .get()
            .max(self.used(legend).margin_box_block_size(false));
        let extra_top = effective_border - self.used(fieldset).border_top.get();

        // Lay out non-legend children below the legend accommodation.
        self.margin_state.borrow_mut().reset();
        let mut bottom_of_lowest_margin_box = CssPixels::default();
        {
            let saved = self.block_offset_of_current_block_container.replace(Some(extra_top));
            for child in self.children(fieldset) {
                if child != legend {
                    self.layout_block_level_box(frame, child, fieldset, &mut bottom_of_lowest_margin_box, child_input);
                }
            }
            self.block_offset_of_current_block_container.set(saved);
        }

        if self.layout_mode == LayoutMode::IntrinsicSizing && !self.used(fieldset).has_definite_inline_size() {
            let mut inline_size = self.greatest_child_inline_size_including_floats(fieldset);
            let style = self.style(fieldset);
            if self.used(fieldset).inline_size_constraint.get() == SizeConstraint::None {
                let sizing = self.sizing();
                if !sizing.should_treat_max_inline_size_as_none(
                    fieldset,
                    available_space.inline_size,
                    input.containing_block_constraints,
                ) {
                    inline_size = inline_size.min(sizing.calculate_inner_inline_size(
                        fieldset,
                        available_space.inline_size,
                        style.max_width(),
                        input.containing_block_constraints,
                    ));
                }
                if !style.min_width().is_auto() {
                    inline_size = inline_size.max(sizing.calculate_inner_inline_size(
                        fieldset,
                        available_space.inline_size,
                        style.min_width(),
                        input.containing_block_constraints,
                    ));
                }
            }
            let used = self.used_mut(fieldset);
            used.set_content_inline_size(inline_size);
            used.set_content_block_size(bottom_of_lowest_margin_box);
        }

        // The element is expected to be positioned in the block-flow direction such that its border box is centered over
        // the border on the block-start side of the fieldset element.
        // FIXME: Take writing modes into consideration.
        let legend_border_box_centering_offset =
            (effective_border - self.used(legend).border_box_block_size(false)) / 2;
        let fieldset_border_box_block_start_in_content =
            -(self.used(fieldset).border_top.get() + self.used(fieldset).padding_top.get());
        let legend_content_block_offset = fieldset_border_box_block_start_in_content
            + legend_border_box_centering_offset
            + self.used(legend).border_box_top(false);
        if let Some(legend_flow_position) = self.pending_legend_flow_position.take() {
            self.place_child(
                legend,
                FfiCssPixelPoint {
                    x: legend_flow_position.inline_offset,
                    y: legend_content_block_offset,
                },
            );
            self.translate_floats_in_subtree(
                legend,
                FfiCssPixelPoint {
                    x: CssPixels::default(),
                    y: legend_content_block_offset - legend_flow_position.block_offset,
                },
            );
        }
        self.compute_and_store_baselines(fieldset);
    }

    fn determine_used_value_for_column_count(&self, used_inline_size: CssPixels) -> Option<i32> {
        let style = self.style(self.root);
        // (01) if ((column-width = auto) and (column-count = auto)) then
        if style.column_width().is_auto() && !style.has_column_count() {
            // (02) exit; /* not a multicol container */
            return None;
        }
        // (03) if column-width = auto then
        if style.column_width().is_auto() {
            // (04) N := column-count
            return Some(style.column_count());
        }
        let column_gap = if style.column_gap().is_normal() {
            style.font_size()
        } else {
            style.column_gap().to_px(used_inline_size)
        };
        let column_width = style
            .column_width()
            .to_px(used_inline_size)
            .max(CssPixels::from_integer(1));
        let denominator = column_width + column_gap;
        let available_count = if denominator > CssPixels::default() {
            ((used_inline_size + column_gap).raw_value() / denominator.raw_value()).max(1)
        } else {
            1
        };
        if style.has_column_count() {
            Some(style.column_count().min(available_count))
        } else {
            Some(available_count)
        }
    }

    pub(crate) fn run(&self, frame: &mut FcFrame<'pass>, input: LayoutInput) {
        let available_space = input.available_space;
        // https://drafts.csswg.org/css-multicol-2/#the-multi-column-model
        let root_inline_size = self.used(self.root).content_inline_size.get();
        if let Some(column_count) = self.determine_used_value_for_column_count(root_inline_size) {
            let style = self.style(self.root);
            let column_gap = if style.column_gap().is_normal() {
                style.font_size()
            } else {
                style.column_gap().to_px(root_inline_size)
            };
            // FIXME: Do multi-column layout.
            let _column_width =
                ((root_inline_size + column_gap) / column_count.max(1) as usize - column_gap).max(CssPixels::default());
        }

        let root_input = LayoutInput {
            content_box_position_in_bfc_root: Some(FfiCssPixelPoint::default()),
            ..input
        };
        let root_facts = self.facts(self.root);
        if root_facts.is_fieldset_box() && !root_facts.rendered_legend().is_invalid() {
            self.layout_fieldset_with_rendered_legend(frame, self.root, root_input);
            return;
        }
        if root_facts.children_are_inline() {
            self.layout_inline_children(frame, self.root, root_input, available_space);
        } else {
            self.layout_block_level_children(frame, self.root, root_input, available_space);
        }

        // Fieldsets without a rendered legend skip collapsed margin assignment.
        if root_facts.is_fieldset_box() {
            return;
        }

        // Assign collapsed margin left after children layout of formatting context to the last child box
        let collapsed_margin = self.margin_state.borrow().current_collapsed_margin();
        if collapsed_margin != CssPixels::default() {
            for child in self.children(self.root).into_iter().rev() {
                let facts = self.facts(child);
                if facts.is_absolutely_positioned() || facts.is_floating() {
                    continue;
                }
                if self.margins_collapse_through(child) {
                    continue;
                }
                self.used_mut(child).margin_bottom.set(collapsed_margin);
                break;
            }
            // The margin reassignment above changed a child's margin box, which the root's baselines may
            // have been derived from (a scroll container child exports its bottom margin edge), so re-derive them.
            self.compute_and_store_baselines(self.root);
        }
    }

    pub(crate) fn parent_context_did_dimension_child_root_box(&self) {
        self.was_notified_after_parent_dimensioned_my_root_box.set(true);
        let floats = self.floats.borrow();
        for &floating_box in floats.iter() {
            // SAFETY: Float records retain stable state-owned used-values pointers.
            let used = floating_box.used_values;
            let content_block_offset =
                floating_box.top_margin_edge + used.margin_top.get() + used.border_box_top(false);
            let inline_offset = if floating_box.side == FloatSide::Left {
                // Left-side floats: offset_from_edge is from left edge (0) to left content edge of floating_box.
                floating_box.offset_from_edge
            } else {
                // Right-side floats: offset_from_edge is from right edge (float_containing_block_inline_size) to the left content edge of floating_box.
                let float_containing_block_inline_size = match used.inline_size_constraint.get() {
                    SizeConstraint::MinContent => CssPixels::default(),
                    // Preserve the MaxContent saturation quirk from the C++ fixed-point subtraction.
                    SizeConstraint::MaxContent => CssPixels::from_raw(i32::MAX),
                    SizeConstraint::None => floating_box.percentage_basis_inline_size.unwrap_or_default(),
                };
                float_containing_block_inline_size - floating_box.offset_from_edge
            };
            self.place_child(
                floating_box.box_,
                FfiCssPixelPoint {
                    x: inline_offset,
                    y: content_block_offset,
                },
            );
        }
    }

    pub(crate) fn was_notified_after_parent_dimensioned_root(&self) -> bool {
        self.was_notified_after_parent_dimensioned_my_root_box.get()
    }

    pub(crate) fn take_pending_table_box_content_offset_in_wrapper(&self) -> Option<LogicalOffset> {
        self.pending_table_box_content_offset_in_wrapper.take()
    }

    pub(crate) fn set_pending_table_box_content_offset_in_wrapper(&self, offset: LogicalOffset) {
        self.pending_table_box_content_offset_in_wrapper.set(Some(offset));
    }

    pub(crate) fn layout_table_caption(
        &self,
        frame: &mut FcFrame<'pass>,
        table_box: Node,
        caption: Node,
        phase: CaptionPhase,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CaptionLayoutResult {
        let mut caption_was_placed = false;
        let mut child_layout = None;
        if formatting_context_type_created_by_box(self.facts(caption)).is_some() {
            let mut inner_available_space = available_space;
            let is_block_context =
                formatting_context_type_created_by_box(self.facts(caption)) == Some(FfiFormattingContextType::Block);
            let mut caption_offset = LogicalOffset::default();
            if is_block_context {
                let available_inline_size = available_space.inline_size.to_px_or_zero();
                self.resolve_vertical_box_model_metrics(caption, available_inline_size);
                self.compute_inline_size(caption, available_space, constraints, FfiCssPixelPoint::default());
                inner_available_space = self
                    .used(caption)
                    .available_inner_space_or_constraints_from(available_space);
                caption_offset.inline_offset = self.used(caption).border_box_left(false);
                if phase == CaptionPhase::Bottom {
                    caption_offset.block_offset = self.used(table_box).margin_box_block_size(false)
                        + self.used(caption).margin_top.get()
                        + self.used(caption).border_box_top(false);
                }
            }

            child_layout = self.layout_inside(
                frame,
                caption,
                LayoutInput {
                    available_space: inner_available_space,
                    containing_block_constraints: constraints,
                    content_box_position_in_bfc_root: None,
                    table_grid_min_border_box_block_size: None,
                },
                true,
            );
            if is_block_context
                && let Some(child_layout) = child_layout.as_ref()
            {
                if self
                    .sizing()
                    .should_treat_block_size_as_auto(caption, available_space, constraints)
                {
                    let content_block_size = if self.style(caption).has_size_containment() {
                        CssPixels::default()
                    } else {
                        child_layout.result().automatic_content_block_size
                    };
                    self.used_mut(caption).set_content_block_size(content_block_size);
                }
                self.place_child(
                    caption,
                    FfiCssPixelPoint {
                        x: caption_offset.inline_offset,
                        y: caption_offset.block_offset,
                    },
                );
                caption_was_placed = true;
            }
        }

        if phase == CaptionPhase::Bottom && !caption_was_placed {
            self.place_child(
                caption,
                FfiCssPixelPoint {
                    x: self.used(caption).border_box_left(false),
                    y: self.used(table_box).margin_box_block_size(false)
                        + self.used(caption).margin_top.get()
                        + self.used(caption).border_box_top(false),
                },
            );
        }
        let result = CaptionLayoutResult {
            margin_box_block_size: self.used(caption).margin_box_block_size(false),
            pending_table_block_offset: if phase == CaptionPhase::Top {
                self.used(caption).content_block_size.get() + self.used(caption).margin_box_bottom(false)
            } else {
                CssPixels::default()
            },
        };
        if let Some(child_layout) = child_layout {
            child_layout.finish();
        }
        result
    }

    pub(crate) fn layout_interrupting_block_inside_inline_context(
        &self,
        frame: &mut FcFrame<'pass>,
        node: Node,
        containing_block: Node,
        input: LayoutInput,
        line_builder: &mut LineBuilder<'_, '_, '_>,
    ) {
        let current_block_offset = line_builder.current_block_offset();
        let saved = self
            .block_offset_of_current_block_container
            .replace(Some(current_block_offset));
        let mut dummy_bottom = CssPixels::default();
        self.layout_block_level_box(frame, node, containing_block, &mut dummy_bottom, input);
        // SAFETY: The builder remains live and no reference escaped.
        let block_bottom = self
            .block_offset_of_current_block_container
            .get()
            .unwrap_or_else(|| line_builder.current_block_offset());
        self.block_offset_of_current_block_container.set(saved);
        line_builder.append_block_level_box(
            node,
            block_bottom,
            self.margin_state.borrow().current_collapsed_margin(),
        );
    }

    fn margin_box_left_of_float_record(&self, floating_box: FloatingBox) -> CssPixels {
        Self::margin_box_left_of_float_in_root(
            floating_box,
            floating_box.containing_block_rect_in_root_coordinate_space,
        )
    }

    pub(crate) fn layout_floating_box(
        &self,
        frame: &mut FcFrame<'pass>,
        node: Node,
        block_container: Node,
        input: LayoutInput,
        block_offset: CssPixels,
        mut line_builder: Option<&mut LineBuilder<'_, '_, '_>>,
    ) {
        let available_space = input.available_space;
        assert!(self.facts(node).is_floating());
        let block_container_inline_size = self.used(block_container).content_inline_size.get();
        self.resolve_vertical_box_model_metrics(node, block_container_inline_size);
        let containing_block_rect = self.containing_block_rect(
            block_container,
            input
                .content_box_position_in_bfc_root
                .expect("float layout requires its containing block position in the BFC root"),
        );
        let containing_block_rect_now = containing_block_rect.translated(
            CssPixels::default(),
            self.block_offset_adjustment_from_pending_ancestor_block_start_margins(block_container),
        );
        self.compute_inline_size(
            node,
            available_space,
            input.containing_block_constraints,
            FfiCssPixelPoint {
                x: containing_block_rect_now.x,
                y: containing_block_rect_now.y,
            },
        );
        self.resolve_used_block_size_if_not_treated_as_auto(node, available_space, input.containing_block_constraints);
        let facts = self.facts(node);
        if facts.has_auto_content_box_size() || self.style(node).display().is_flex_inside() {
            self.resolve_used_block_size_if_treated_as_auto(
                node,
                available_space,
                input.containing_block_constraints,
                None,
            );
        }
        let inner = self
            .used(node)
            .available_inner_space_or_constraints_from(available_space);
        let child_layout = self.layout_inside(
            frame,
            node,
            LayoutInput {
                available_space: inner,
                containing_block_constraints: input.containing_block_constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
            false,
        );
        // A floating table wrapper shrink-to-fits from cached intrinsic sizes, which may not match
        // the inline size table layout just produced; the wrapper has the same inline size as the table grid box.
        if facts.is_table_wrapper()
            && let Some(child_layout) = child_layout.as_ref()
        {
            self.used_mut(node)
                .set_content_inline_size(child_layout.result().automatic_content_inline_size);
        }
        self.resolve_used_block_size_if_treated_as_auto(
            node,
            available_space,
            input.containing_block_constraints,
            child_layout
                .as_ref()
                .map(|child_layout| child_layout.result().automatic_content_block_size),
        );

        // Next, float to the left and/or right
        // FIXME: Honor writing-mode, direction and text-orientation.
        let style = self.style(node);
        let side = if matches!(style.float_(), float::LEFT | float::INLINE_START) {
            Some(FloatSide::Left)
        } else if matches!(style.float_(), float::RIGHT | float::INLINE_END) {
            Some(FloatSide::Right)
        } else {
            None
        };
        let Some(side) = side else {
            if let Some(child_layout) = child_layout {
                child_layout.finish();
            }
            return;
        };
        let mut margin_box_ceiling = if let Some(line_builder) = line_builder.as_deref_mut() {
            line_builder.ceiling_for_float_to_be_inserted_here(node)
        } else {
            block_offset
        };
        if side == FloatSide::Left && matches!(style.clear(), clear::LEFT | clear::BOTH | clear::INLINE_START) {
            margin_box_ceiling =
                margin_box_ceiling.max(self.lowest_left_margin_edge.get() - containing_block_rect_now.y);
        }
        if side == FloatSide::Right && matches!(style.clear(), clear::RIGHT | clear::BOTH | clear::INLINE_END) {
            margin_box_ceiling =
                margin_box_ceiling.max(self.lowest_right_margin_edge.get() - containing_block_rect_now.y);
        }
        let mut ceiling_in_root = containing_block_rect_now.y + margin_box_ceiling;
        if let Some(last) = self.floats.borrow().last().copied() {
            ceiling_in_root = ceiling_in_root.max(
                last.margin_box_rect_in_root_coordinate_space.y
                    + self.block_offset_adjustment_from_pending_ancestor_block_start_margins(last.box_),
            );
        }
        let placement = self.place_float(
            side,
            self.used(node),
            available_space,
            containing_block_rect_now,
            ceiling_in_root,
        );
        let content_block_offset = placement.block_start - containing_block_rect_now.y
            + self.used(node).margin_top.get()
            + self.used(node).border_box_top(false);
        let mut margin_box_rect = Self::margin_box_rect(self.used(node))
            .translated(CssPixels::default(), content_block_offset)
            .translated(containing_block_rect.x, containing_block_rect.y);
        let floating_box = FloatingBox {
            box_: node,
            used_values: self.used_pointer(node),
            side,
            offset_from_edge: placement.offset_from_edge,
            top_margin_edge: content_block_offset
                - self.used(node).margin_top.get()
                - self.used(node).border_box_top(false),
            bottom_margin_edge: content_block_offset
                + self.used(node).content_block_size.get()
                + self.used(node).margin_box_bottom(false),
            margin_box_rect_in_root_coordinate_space: margin_box_rect,
            containing_block_rect_in_root_coordinate_space: containing_block_rect,
            percentage_basis_inline_size: input.containing_block_constraints.percentage_basis_inline_size,
        };
        margin_box_rect.x = self.margin_box_left_of_float_record(floating_box);
        let mut floating_box = floating_box;
        floating_box.margin_box_rect_in_root_coordinate_space = margin_box_rect;
        self.floats.borrow_mut().push(floating_box);
        self.add_float_to_bands(floating_box, containing_block_rect);

        let bottom_margin_edge = margin_box_rect.bottom();
        let root_slot_index = self.callbacks.slot_index(self.root);
        let mut rare = self.state.block_rare_data_mut(root_slot_index);
        rare.lowest_floating_descendant_bottom_margin_edge = Some(
            rare.lowest_floating_descendant_bottom_margin_edge
                .map_or(bottom_margin_edge, |lowest| lowest.max(bottom_margin_edge)),
        );
        drop(rare);
        if let Some(line_builder) = line_builder {
            line_builder.recalculate_available_space();
        }
        let block_container_used = self.used(block_container);
        self.compute_inset(
            node,
            LogicalSize {
                inline_size: block_container_used.content_inline_size.get(),
                block_size: block_container_used.content_block_size.get(),
            },
        );
        if let Some(child_layout) = child_layout {
            child_layout.finish();
        }
    }

    fn layout_inline_children(
        &self,
        frame: &mut FcFrame<'pass>,
        block_container: Node,
        input: LayoutInput,
        available_space_for_children: AvailableSpace,
    ) {
        assert!(self.facts(block_container).children_are_inline());
        let inline_input = self.child_layout_input(block_container, input, available_space_for_children);
        let mut context = InlineFormattingContext::new_with_rust_parent(
            frame,
            self.state,
            block_container,
            self.layout_mode,
            inline_input,
            self.callbacks,
            self,
        );
        context.run();
        let automatic_inline_size = context.automatic_content_inline_size;
        let automatic_block_size = context.automatic_content_block_size;
        if !self.used(block_container).has_definite_inline_size() {
            // NOTE: min-width or max-width for boxes with inline children can only be applied after inside layout
            //       is done and the inline size of the box content is known
            let mut used_inline_size = automatic_inline_size;
            // NOTE: Min and max constraints are not applied to a box that is being sized under an intrinsic
            //       sizing constraint: per css-sizing-3, min/max-width affect a box's intrinsic size
            //       *contributions*, and the callers of calculate_{min,max}_content_inline_size() apply them.
            //       Applying them here would bake the box's own min/max-width into its measured intrinsic
            //       size, and the border-box adjustment would consume border/padding that measurement
            //       state does not have.
            if self.used(block_container).inline_size_constraint.get() == SizeConstraint::None {
                // https://www.w3.org/TR/css-sizing-3/#sizing-values
                // Percentages are resolved against the appropriate inline or block size of the containing block.
                let containing_inline_size = input.containing_block_constraints.inline_basis();
                let available_inline_size = AvailableSize::definite(containing_inline_size);
                let sizing = self.sizing();
                let style = self.style(block_container);
                if !sizing.should_treat_max_inline_size_as_none(
                    block_container,
                    input.available_space.inline_size,
                    input.containing_block_constraints,
                ) {
                    let maximum = sizing.calculate_inner_inline_size(
                        block_container,
                        available_inline_size,
                        style.max_width(),
                        input.containing_block_constraints,
                    );
                    used_inline_size = used_inline_size.min(maximum);
                }
                let min_is_auto = style.min_width().is_auto()
                    || (style.min_width().is_fit_content()
                        && input.available_space.inline_size.is_intrinsic_sizing_constraint())
                    || (style.min_width().is_max_content() && input.available_space.inline_size == AvailableSize::MaxContent)
                    || (style.min_width().is_min_content() && input.available_space.inline_size == AvailableSize::MinContent);
                if !min_is_auto {
                    let minimum = sizing.calculate_inner_inline_size(
                        block_container,
                        available_inline_size,
                        style.min_width(),
                        input.containing_block_constraints,
                    );
                    used_inline_size = used_inline_size.max(minimum);
                }
            }
            let used = self.used_mut(block_container);
            used.set_content_inline_size(used_inline_size);
            used.set_content_block_size(automatic_block_size);
        }
    }

    fn compute_automatic_block_size_for_block_level_element(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let facts = self.facts(node);
        if facts.creates_block_formatting_context() {
            return automatic_block_size_for_bfc_root(self.state, self.callbacks, node);
        }
        let style = self.style(node);
        let sizing = self.sizing();
        if style.display().is_flex_inside() {
            // https://drafts.csswg.org/css-flexbox-1/#algo-main-container
            // NOTE: The automatic block size of a block-level flex container is its max-content size.
            return sizing.calculate_max_content_block_size(
                node,
                available_space.inline_size.to_px_or_zero(),
                constraints,
            );
        }
        if style.display().is_grid_inside() {
            // https://www.w3.org/TR/css-grid-2/#intrinsic-sizes
            // In both inline and block formatting contexts, the grid container’s auto block size is its
            // max-content size.
            return sizing.calculate_max_content_block_size(
                node,
                available_space.inline_size.to_px_or_zero(),
                constraints,
            );
        }
        if style.display().is_table_inside() {
            return sizing.calculate_max_content_block_size(
                node,
                available_space.inline_size.to_px_or_zero(),
                constraints,
            );
        }

        // https://www.w3.org/TR/CSS22/visudet.html#normal-block
        // 10.6.3 Block-level non-replaced elements in normal flow when 'overflow' computes to 'visible'

        // The element's block size is the distance from its block-start content edge to the first applicable edge below.
        // 1. the bottom edge of the last line box, if the box establishes a inline formatting context with one or more lines
        if facts.children_are_inline() {
            let line_data = self.state.line_data(self.callbacks.slot_index(node));
            if let Some(last_line) = line_data.as_deref().and_then(|data| data.line_boxes.last()) {
                let mut block_size = last_line.physical_vertical_end();
                if last_line.has_block_level_box {
                    let mut margin_state = self.margin_state.borrow_mut();
                    let mut margin_bottom = margin_state.current_collapsed_margin();
                    let used = self.used(node);
                    if used.padding_bottom.get() == CssPixels::default()
                        && used.border_bottom.get() == CssPixels::default()
                    {
                        margin_state.box_last_in_flow_child_margin_bottom_collapsed = true;
                        margin_bottom = CssPixels::default();
                    }
                    block_size = (block_size + margin_bottom).max(CssPixels::default());
                }
                return block_size;
            }
        }

        // 2. the bottom edge of the bottom (possibly collapsed) margin of its last in-flow child, if the child's bottom margin does not collapse with the element's bottom margin
        // 3. the bottom border edge of the last in-flow child whose top margin doesn't collapse with the element's bottom margin
        if !facts.children_are_inline() {
            let mut marker_line_block_size = CssPixels::default();
            for child in self.children(node).into_iter().rev() {
                let child_facts = self.facts(child);
                if child_facts.is_absolutely_positioned() || child_facts.is_floating() {
                    continue;
                }
                // NOTE: Markers are not in-flow, but for list items that contain only floats (or are otherwise empty),
                //       the marker's line-height determines the list item's block size. This ensures proper stacking of
                //       list items and alignment with their floated content.
                if child_facts.is_list_item_marker_box() {
                    marker_line_block_size = self.style(child).line_height();
                    continue;
                }
                if self.margins_collapse_through(child) {
                    continue;
                }
                let child_used = self.used(child);
                let mut margin_state = self.margin_state.borrow_mut();
                let mut margin_bottom = margin_state.current_collapsed_margin();
                let used = self.used(node);
                if used.padding_bottom.get() == CssPixels::default() && used.border_bottom.get() == CssPixels::default()
                {
                    margin_state.box_last_in_flow_child_margin_bottom_collapsed = true;
                    margin_bottom = CssPixels::default();
                }
                return (child_used.content_offset.get().y
                    + child_used.content_block_size.get()
                    + child_used.border_box_bottom(false)
                    + margin_bottom)
                    .max(CssPixels::default());
            }
            // If no in-flow children were found but there's a marker, use the marker's line-height.
            if marker_line_block_size > CssPixels::default() {
                return marker_line_block_size;
            }
        }

        // AD-HOC: Contenteditable elements must have a minimum block size (line-height) when empty, to remain clickable
        //         and usable for text input, even though this is not specified.
        //         See: https://github.com/w3c/editing/issues/70.
        if facts.is_editing_host() {
            return style.line_height();
        }
        // 4. zero, otherwise
        CssPixels::default()
    }

    pub(crate) fn greatest_child_inline_size_including_floats(&self, node: Node) -> CssPixels {
        // Similar to the former C++ greatest_child_inline_size() helper.
        // but this one takes floats into account!
        let mut max_inline_size = CssPixels::default();

        // https://drafts.csswg.org/css2/#floats
        // A line box is next to a float when there exists a vertical position that satisfies all of these
        // four conditions: (a) at or below the top of the line box, (b) at or above the bottom of the line
        // box, (c) below the top margin edge of the float, and (d) above the bottom margin edge of the float.
        let line_box_is_next_to_float = |block_start: CssPixels, block_end: CssPixels, floating_box: &FloatingBox| {
            block_start < floating_box.bottom_margin_edge && block_end > floating_box.top_margin_edge
        };
        let inline_size_to_make_room_for_float_margin_box = |floating_box: &FloatingBox| {
            // SAFETY: Float records retain stable state-owned used-values pointers.
            let used = floating_box.used_values;
            if floating_box.side == FloatSide::Left {
                floating_box.offset_from_edge
                    + used.content_inline_size.get()
                    + used.margin_right.get()
                    + used.border_box_right(false)
            } else {
                floating_box.offset_from_edge + used.margin_left.get() + used.border_box_left(false)
            }
        };

        let floats = self.floats.borrow();
        // https://drafts.csswg.org/css-sizing-3/#intrinsic-contribution
        // A box’s min-content contribution/max-content contribution in each axis is the size of the
        // content box of a hypothetical width/auto-sized float that contains only that box
        //
        // Only direct floats participate here. Descendant floats contribute through their containing block's
        // own min-content contribution, not as if they belonged to this box's hypothetical float.
        for candidate in floats
            .iter()
            .filter(|floating_box| self.containing_block(floating_box.box_) == node)
        {
            let mut inline_space = SpaceUsedByFloats::default();
            for direct_float in floats
                .iter()
                .filter(|floating_box| self.containing_block(floating_box.box_) == node)
            {
                if line_box_is_next_to_float(candidate.top_margin_edge, candidate.bottom_margin_edge, direct_float) {
                    let inline_size = inline_size_to_make_room_for_float_margin_box(direct_float);
                    if direct_float.side == FloatSide::Left {
                        inline_space.left = inline_space.left.max(inline_size);
                    } else {
                        inline_space.right = inline_space.right.max(inline_size);
                    }
                }
            }
            max_inline_size = max_inline_size.max(inline_space.left + inline_space.right);
        }

        let facts = self.facts(node);
        if facts.children_are_inline() {
            if let Some(data) = self.state.line_data(self.callbacks.slot_index(node)) {
                for line in &data.line_boxes {
                    let mut inline_size_here = crate::layout::line_physical_horizontal_extent(line);
                    let line_block_start = line.physical_vertical_end() - line.physical_vertical_extent();
                    let line_block_end = line.physical_vertical_end();
                    let mut extra_left = CssPixels::default();
                    for left_float in floats.iter().filter(|floating_box| {
                        floating_box.side == FloatSide::Left && self.containing_block(floating_box.box_) == node
                    }) {
                        // NOTE: Floats directly affect the automatic size of their containing block, but only indirectly anything above in the tree.
                        if line_box_is_next_to_float(line_block_start, line_block_end, left_float) {
                            extra_left = extra_left.max(inline_size_to_make_room_for_float_margin_box(left_float));
                        }
                    }
                    let mut extra_right = CssPixels::default();
                    for right_float in floats.iter().filter(|floating_box| {
                        floating_box.side == FloatSide::Right && self.containing_block(floating_box.box_) == node
                    }) {
                        // NOTE: Floats directly affect the automatic size of their containing block, but only indirectly anything above in the tree.
                        if line_box_is_next_to_float(line_block_start, line_block_end, right_float) {
                            extra_right = extra_right.max(inline_size_to_make_room_for_float_margin_box(right_float));
                        }
                    }
                    inline_size_here += extra_left + extra_right;
                    max_inline_size = max_inline_size.max(inline_size_here);
                }
            }
        } else {
            drop(floats);
            for child in self.children(node) {
                if self.facts(child).is_absolutely_positioned() {
                    continue;
                }
                if let Some(child_used) = self.try_used_pointer(child) {
                    max_inline_size = max_inline_size.max(child_used.margin_box_inline_size(false));
                }
            }
        }
        max_inline_size
    }

    pub(crate) fn automatic_content_inline_size(&self) -> CssPixels {
        let root_facts = self.facts(self.root);
        if root_facts.children_are_inline() {
            return self.used(self.root).content_inline_size.get();
        }
        if root_facts.is_table_wrapper() {
            let mut stack = Vec::new();
            let mut child = self.first_child(self.root);
            while !child.is_invalid() {
                stack.push(child);
                child = self.next_sibling(child);
            }
            stack.reverse();
            while let Some(node) = stack.pop() {
                let facts = self.facts(node);
                if facts.is_box() && self.style(node).display().is_table_inside() {
                    return self.used(node).border_box_inline_size(false);
                }
                let mut children = Vec::new();
                let mut child = self.first_child(node);
                while !child.is_invalid() {
                    children.push(child);
                    child = self.next_sibling(child);
                }
                for child in children.into_iter().rev() {
                    stack.push(child);
                }
            }
            unreachable!("a table wrapper contains its table box");
        }
        self.greatest_child_inline_size_including_floats(self.root)
    }

    pub(crate) fn automatic_content_block_size(&self) -> CssPixels {
        automatic_block_size_for_bfc_root(self.state, self.callbacks, self.root)
    }
}

// https://www.w3.org/TR/CSS22/visudet.html#root-height
pub(crate) fn automatic_block_size_for_bfc_root(
    state: &LayoutState,
    callbacks: FfiLayoutFcCallbacks,
    root: Node,
) -> CssPixels {
    let facts = state.node_facts(&callbacks, root);
    let used = state.used_values(&callbacks, root);
    let mut bottom = None;
    if facts.children_are_inline() {
        // If it only has inline-level children, the block size is the distance between
        // the top content edge and the bottom of the bottommost line box.
        let line_data = state.line_data(callbacks.slot_index(root));
        if let Some(last_line) = line_data.as_ref().and_then(|data| data.line_boxes.last()) {
            bottom = Some(last_line.physical_vertical_end());
            // A trailing interrupting block's bottom margin cannot collapse out of a BFC root,
            // so it contributes to the root's automatic block size. The line box bottom excludes it.
            if last_line.has_block_level_box {
                bottom = Some((bottom.unwrap() + last_line.block_level_box_block_end_margin).max(CssPixels::default()));
            }
        }
    } else {
        // If it has block-level children, the block size is the distance between
        // the top margin-edge of the topmost block-level child box
        // and the bottom margin-edge of the bottommost block-level child box.
        // NOTE: The top margin edge of the topmost block-level child box is the same as the top content edge of the root box.
        let mut child = callbacks.first_child(root);
        while !child.is_invalid() {
            let child_facts = state.node_facts(&callbacks, child);
            if child_facts.is_box() && !child_facts.is_absolutely_positioned() && !child_facts.is_floating() {
                let child_used = state.try_used_values(&callbacks, child);
                // Children that have not been laid out yet contribute nothing to the automatic block size.
                if let Some(child_used) = child_used {
                    let child_bottom = child_used.content_offset.get().y
                        + child_used.content_block_size.get()
                        + child_used.margin_box_bottom(false);
                    bottom = Some(bottom.map_or(child_bottom, |value: CssPixels| value.max(child_bottom)));
                }
            }
            child = callbacks.next_sibling(child);
        }
    }
    // In addition, if the element has any floating descendants
    // whose bottom margin edge is below the element's bottom content edge,
    // then the block size is increased to include those edges.
    if let Some(lowest) = state
        .block_rare_data(callbacks.slot_index(root))
        .and_then(|rare| rare.lowest_floating_descendant_bottom_margin_edge)
    {
        bottom = Some(bottom.map_or(lowest, |value| value.max(lowest)));
    }
    let _ = used;
    bottom.unwrap_or_default().max(CssPixels::default())
}
