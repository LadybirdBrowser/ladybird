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

#[derive(Clone, Copy, Debug, PartialEq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiAnchorSideKind {
    Invalid,
    Top,
    Right,
    Bottom,
    Left,
    Center,
    Start,
    End,
    SelfStart,
    SelfEnd,
    Inside,
    Outside,
    Percentage,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiAnchorFunctionFacts {
    pub has_anchor_name: bool,
    pub anchor_name: usize,
    pub side_kind: FfiAnchorSideKind,
    pub side_percentage: f64,
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u8)]
// NB: Some variants are only constructed by C++ through the FFI.
#[allow(dead_code)]
pub enum FfiAnchorFallbackKind {
    None,
    Px,
    Percentage,
    Calculated,
    Anchor,
}

#[derive(Clone, Copy, Debug)]
#[repr(C)]
pub struct FfiAnchorFallbackFacts {
    pub kind: FfiAnchorFallbackKind,
    pub px: CssPixels,
    pub fraction: f64,
    pub value: *const c_void,
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
struct LineFragmentFacts {
    layout_node: Node,
    is_atomic_inline: bool,
    writing_mode: u8,
    style_block_axis_is_reverse: bool,
    inline_offset: CssPixels,
    block_offset: CssPixels,
    offset: FfiCssPixelPoint,
    size: FfiCssPixelPoint,
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

    fn is_empty(self) -> bool {
        self.width <= CssPixels::default() || self.height <= CssPixels::default()
    }

    fn translated(self, offset: FfiCssPixelPoint) -> Self {
        Self {
            x: self.x + offset.x,
            y: self.y + offset.y,
            ..self
        }
    }

    fn union(self, other: Self) -> Self {
        let left = self.left().min(other.left());
        let top = self.top().min(other.top());
        let right = self.right().max(other.right());
        let bottom = self.bottom().max(other.bottom());
        Self {
            x: left,
            y: top,
            width: right - left,
            height: bottom - top,
        }
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
        let rust_state = self.rust_state();
        let fc_type = crate::layout::independent_formatting_context_type(rust_state, node, &self.callbacks);
        let mut context = crate::layout::create_formatting_context(
            rust_state,
            node,
            crate::layout::FcParents::default(),
            fc_type,
            layout_mode,
            false,
            self.callbacks,
        );
        crate::layout::run_formatting_context(&mut context, input, None);
        complete_formatting_context_after_root_box_has_used_size(&mut context);
        crate::layout::ChildLayoutResult {
            automatic_content_inline_size: context.automatic_content_inline_size,
            automatic_content_block_size: context.automatic_content_block_size,
        }
    }

    pub(crate) fn rust_state(&self) -> &LayoutState {
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

pub(crate) struct SizingContext<'pass> {
    state: &'pass LayoutState,
    callbacks: FfiLayoutFcCallbacks,
}

impl<'pass> SizingContext<'pass> {
    pub(crate) fn new(state: &'pass LayoutState, callbacks: FfiLayoutFcCallbacks) -> Self {
        Self { state, callbacks }
    }

    fn facts(&self, node: Node) -> NodeFacts<'_> {
        self.state.node_facts(&self.callbacks, node)
    }

    fn style(&self, node: Node) -> StyleValues<'pass> {
        self.state.style_facts(&self.callbacks, node)
    }

    fn used(&self, node: Node) -> &'pass UsedValues {
        self.state.used_values(&self.callbacks, node)
    }

    fn used_mut(&self, node: Node) -> &'pass UsedValues {
        self.state.used_values(&self.callbacks, node)
    }

    fn parent(&self, node: Node) -> Node {
        self.callbacks.parent(node)
    }

    fn first_child(&self, node: Node) -> Node {
        self.callbacks.first_child(node)
    }

    fn next_sibling(&self, node: Node) -> Node {
        self.callbacks.next_sibling(node)
    }

    fn has_children(&self, node: Node) -> bool {
        !self.callbacks.first_child(node).is_invalid()
    }

    fn content_block_size_from_aspect_ratio(&self, node: Node, content_inline_size: CssPixels) -> CssPixels {
        let style = self.style(node);
        let used = self.used(node);
        content_block_size_from_aspect_ratio_values(
            content_inline_size,
            self.facts(node).preferred_aspect_ratio().unwrap(),
            style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
            style.border_left_width() + used.padding_left.get(),
            style.border_right_width() + used.padding_right.get(),
            style.border_top_width() + used.padding_top.get(),
            style.border_bottom_width() + used.padding_bottom.get(),
        )
    }

    fn content_inline_size_from_aspect_ratio(&self, node: Node, content_block_size: CssPixels) -> CssPixels {
        let style = self.style(node);
        let used = self.used(node);
        content_inline_size_from_aspect_ratio_values(
            content_block_size,
            self.facts(node).preferred_aspect_ratio().unwrap(),
            style.box_sizing_for_aspect_ratio() == box_sizing::BORDER_BOX,
            style.border_left_width() + used.padding_left.get(),
            style.border_right_width() + used.padding_right.get(),
            style.border_top_width() + used.padding_top.get(),
            style.border_bottom_width() + used.padding_bottom.get(),
        )
    }

    fn auto_content_size(&self, node: Node) -> ReplacedIntrinsicSize {
        let facts = self.facts(node);
        ReplacedIntrinsicSize {
            width: facts.has_auto_content_width().then_some(facts.auto_content_width()),
            height: facts.has_auto_content_height().then_some(facts.auto_content_height()),
            aspect_ratio: facts.has_auto_content_aspect_ratio().then_some(PixelFraction {
                numerator: facts.auto_content_aspect_ratio_numerator(),
                denominator: facts.auto_content_aspect_ratio_denominator(),
            }),
        }
    }

    fn intrinsic_size_for_replaced_sizing(&self, node: Node) -> ReplacedIntrinsicSize {
        let auto_size = self.auto_content_size(node);
        if auto_size.width.is_some() || auto_size.height.is_some() || auto_size.aspect_ratio.is_some() {
            return auto_size;
        }
        let facts = self.facts(node);
        // https://drafts.csswg.org/css-ui-4/#appearance-switching
        // The element is rendered following the usual rules of CSS. Replaced elements other than widgets are not affected
        // by this and remain replaced elements. Widgets must not have their native appearance, and instead must have their
        // primitive appearance.
        //
        // https://html.spec.whatwg.org/multipage/rendering.html#the-input-element-as-a-text-entry-widget
        // An input element whose type attribute is in one of the above states is an element with default preferred size,
        // and user agents are expected to apply the 'field-sizing' CSS property to the element.
        ReplacedIntrinsicSize {
            width: facts
                .has_default_preferred_width()
                .then_some(facts.default_preferred_width()),
            height: facts
                .has_default_preferred_height()
                .then_some(facts.default_preferred_height()),
            aspect_ratio: None,
        }
    }

    fn max_content_size_for_replaced_element_without_natural_size(
        &self,
        node: Node,
        natural_size: ReplacedIntrinsicSize,
        dimension: SizeDimension,
        constraints: ReplacedMaxContentSizeConstraints,
    ) -> Option<CssPixels> {
        // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        // the intrinsic sizes of replaced elements without natural sizes are defined below:
        let facts = self.facts(node);
        let is_inline_axis = dimension == SizeDimension::Inline;
        if !facts.is_replaced_box()
            || if is_inline_axis {
                natural_size.width.is_some()
            } else {
                natural_size.height.is_some()
            }
        {
            return None;
        }

        // SVG Integration says that a non-top-level <svg> starts with auto width/height, and that with a viewBox, missing
        // width/height attributes "keep" their auto value. The resulting width, height, and aspect ratio are then
        // "used in CSS sizing as intrinsic element size properties".
        //
        // CSS Sizing defines max-content as the size the box would have "if it was a float" with an auto preferred size.
        // CSS2 replaced sizing then resolves auto width from "(used height) * (intrinsic ratio)", and auto height from
        // "(used width) / (intrinsic ratio)". Keep this SVG specific bridge before falling through to CSS Sizing's fallback
        // for replaced elements without natural sizes.
        //  - https://svgwg.org/specs/integration/#svg-css-sizing
        //  - https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        //  - https://drafts.csswg.org/css2/#inline-replaced-width
        //  - https://drafts.csswg.org/css2/#inline-replaced-height
        if facts.is_svg_svg_box()
            && let Some(ratio) = natural_size.aspect_ratio
        {
            if is_inline_axis {
                if let Some(height) = natural_size.height {
                    return Some(ratio.multiply(height));
                }
            } else if let Some(width) = natural_size.width {
                return Some(ratio.divide(width));
            }
        }

        // For the max-content size:
        // If it has a preferred aspect ratio:
        if facts.has_preferred_aspect_ratio() {
            if let Some(size) = constraints.definite_size_in_ratio_determining_axis {
                // If the available space is definite in the inline axis, use the stretch fit into that size for the inline size
                // and calculate the block size using the aspect ratio.
                //
                // NB: This helper is only for the max-content size, which has no definite available inline size. Callers may
                //     still know a definite used size in the opposite axis when the box lacks a natural size in that axis.
                return Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(node, size)
                } else {
                    self.content_block_size_from_aspect_ratio(node, size)
                });
            }

            let style = self.style(node);
            // Otherwise if the box has a <length> as its computed value for min-width or min-height, use that size and
            // calculate the other dimension using the aspect ratio; if both dimensions have a <length> minimum, choose the
            // one that results in the larger overall size.
            //
            // NOTE: This case was previous calculated from a 300x150 default size, rather than the box’s min size. This is
            //       believed to be a better behavior, and likely to be Web-compatible, but please send feedback to the CSSWG
            //       if there are any problems.
            let size_from_min_inline = if let Some(inline_size) = constraints.minimum_inline_size {
                Some(if is_inline_axis {
                    inline_size
                } else {
                    self.content_block_size_from_aspect_ratio(node, inline_size)
                })
            } else if style.min_width().is_length_percentage() && !style.min_width().contains_percentage {
                let inline_size = style.min_width().to_px(CssPixels::default());
                Some(if is_inline_axis {
                    inline_size
                } else {
                    self.content_block_size_from_aspect_ratio(node, inline_size)
                })
            } else {
                None
            };
            let size_from_min_block = if let Some(block_size) = constraints.minimum_block_size {
                Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(node, block_size)
                } else {
                    block_size
                })
            } else if style.min_height().is_length_percentage() && !style.min_height().contains_percentage {
                let block_size = style.min_height().to_px(CssPixels::default());
                Some(if is_inline_axis {
                    self.content_inline_size_from_aspect_ratio(node, block_size)
                } else {
                    block_size
                })
            } else {
                None
            };

            return match (size_from_min_inline, size_from_min_block) {
                (Some(inline), Some(block)) => Some(inline.max(block)),
                (Some(inline), None) => Some(inline),
                (None, Some(block)) => Some(block),
                (None, None) => Some(if is_inline_axis {
                    // Otherwise use an inline size matching the corresponding dimension of the initial containing block and calculate
                    // the other dimension using the aspect ratio.
                    //
                    // NOTE: This author-controllable behavior is made possible by the new auto value for the min size properties.
                    //       This is believed to be a better behavior, but it is not yet clear if it is Web-compatible, so please
                    //       send feedback to the CSSWG if there are any problems.
                    facts.initial_containing_block_inline_size()
                } else {
                    self.content_block_size_from_aspect_ratio(node, facts.initial_containing_block_inline_size())
                }),
            };
        }

        // If it has no preferred aspect ratio:
        // For both the min-content size and max-content size:
        // If the box has a <length> as its computed minimum size (min-width/min-height) in that dimension, use that size.
        let min_size = if is_inline_axis {
            self.style(node).min_width()
        } else {
            self.style(node).min_height()
        };
        if min_size.is_length_percentage() && !min_size.contains_percentage {
            return Some(min_size.to_px(CssPixels::default()));
        }
        // Otherwise, use 300px for the width and/or 150px for the height as needed.
        Some(CssPixels::from_integer(if is_inline_axis { 300 } else { 150 }))
    }

    fn tentative_inline_size_for_replaced_element(
        &self,
        node: Node,
        computed_inline_size: FfiSizeValue,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.3.2 Inline, replaced elements, https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-width
        // Treat percentages of indefinite containing block widths as 0 (the initial width).
        if computed_inline_size.is_percentage() && constraints.percentage_basis_inline_size.is_none() {
            return CssPixels::default();
        }
        let style = self.style(node);
        let computed_block_size = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            FfiSizeValue::auto_value()
        } else {
            style.height()
        };
        let used_inline_size = if computed_inline_size.is_auto() {
            computed_inline_size.to_px(available_space.inline_size.to_px_or_zero())
        } else {
            self.calculate_inner_inline_size(node, available_space.inline_size, computed_inline_size, constraints)
        };
        // If 'height' and 'width' both have computed values of 'auto' and the element also has an intrinsic width,
        // then that intrinsic width is the used value of 'width'.
        let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
        if computed_block_size.is_auto()
            && computed_inline_size.is_auto()
            && let Some(width) = intrinsic.width
        {
            return width;
        }
        // If 'height' and 'width' both have computed values of 'auto' and the element has no intrinsic width,
        // but does have an intrinsic height and intrinsic ratio;
        // or if 'width' has a computed value of 'auto',
        // 'height' has some other computed value, and the element does have an intrinsic ratio; then the used value of 'width' is:
        //
        //     (used height) * (intrinsic ratio)
        let has_ratio = self.facts(node).has_preferred_aspect_ratio();
        if (computed_block_size.is_auto()
            && computed_inline_size.is_auto()
            && intrinsic.width.is_none()
            && intrinsic.height.is_some()
            && has_ratio)
            || (computed_inline_size.is_auto() && !computed_block_size.is_auto() && has_ratio)
        {
            let block_size = self.compute_block_size_for_replaced_element(node, available_space, constraints);
            return self.content_inline_size_from_aspect_ratio(node, block_size);
        }
        // If 'height' and 'width' both have computed values of 'auto' and the element has an intrinsic ratio but no intrinsic height or width,
        // then the used value of 'width' is undefined in CSS 2.2. However, it is suggested that, if the containing block's width does not itself
        // depend on the replaced element's width, then the used value of 'width' is calculated from the constraint equation used for block-level,
        // non-replaced elements in normal flow.
        if computed_block_size.is_auto()
            && computed_inline_size.is_auto()
            && intrinsic.width.is_none()
            && intrinsic.height.is_none()
            && has_ratio
        {
            if !available_space.inline_size.is_intrinsic_sizing_constraint() {
                return self.calculate_stretch_fit_inline_size(node, available_space.inline_size);
            }
            match cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                style.width().contains_percentage,
                available_space.inline_size,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    return CssPixels::default();
                }
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => {}
                CyclicPercentageIntrinsicContribution::NotCyclic => {
                    return self.calculate_stretch_fit_inline_size(node, available_space.inline_size);
                }
            }
        }
        // Otherwise, if 'width' has a computed value of 'auto', and the element has an intrinsic width, then that intrinsic width is the used value of 'width'.
        //
        // Otherwise, if 'width' has a computed value of 'auto', but none of the conditions above are met, then the used value of 'width' becomes 300px.
        // If 300px is too wide to fit the device, UAs should use the width of the largest rectangle that has a 2:1 ratio and fits the device instead.
        if computed_inline_size.is_auto() {
            if let Some(width) = intrinsic.width {
                return width;
            }
            return CssPixels::from_integer(300);
        }
        used_inline_size
    }

    fn tentative_block_size_for_replaced_element(
        &self,
        node: Node,
        computed_block_size: FfiSizeValue,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.6.2 Inline replaced elements, block-level replaced elements in normal flow, 'inline-block' replaced elements in normal flow and floating replaced elements
        // https://www.w3.org/TR/CSS22/visudet.html#inline-replaced-height
        let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
        // If 'height' and 'width' both have computed values of 'auto' and the element also has
        // an intrinsic height, then that intrinsic height is the used value of 'height'.
        if self.should_treat_inline_size_as_auto(node, available_space)
            && self.should_treat_block_size_as_auto(node, available_space, constraints)
            && let Some(height) = intrinsic.height
        {
            return height;
        }
        // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic ratio then the used value of 'height' is:
        //
        //     (used width) / (intrinsic ratio)
        if computed_block_size.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            return self.content_block_size_from_aspect_ratio(node, self.used(node).content_inline_size.get());
        }
        // Otherwise, if 'height' has a computed value of 'auto', and the element has an intrinsic height, then that intrinsic height is the used value of 'height'.
        //
        // Otherwise, if 'height' has a computed value of 'auto', but none of the conditions above are met,
        // then the used value of 'height' must be set to the height of the largest rectangle that has a 2:1 ratio, has a height not greater than 150px,
        // and has a width not greater than the device width.
        if computed_block_size.is_auto() {
            return intrinsic.height.unwrap_or_else(|| CssPixels::from_integer(150));
        }
        // FIXME: Handle cases when available_space is not definite.
        self.calculate_inner_block_size(node, available_space, computed_block_size, constraints)
    }

    fn solve_replaced_size_constraint(
        &self,
        node: Node,
        input_inline_size: CssPixels,
        input_block_size: CssPixels,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> (CssPixels, CssPixels) {
        // 10.4 Minimum and maximum widths: 'min-width' and 'max-width'
        // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
        let style = self.style(node);
        let min_inline = if style.min_width().is_auto() {
            CssPixels::default()
        } else {
            self.calculate_inner_inline_size(node, available_space.inline_size, style.min_width(), constraints)
        };
        let specified_max_inline =
            if self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
                input_inline_size
            } else {
                self.calculate_inner_inline_size(node, available_space.inline_size, style.max_width(), constraints)
            };
        let max_inline = min_inline.max(specified_max_inline);
        let min_block = if style.min_height().is_auto() {
            CssPixels::default()
        } else {
            self.calculate_inner_block_size(node, available_space, style.min_height(), constraints)
        };
        let specified_max_block =
            if self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints) {
                input_block_size
            } else {
                self.calculate_inner_block_size(node, available_space, style.max_height(), constraints)
            };
        let max_block = min_block.max(specified_max_block);

        // These are from the "Constraint Violation" table in spec, but reordered so that each condition is
        // interpreted as mutually exclusive to any other.
        if input_inline_size < min_inline && input_block_size > max_block {
            return (min_inline, max_block);
        }
        if input_inline_size > max_inline && input_block_size < min_block {
            return (max_inline, min_block);
        }
        if input_inline_size > CssPixels::default() && input_block_size > CssPixels::default() {
            let max_inline_fraction_le_max_block = (max_inline.raw_value() as i64)
                * (input_block_size.raw_value() as i64)
                <= (max_block.raw_value() as i64) * (input_inline_size.raw_value() as i64);
            if input_inline_size > max_inline && input_block_size > max_block && max_inline_fraction_le_max_block {
                return (
                    max_inline,
                    min_block.max(self.content_block_size_from_aspect_ratio(node, max_inline)),
                );
            }
            if input_inline_size > max_inline && input_block_size > max_block && !max_inline_fraction_le_max_block {
                return (
                    min_inline.max(self.content_inline_size_from_aspect_ratio(node, max_block)),
                    max_block,
                );
            }
            let min_inline_fraction_le_min_block = (min_inline.raw_value() as i64)
                * (input_block_size.raw_value() as i64)
                <= (min_block.raw_value() as i64) * (input_inline_size.raw_value() as i64);
            if input_inline_size < min_inline && input_block_size < min_block && min_inline_fraction_le_min_block {
                return (
                    max_inline.min(self.content_inline_size_from_aspect_ratio(node, min_block)),
                    min_block,
                );
            }
            if input_inline_size < min_inline && input_block_size < min_block && !min_inline_fraction_le_min_block {
                return (
                    min_inline,
                    max_block.min(self.content_block_size_from_aspect_ratio(node, min_inline)),
                );
            }
        }
        if input_inline_size > max_inline {
            return (
                max_inline,
                self.content_block_size_from_aspect_ratio(node, max_inline)
                    .max(min_block),
            );
        }
        if input_inline_size < min_inline {
            return (
                min_inline,
                self.content_block_size_from_aspect_ratio(node, min_inline)
                    .min(max_block),
            );
        }
        if input_block_size > max_block {
            return (
                self.content_inline_size_from_aspect_ratio(node, max_block)
                    .max(min_inline),
                max_block,
            );
        }
        if input_block_size < min_block {
            return (
                self.content_inline_size_from_aspect_ratio(node, min_block)
                    .min(max_inline),
                min_block,
            );
        }
        (input_inline_size, input_block_size)
    }

    pub(crate) fn compute_inline_size_for_replaced_element(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.3.4 Block-level, replaced elements in normal flow...
        // 10.3.2 Inline, replaced elements
        let style = self.style(node);
        let computed_inline = if self.should_treat_inline_size_as_auto(node, available_space) {
            FfiSizeValue::auto_value()
        } else {
            style.width()
        };
        let computed_block = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            FfiSizeValue::auto_value()
        } else {
            style.height()
        };
        // 1. The tentative used width is calculated (without 'min-width' and 'max-width')
        let mut used =
            self.tentative_inline_size_for_replaced_element(node, computed_inline, available_space, constraints);
        if computed_inline.is_auto() && computed_block.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            let block =
                self.tentative_block_size_for_replaced_element(node, computed_block, available_space, constraints);
            used = self
                .solve_replaced_size_constraint(node, used, block, available_space, constraints)
                .0;
        }
        // 2. If the tentative used width is greater than 'max-width', the rules above are applied again,
        //    but this time using the computed value of 'max-width' as the computed value for 'width'.
        if !self.should_treat_max_inline_size_as_none(node, available_space.inline_size, constraints) {
            let max =
                self.calculate_inner_inline_size(node, available_space.inline_size, style.max_width(), constraints);
            if used > max {
                used = self.tentative_inline_size_for_replaced_element(
                    node,
                    style.max_width(),
                    available_space,
                    constraints,
                );
            }
        }
        // 3. If the resulting width is smaller than 'min-width', the rules above are applied again,
        //    but this time using the value of 'min-width' as the computed value for 'width'.
        if !style.min_width().is_auto() {
            let min =
                self.calculate_inner_inline_size(node, available_space.inline_size, style.min_width(), constraints);
            if used < min {
                used = self.tentative_inline_size_for_replaced_element(
                    node,
                    style.min_width(),
                    available_space,
                    constraints,
                );
            }
        }
        used
    }

    pub(crate) fn compute_block_size_for_replaced_element(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // 10.6.2 Inline replaced elements
        // 10.6.4 Block-level replaced elements in normal flow
        // 10.6.6 Floating replaced elements
        // 10.6.10 'inline-block' replaced elements in normal flow
        let style = self.style(node);
        let computed_inline = if self.should_treat_inline_size_as_auto(node, available_space) {
            FfiSizeValue::auto_value()
        } else {
            style.width()
        };
        let computed_block = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            FfiSizeValue::auto_value()
        } else {
            style.height()
        };
        // 1. The tentative used height is calculated (without 'min-height' and 'max-height')
        let mut used =
            self.tentative_block_size_for_replaced_element(node, computed_block, available_space, constraints);
        if computed_inline.is_auto() && computed_block.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            // However, for replaced elements with both 'width' and 'height' computed as 'auto',
            // use the algorithm under 'Minimum and maximum widths'
            // https://www.w3.org/TR/CSS22/visudet.html#min-max-widths
            // to find the used width and height.
            let intrinsic = self.intrinsic_size_for_replaced_sizing(node);
            if intrinsic.width.is_some() || intrinsic.height.is_none() {
                // NOTE: This is a special case where calling tentative_inline_size_for_replaced_element() would call us right back,
                //       and we'd end up in an infinite loop. So we need to handle this case separately.
                let inline = self.tentative_inline_size_for_replaced_element(
                    node,
                    computed_inline,
                    available_space,
                    constraints,
                );
                used = self
                    .solve_replaced_size_constraint(node, inline, used, available_space, constraints)
                    .1;
            }
        }
        // 2. If this tentative height is greater than 'max-height', the rules above are applied again,
        //    but this time using the value of 'max-height' as the computed value for 'height'.
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints) {
            let max = self.calculate_inner_block_size(node, available_space, style.max_height(), constraints);
            if used > max {
                used = self.tentative_block_size_for_replaced_element(
                    node,
                    style.max_height(),
                    available_space,
                    constraints,
                );
            }
        }
        // 3. If the resulting height is smaller than 'min-height', the rules above are applied again,
        //    but this time using the value of 'min-height' as the computed value for 'height'.
        if !style.min_height().is_auto() {
            let min = self.calculate_inner_block_size(node, available_space, style.min_height(), constraints);
            if used < min {
                used = self.tentative_block_size_for_replaced_element(
                    node,
                    style.min_height(),
                    available_space,
                    constraints,
                );
            }
        }
        used
    }

    pub(crate) fn box_is_sized_as_replaced_element(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let facts = self.facts(node);
        // When a box has a preferred aspect ratio, its automatic sizes are calculated the same as for a
        // replaced element with a natural aspect ratio and no natural size in that axis, see e.g. CSS2 §10
        // and CSS Flexible Box Model Level 1 §9.2.
        // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-automatic
        if facts.has_replaced_element_table_display_adjustment()
            || (facts.is_replaced_box() && facts.has_auto_content_box_size())
        {
            return true;
        }
        if facts.has_preferred_aspect_ratio() || facts.has_auto_content_box_size() {
            // From CSS2:
            // If height and width both have computed values of auto and the element has an intrinsic ratio but no intrinsic height or width,
            // then the used value of width is undefined in CSS 2.
            // However, it is suggested that, if the containing block’s width does not itself depend on the replaced element’s width,
            // then the used value of width is calculated from the constraint equation used for block-level, non-replaced elements in normal flow.
            //
            // AD-HOC: If box has preferred aspect ratio but width and height are not specified, then we should
            //         size it as a normal box to match other browsers.
            if self.should_treat_inline_size_as_auto(node, available_space)
                && self.should_treat_block_size_as_auto(node, available_space, constraints)
                && !facts.has_auto_content_width()
                && !facts.has_auto_content_height()
            {
                return false;
            }
            return true;
        }
        false
    }

    pub(crate) fn constraints_for_child_context(
        &self,
        containing_block: Node,
        constraints: ContainingBlockConstraints,
    ) -> ContainingBlockConstraints {
        let facts = self.facts(containing_block);
        let style = self.style(containing_block);
        let used = self.used(containing_block);
        // Anonymous boxes are invisible to percentage resolution: their children resolve percentages
        // against the closest non-anonymous ancestor, so an anonymous containing block without a
        // definite size of its own passes the constraints it was given through. Anonymous table
        // cells are the exception: they are proper containing blocks with their own size semantics.
        let should_forward_indefinite_basis = facts.is_box()
            && facts.is_anonymous()
            && !facts.is_table_cell()
            && !facts.has_auto_content_box_size()
            && used.inline_size_constraint.get() == SizeConstraint::None
            && used.block_size_constraint.get() == SizeConstraint::None;

        let inline = if used.has_definite_inline_size() {
            Some(used.content_inline_size.get())
        } else if should_forward_indefinite_basis {
            constraints.percentage_basis_inline_size
        } else {
            None
        };
        let block = if used.has_definite_block_size() {
            Some(used.content_block_size.get())
        } else if should_forward_indefinite_basis {
            constraints.percentage_basis_block_size
        } else {
            None
        };

        // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
        // 1. Let element be the nearest ancestor containing block of element, if there is one.
        //    Otherwise, return the initial containing block.
        //
        // 2. If element has a computed value of the display property that is table-cell, then return a
        //    UA-defined value.
        // FIXME: Likely UA-defined value should not be 0.
        //
        // 3. If element has a computed value of the height property that is not auto, then return element.
        //
        // 4. If element has a computed value of the position property that is absolute, or if element is a
        //    not a block container or a table wrapper box, then return element.
        //
        // 5. Jump to the first step.
        // NOTE: Evaluated incrementally: in-flow auto-height block containers pass the basis they
        //       inherited from their own containing block through to their children.
        let quirks_block = if facts.is_viewport() {
            Some(used.content_block_size.get())
        } else if facts.is_table_cell() {
            Some(CssPixels::default())
        } else if !style.height().is_auto()
            || facts.is_absolutely_positioned()
            || !facts.is_block_container()
            || facts.is_table_wrapper()
        {
            Some(used.content_block_size.get())
        } else {
            constraints.quirks_mode_percentage_basis_block_size
        };

        ContainingBlockConstraints {
            percentage_basis_inline_size: inline,
            percentage_basis_block_size: block,
            quirks_mode_percentage_basis_block_size: quirks_block,
        }
    }

    pub(crate) fn should_treat_inline_size_as_auto(&self, node: Node, available_space: AvailableSpace) -> bool {
        let style = self.style(node);
        let size = style.width();
        if size.is_auto() {
            return true;
        }
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage {
            match cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                true,
                available_space.inline_size,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => return false,
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => return true,
                CyclicPercentageIntrinsicContribution::NotCyclic => {}
            }
            if available_space.inline_size == AvailableSize::Indefinite {
                return true;
            }
        }
        let facts = self.facts(node);
        // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for width...
        if facts.has_preferred_aspect_ratio() && size.is_intrinsic_sizing_constraint() {
            // If the box has no natural height to resolve the aspect ratio, we treat the width as auto.
            if !facts.has_auto_content_height() {
                return true;
            }
            // If the box has definite height, we can resolve the width through the aspect ratio.
            if self.used(node).has_definite_block_size() {
                return true;
            }
        }
        false
    }

    pub(crate) fn should_treat_block_size_as_auto(
        &self,
        node: Node,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let style = self.style(node);
        let size = style.height();
        let facts = self.facts(node);
        if size.is_auto() {
            if self.used(node).has_definite_inline_size() && facts.has_preferred_aspect_ratio() {
                return false;
            }
            return true;
        }
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage {
            match cyclic_percentage_intrinsic_contribution(
                facts.is_replaced_box(),
                true,
                available_space.block_size,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => return false,
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => return true,
                CyclicPercentageIntrinsicContribution::NotCyclic => {}
            }
            // https://www.w3.org/TR/CSS22/visudet.html#the-height-property
            // If the height of the containing block is not specified explicitly (i.e., it depends on
            // content height), and this element is not absolutely positioned, the percentage value
            // is treated as 'auto'.
            // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
            // In quirks mode, percentage heights can resolve even without explicit containing block
            // height. The quirk applies to DOM elements only (not anonymous boxes), and excludes
            // table-related display types.
            if !facts.is_absolutely_positioned() {
                let parent = self.parent(node);
                let parent_is_flex_or_grid = if parent.is_invalid() {
                    false
                } else {
                    let display = self.facts(parent).display();
                    display.is_flex_inside() || display.is_grid_inside()
                };
                // Flex/grid items resolve percentage heights against their container, not via quirk.
                // The quirk should not apply inside user agent shadow trees.
                let quirk_applies = facts.document_in_quirks_mode()
                    && !facts.is_anonymous()
                    && !facts.is_table_box()
                    && !parent_is_flex_or_grid
                    && !facts.is_in_user_agent_shadow_tree();
                if !quirk_applies && constraints.percentage_basis_block_size.is_none() {
                    return true;
                }
            }
        }
        // AD-HOC: If the box has a preferred aspect ratio and an intrinsic keyword for height...
        if facts.has_preferred_aspect_ratio() && size.is_intrinsic_sizing_constraint() {
            // If the box has no natural width to resolve the aspect ratio, we treat the height as auto.
            if !facts.has_auto_content_width() {
                return true;
            }
            // If the box has definite width, we can resolve the height through the aspect ratio.
            if self.used(node).has_definite_inline_size() {
                return true;
            }
        }
        false
    }

    pub(crate) fn should_treat_max_inline_size_as_none(
        &self,
        node: Node,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        let size = self.style(node).max_width();
        if size.is_none() || (available == AvailableSize::MaxContent && size.is_max_content()) {
            return true;
        }
        // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
        if size.contains_percentage {
            match cyclic_percentage_intrinsic_contribution(
                self.facts(node).is_replaced_box(),
                true,
                available,
                CyclicPercentageSizeProperty::PreferredOrMaxSize,
            ) {
                CyclicPercentageIntrinsicContribution::ResolveAsZero => return false,
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => return true,
                CyclicPercentageIntrinsicContribution::NotCyclic => {}
            }
            if constraints.percentage_basis_inline_size.is_none() {
                return true;
            }
        }
        (size.is_fit_content() && available.is_intrinsic_sizing_constraint())
            || (size.is_max_content() && available == AvailableSize::MaxContent)
            || (size.is_min_content() && available == AvailableSize::MinContent)
    }

    pub(crate) fn should_treat_max_block_size_as_none(
        &self,
        node: Node,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        // https://www.w3.org/TR/CSS22/visudet.html#min-max-heights
        // If the height of the containing block is not specified explicitly (i.e., it depends on content height),
        // and this element is not absolutely positioned, the percentage value is treated as '0' (for 'min-height')
        // or 'none' (for 'max-height').
        let size = self.style(node).max_height();
        if size.is_none() {
            return true;
        }
        if size.contains_percentage {
            if available == AvailableSize::MinContent {
                return false;
            }
            if constraints.percentage_basis_block_size.is_none() {
                return true;
            }
        }
        (size.is_fit_content() && available.is_intrinsic_sizing_constraint())
            || (size.is_max_content() && available == AvailableSize::MaxContent)
            || (size.is_min_content() && available == AvailableSize::MinContent)
    }

    fn calculate_stretch_fit_inline_size(&self, node: Node, available: AvailableSize) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
        // The size a box would take if its outer size filled the available space in the given axis;
        // in other words, the stretch fit into the available space, if that is definite.
        //
        // Undefined if the available space is indefinite.
        if !matches!(available, AvailableSize::Definite(_)) {
            return CssPixels::default();
        }
        let used = self.used(node);
        available.to_px_or_zero()
            - used.margin_left.get()
            - used.margin_right.get()
            - used.padding_left.get()
            - used.padding_right.get()
            - used.border_left.get()
            - used.border_right.get()
    }

    fn calculate_stretch_fit_block_size(&self, node: Node, available: AvailableSize) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#stretch-fit-size
        // The size a box would take if its outer size filled the available space in the given axis;
        // in other words, the stretch fit into the available space, if that is definite.
        // Undefined if the available space is indefinite.
        let used = self.used(node);
        available.to_px_or_zero()
            - used.margin_top.get()
            - used.margin_bottom.get()
            - used.padding_top.get()
            - used.padding_bottom.get()
            - used.border_top.get()
            - used.border_bottom.get()
    }

    fn intrinsic_block_cache_get(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<CssPixels> {
        self.callbacks
            .arena()
            .intrinsic_block_size_cache_get(self.callbacks.node_data(node), kind, key)
    }

    fn intrinsic_block_cache_put(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: CssPixels,
    ) {
        self.callbacks
            .arena()
            .intrinsic_block_size_cache_put(self.callbacks.node_data(node), kind, key, value);
    }

    fn intrinsic_inline_measurement_cache_get(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
    ) -> Option<IntrinsicInlineSizeMeasurement> {
        self.callbacks.arena().intrinsic_inline_size_measurement_cache_get(
            self.callbacks.node_data(node),
            kind,
            key,
        )
    }

    fn intrinsic_inline_measurement_cache_put(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        value: IntrinsicInlineSizeMeasurement,
    ) {
        self.callbacks.arena().intrinsic_inline_size_measurement_cache_put(
            self.callbacks.node_data(node),
            kind,
            key,
            value,
        );
    }

    fn cache_intrinsic_inline_measurement(
        &self,
        node: Node,
        kind: IntrinsicSizeCacheKind,
        key: IntrinsicSizeCacheKey,
        measurement: &MeasurementState,
        result: ChildLayoutResult,
        available_block_size: AvailableSize,
    ) {
        let used = measurement.root_used();
        self.intrinsic_inline_measurement_cache_put(
            node,
            kind,
            key,
            IntrinsicInlineSizeMeasurement {
                automatic_content_inline_size: result.automatic_content_inline_size,
                available_block_size,
                content_inline_size: used.content_inline_size.get(),
                content_block_size: used.content_block_size.get(),
                automatic_content_block_size: result.automatic_content_block_size,
                uses_collapsing_borders_model: used.uses_collapsing_borders_model.get(),
                has_first_baseline: used.has_first_baseline.get(),
                first_baseline: used.first_baseline.get(),
                has_last_baseline: used.has_last_baseline.get(),
                last_baseline: used.last_baseline.get(),
            },
        );
    }

    pub(crate) fn apply_cached_intrinsic_inline_measurement(
        &self,
        node: Node,
        available_inline_size: AvailableSize,
        available_block_size: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> Option<CssPixels> {
        // OPTIMIZATION: Calculating an intrinsic inline size already performs a complete measurement layout.
        // A later equivalent intrinsic line build only consumes the atomic box's measured dimensions and
        // baselines, so retain that summary instead of formatting the same descendants again. Commit layout
        // must still create all descendant geometry.
        if !self.state.is_measurement() {
            return None;
        }
        let kind = match available_inline_size {
            AvailableSize::MinContent => IntrinsicSizeCacheKind::MinContentInline,
            AvailableSize::MaxContent => IntrinsicSizeCacheKind::MaxContentInline,
            AvailableSize::Definite(_) | AvailableSize::Indefinite => return None,
        };
        let measurement = self.intrinsic_inline_measurement_cache_get(node, kind, cache_key(None, constraints))?;
        if measurement.available_block_size != available_block_size {
            return None;
        }
        let used = self.used_mut(node);
        if used.content_inline_size.get() != measurement.content_inline_size {
            return None;
        }

        used.set_content_block_size(measurement.content_block_size);
        used.uses_collapsing_borders_model
            .set(measurement.uses_collapsing_borders_model);
        used.has_first_baseline.set(measurement.has_first_baseline);
        used.first_baseline.set(measurement.first_baseline);
        used.has_last_baseline.set(measurement.has_last_baseline);
        used.last_baseline.set(measurement.last_baseline);
        Some(measurement.automatic_content_block_size)
    }

    fn calculate_transferred_inline_size_for_replaced_element(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> Option<CssPixels> {
        // https://drafts.csswg.org/css-sizing-3/#intrinsic-sizes
        // "size constraints in the opposite dimension will transfer through and can affect the auto size in the considered one"
        let facts = self.facts(node);
        let style = self.style(node);
        // https://drafts.csswg.org/css2/#inline-replaced-width
        // "'width' has a computed value of 'auto', 'height' has some other computed value, and the element does have an intrinsic ratio"
        if !facts.is_replaced_box()
            || !facts.has_preferred_aspect_ratio()
            || !style.width().is_auto()
            || style.height().is_auto()
            || style.height().is_intrinsic_sizing_constraint()
        {
            return None;
        }
        let available_space = self
            .used(node)
            .available_inner_space_or_constraints_from(AvailableSpace {
                inline_size: AvailableSize::MaxContent,
                block_size: AvailableSize::Indefinite,
            });
        if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            return None;
        }
        // https://drafts.csswg.org/css2/#inline-replaced-width
        // "(used height) * (intrinsic ratio)"
        Some(self.compute_inline_size_for_replaced_element(
            node,
            available_space,
            ContainingBlockConstraints::default(),
        ))
    }

    pub(crate) fn calculate_min_content_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let facts = self.facts(node);
        let style = self.style(node);
        if facts.is_replaced_box() && (style.width().contains_percentage || style.max_width().contains_percentage) {
            // https://www.w3.org/TR/css-sizing-3/#replaced-percentage-min-contribution
            // NOTE: If the box is replaced, a cyclic percentage in the value of any max size property or
            //       preferred size property (width/max-width/height/max-height), is resolved against zero
            //       when calculating the min-content contribution in the corresponding axis.
            // FIXME: If the box also has a preferred aspect ratio, then this min-content contribution is
            //        floored by any <length-percentage> minimum size from the opposite axis—resolving any
            //        such percentage against zero—transferred through the preferred aspect ratio.
            // Note: The min-content contribution is, as always, also floored by the minimum size in its own axis.
            if !style.min_width().is_length_percentage() {
                return CssPixels::default();
            }
            let mut zero_constraints = constraints;
            zero_constraints.percentage_basis_inline_size = Some(CssPixels::default());
            return self.calculate_inner_inline_size(
                node,
                AvailableSize::MinContent,
                style.min_width(),
                zero_constraints,
            );
        }
        if let Some(transferred) = self.calculate_transferred_inline_size_for_replaced_element(node, constraints) {
            return transferred;
        }
        let auto_size = self.auto_content_size(node);
        if let Some(width) = auto_size.width {
            return width;
        }
        if facts.is_replaced_box()
            && !facts.has_preferred_aspect_ratio()
            && let Some(fallback) = self.max_content_size_for_replaced_element_without_natural_size(
                node,
                auto_size,
                SizeDimension::Inline,
                ReplacedMaxContentSizeConstraints::default(),
            )
        {
            return fallback;
        }
        // Boxes with no children have zero intrinsic inline size.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        let key = cache_key(None, constraints);
        if let Some(cached) =
            self.intrinsic_inline_measurement_cache_get(node, IntrinsicSizeCacheKind::MinContentInline, key)
        {
            return cached.automatic_content_inline_size;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.inline_size_constraint.set(SizeConstraint::MinContent);
        root.has_definite_inline_size.set(false);
        let block_size = if root.has_definite_block_size() {
            AvailableSize::definite(root.content_block_size.get())
        } else {
            AvailableSize::Indefinite
        };
        let mut result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::MinContent,
                    block_size,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
        );
        result.automatic_content_inline_size = clamp_to_max_dimension_value(result.automatic_content_inline_size);
        let value = result.automatic_content_inline_size;
        self.cache_intrinsic_inline_measurement(
            node,
            IntrinsicSizeCacheKind::MinContentInline,
            key,
            &measurement,
            result,
            block_size,
        );
        value
    }

    pub(crate) fn calculate_max_content_inline_size(
        &self,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let facts = self.facts(node);
        let style = self.style(node);
        let mut auto_size = self.auto_content_size(node);
        if let Some(transferred) = self.calculate_transferred_inline_size_for_replaced_element(node, constraints) {
            return transferred;
        }
        if auto_size.width.is_none() && (facts.has_default_preferred_width() || facts.has_default_preferred_height()) {
            // https://drafts.csswg.org/css-sizing-3/#cyclic-percentage-contribution
            // "If the box is non-replaced, then the entire value of any max size property or preferred size property
            // ('width'/'max-width'/'height'/'max-height') specified as an expression containing a percentage [...] that is
            // cyclic is treated for the purpose of calculating the box's intrinsic size contributions only as that
            // property's initial value."
            //
            // This means an `appearance: none` text input with a cyclic `width: 100%` still contributes its `width: auto`
            // size to max-content sizing. Do not use this for min-content sizing: CSS Sizing's "Compressible Replaced
            // Elements" section considers non-button-like <input> controls replaced for the percentage-sized replaced
            // element rule, so their cyclic-percentage min-content contribution can still compress toward zero.
            auto_size = ReplacedIntrinsicSize {
                width: facts
                    .has_default_preferred_width()
                    .then_some(facts.default_preferred_width()),
                height: facts
                    .has_default_preferred_height()
                    .then_some(facts.default_preferred_height()),
                aspect_ratio: None,
            };
        }
        if let Some(width) = auto_size.width {
            return width;
        }
        let definite_block_size =
            if facts.is_replaced_box() && auto_size.height.is_none() && self.used(node).has_definite_block_size() {
                Some(self.used(node).content_block_size.get())
            } else {
                None
            };
        let max_content_available = AvailableSize::MaxContent;
        let intrinsic_available_space = AvailableSpace {
            inline_size: max_content_available,
            block_size: AvailableSize::Indefinite,
        };
        let resolve_destination_inline_size =
            |size: FfiSizeValue, property: CyclicPercentageSizeProperty| -> Option<CssPixels> {
                if !size.is_length_percentage() {
                    return None;
                }
                match cyclic_percentage_intrinsic_contribution(
                    facts.is_replaced_box(),
                    size.contains_percentage,
                    max_content_available,
                    property,
                ) {
                    CyclicPercentageIntrinsicContribution::TreatAsInitialValue => None,
                    CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                        let mut zero_constraints = constraints;
                        zero_constraints.percentage_basis_inline_size = Some(CssPixels::default());
                        Some(self.calculate_inner_inline_size(node, max_content_available, size, zero_constraints))
                    }
                    CyclicPercentageIntrinsicContribution::NotCyclic => {
                        if size.contains_percentage && constraints.percentage_basis_inline_size.is_none() {
                            None
                        } else {
                            Some(self.calculate_inner_inline_size(node, max_content_available, size, constraints))
                        }
                    }
                }
            };
        let resolve_block_size = |size: FfiSizeValue, property: CyclicPercentageSizeProperty| -> Option<CssPixels> {
            if !size.is_length_percentage() {
                return None;
            }
            if !size.contains_percentage || constraints.percentage_basis_block_size.is_some() {
                return Some(self.calculate_inner_block_size(node, intrinsic_available_space, size, constraints));
            }
            match cyclic_percentage_intrinsic_contribution(
                facts.is_replaced_box(),
                size.contains_percentage,
                max_content_available,
                property,
            ) {
                CyclicPercentageIntrinsicContribution::TreatAsInitialValue => None,
                CyclicPercentageIntrinsicContribution::ResolveAsZero => {
                    let mut zero_constraints = constraints;
                    zero_constraints.percentage_basis_block_size = Some(CssPixels::default());
                    Some(self.calculate_inner_block_size(node, intrinsic_available_space, size, zero_constraints))
                }
                CyclicPercentageIntrinsicContribution::NotCyclic => None,
            }
        };

        let definite_minimum_inline_size =
            resolve_destination_inline_size(style.min_width(), CyclicPercentageSizeProperty::MinSize);
        let definite_minimum_block_size = resolve_block_size(style.min_height(), CyclicPercentageSizeProperty::MinSize);
        let replaced_constraints = ReplacedMaxContentSizeConstraints {
            definite_size_in_ratio_determining_axis: definite_block_size,
            minimum_inline_size: definite_minimum_inline_size,
            minimum_block_size: definite_minimum_block_size,
        };
        if let Some(max_content_inline_size) = self.max_content_size_for_replaced_element_without_natural_size(
            node,
            auto_size,
            SizeDimension::Inline,
            replaced_constraints,
        ) {
            if definite_block_size.is_none()
                && facts.has_preferred_aspect_ratio()
                && let Some(definite_maximum_block_size) =
                    resolve_block_size(style.max_height(), CyclicPercentageSizeProperty::PreferredOrMaxSize)
            {
                // https://drafts.csswg.org/css-sizing-4/#aspect-ratio-size-transfers
                // First, any definite minimum size is converted and transferred from the origin to destination axis.
                // This transferred minimum is capped by any definite preferred or maximum size in the destination axis.
                let mut transferred_minimum =
                    definite_minimum_block_size.map(|value| self.content_inline_size_from_aspect_ratio(node, value));
                if let Some(value) = transferred_minimum {
                    transferred_minimum = resolve_destination_inline_size(
                        style.width(),
                        CyclicPercentageSizeProperty::PreferredOrMaxSize,
                    )
                    .map_or(Some(value), |resolved| Some(value.min(resolved)));
                    let value = transferred_minimum.unwrap();
                    transferred_minimum = resolve_destination_inline_size(
                        style.max_width(),
                        CyclicPercentageSizeProperty::PreferredOrMaxSize,
                    )
                    .map_or(Some(value), |resolved| Some(value.min(resolved)));
                }

                // Then, any definite maximum size is converted and transferred from the origin to destination.
                // This transferred maximum is floored by any definite preferred or minimum size in the destination axis
                // as well as by the transferred minimum, if any.
                let mut transferred_maximum =
                    self.content_inline_size_from_aspect_ratio(node, definite_maximum_block_size);
                if let Some(resolved) =
                    resolve_destination_inline_size(style.width(), CyclicPercentageSizeProperty::PreferredOrMaxSize)
                {
                    transferred_maximum = transferred_maximum.max(resolved);
                }
                if let Some(resolved) =
                    resolve_destination_inline_size(style.min_width(), CyclicPercentageSizeProperty::MinSize)
                {
                    transferred_maximum = transferred_maximum.max(resolved);
                }
                if let Some(transferred_minimum) = transferred_minimum {
                    transferred_maximum = transferred_maximum.max(transferred_minimum);
                }
                return max_content_inline_size.min(transferred_maximum);
            }
            return max_content_inline_size;
        }
        // Boxes with no children have zero intrinsic inline size.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        let key = cache_key(None, constraints);
        if let Some(cached) =
            self.intrinsic_inline_measurement_cache_get(node, IntrinsicSizeCacheKind::MaxContentInline, key)
        {
            return cached.automatic_content_inline_size;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.inline_size_constraint.set(SizeConstraint::MaxContent);
        root.has_definite_inline_size.set(false);
        let block_size = if root.has_definite_block_size() {
            AvailableSize::definite(root.content_block_size.get())
        } else {
            AvailableSize::Indefinite
        };
        let mut result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::MaxContent,
                    block_size,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
        );
        result.automatic_content_inline_size = clamp_to_max_dimension_value(result.automatic_content_inline_size);
        let value = result.automatic_content_inline_size;
        self.cache_intrinsic_inline_measurement(
            node,
            IntrinsicSizeCacheKind::MaxContentInline,
            key,
            &measurement,
            result,
            block_size,
        );
        value
    }

    pub(crate) fn calculate_min_content_block_size(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // https://www.w3.org/TR/css-sizing-3/#min-content-block-size
        let facts = self.facts(node);
        // For block containers, tables, and inline boxes, this is equivalent to the max-content block size.
        if facts.is_block_container() || facts.is_table_box() {
            return self.calculate_max_content_block_size(node, inline_size, constraints);
        }
        let auto_size = self.auto_content_size(node);
        if let Some(height) = auto_size.height {
            return auto_size.aspect_ratio.map_or(height, |ratio| ratio.divide(inline_size));
        }
        // Boxes with no children have zero intrinsic height.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        let key = cache_key(Some(inline_size), constraints);
        if let Some(cached) = self.intrinsic_block_cache_get(node, IntrinsicSizeCacheKind::MinContentBlock, key) {
            return cached;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.block_size_constraint.set(SizeConstraint::MinContent);
        root.has_definite_block_size.set(false);
        root.set_content_inline_size(inline_size);
        let result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::definite(inline_size),
                    block_size: AvailableSize::MinContent,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
        );
        let value = clamp_to_max_dimension_value(result.automatic_content_block_size);
        self.intrinsic_block_cache_put(node, IntrinsicSizeCacheKind::MinContentBlock, key, value);
        value
    }

    pub(crate) fn calculate_max_content_block_size(
        &self,
        node: Node,
        inline_size: CssPixels,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        if let Some(ratio) = self.facts(node).preferred_aspect_ratio() {
            return ratio.divide(inline_size);
        }
        let auto_size = self.auto_content_size(node);
        if let Some(height) = auto_size.height {
            return height;
        }
        if let Some(fallback) = self.max_content_size_for_replaced_element_without_natural_size(
            node,
            auto_size,
            SizeDimension::Block,
            ReplacedMaxContentSizeConstraints::default(),
        ) {
            return fallback;
        }
        // Boxes with no children have zero intrinsic height.
        if !self.has_children(node) {
            return CssPixels::default();
        }
        let key = cache_key(Some(inline_size), constraints);
        if let Some(cached) = self.intrinsic_block_cache_get(node, IntrinsicSizeCacheKind::MaxContentBlock, key) {
            return cached;
        }

        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        let root = measurement.root_used();
        root.block_size_constraint.set(SizeConstraint::MaxContent);
        root.has_definite_block_size.set(false);
        root.set_content_inline_size(inline_size);
        let result = measurement.run(
            node,
            LayoutInput {
                available_space: AvailableSpace {
                    inline_size: AvailableSize::definite(inline_size),
                    block_size: AvailableSize::MaxContent,
                },
                containing_block_constraints: constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
        );
        let value = clamp_to_max_dimension_value(result.automatic_content_block_size);
        self.intrinsic_block_cache_put(node, IntrinsicSizeCacheKind::MaxContentBlock, key, value);
        value
    }

    pub(crate) fn measure_automatic_content_block_size(
        &self,
        node: Node,
        layout_mode: LayoutMode,
        inner_available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let measurement = MeasurementState::create(self.callbacks, node, constraints);
        measurement
            .run_with_layout_mode(
                node,
                layout_mode,
                LayoutInput {
                    available_space: inner_available_space,
                    containing_block_constraints: constraints,
                    content_box_position_in_bfc_root: None,
                    table_grid_min_border_box_block_size: None,
                },
            )
            .automatic_content_block_size
    }

    pub(crate) fn make_button_content_box_definite(
        &self,
        node: Node,
        layout_mode: LayoutMode,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
        measured_content_block_size: Option<CssPixels>,
    ) {
        let facts = self.facts(node);
        if !facts.uses_button_layout() {
            return;
        }
        // Flex/grid-inside buttons are their own flex/grid container and get no anonymous content wrapper,
        // so there is nothing to make definite for centering.
        let style = self.style(node);
        if style.display().is_flex_inside() || style.display().is_grid_inside() {
            return;
        }
        // With auto height and no min-height the content box already exactly wraps the content, so there is
        // no extra space to center within and no need to force a definite content box.
        if style.height().is_auto() && style.min_height().is_auto() {
            return;
        }
        if self.used(node).has_definite_block_size() {
            return;
        }
        let natural = measured_content_block_size.unwrap_or_else(|| {
            self.measure_automatic_content_block_size(
                node,
                layout_mode,
                self.used(node)
                    .available_inner_space_or_constraints_from(available_space),
                constraints,
            )
        });
        let mut used_block_size = if self.should_treat_block_size_as_auto(node, available_space, constraints) {
            natural
        } else {
            self.calculate_inner_block_size(node, available_space, style.height(), constraints)
        };
        if !self.should_treat_max_block_size_as_none(node, available_space.block_size, constraints)
            && !style.max_height().is_auto()
        {
            used_block_size = used_block_size.min(self.calculate_inner_block_size(
                node,
                available_space,
                style.max_height(),
                constraints,
            ));
        }
        if !style.min_height().is_auto() {
            used_block_size = used_block_size.max(self.calculate_inner_block_size(
                node,
                available_space,
                style.min_height(),
                constraints,
            ));
        }
        // Only force a definite content box when the button's used block size exceeds its content block size, so a larger
        // preferred or minimum size has room to center within. A content-sized box stays indefinite, so an intrinsic
        // keyword does not resolve percentage-sized descendants.
        if used_block_size <= natural {
            return;
        }
        let used = self.used_mut(node);
        used.set_content_block_size(used_block_size);
        used.has_definite_block_size.set(true);
    }

    pub(crate) fn table_box_inside_wrapper(&self, wrapper: Node) -> Node {
        fn find(context: &SizingContext, parent: Node) -> Option<Node> {
            let mut child = context.first_child(parent);
            while !child.is_invalid() {
                let facts = context.facts(child);
                if facts.is_box() && facts.display().is_table_inside() {
                    return Some(child);
                }
                if let Some(table) = find(context, child) {
                    return Some(table);
                }
                child = context.next_sibling(child);
            }
            None
        }

        find(self, wrapper).expect("table wrapper must contain a table box")
    }

    fn create_measurement_used_values(
        measurement: &MeasurementState,
        node: Node,
        constraints: ContainingBlockConstraints,
    ) -> &UsedValues {
        let callbacks = *measurement.callbacks();
        measurement
            .rust_state()
            .create_used_values(&callbacks, node, constraints)
    }

    // 17.5.2 Table width algorithms: the 'table-layout' property
    // https://www.w3.org/TR/CSS22/tables.html#width-layout
    pub(crate) fn compute_table_box_inline_size_inside_wrapper(
        &self,
        wrapper: Node,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
        table_wrapper_containing_block_inline_size: Option<CssPixels>,
        table_wrapper_inline_size_mode: TableWrapperInlineSizeMode,
    ) -> CssPixels {
        // CSS 2 says the table wrapper inline size is the border-edge inline size of the table grid box inside it.

        let style = self.style(wrapper);
        let containing_block_inline_size =
            table_wrapper_containing_block_inline_size.unwrap_or_else(|| available_space.inline_size.to_px_or_zero());

        // If 'margin-left', or 'margin-right' are computed as 'auto', their used value is '0'.
        let margin_left = style.margin_left().to_px(containing_block_inline_size);
        let margin_right = style.margin_right().to_px(containing_block_inline_size);

        // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
        let available_inline_size = containing_block_inline_size - margin_left - margin_right;
        let table_box = self.table_box_inside_wrapper(wrapper);

        let measurement = MeasurementState::create(self.callbacks, wrapper, table_wrapper_constraints);

        // The table wrapper is invisible to percentage resolution, so the table box gets the
        // wrapper's constraints unchanged. Callers measuring a table wrapper for grid alignment
        // pass the grid-area inline size as the wrapper's percentage basis.
        let table_constraints = table_wrapper_constraints;
        let table_used = Self::create_measurement_used_values(&measurement, table_box, table_constraints);
        let table_style = self.style(table_box);
        table_used.border_left.set(table_style.border_left_width());
        table_used.border_right.set(table_style.border_right_width());
        table_used
            .padding_left
            .set(table_style.padding_left().to_px(containing_block_inline_size));
        table_used
            .padding_right
            .set(table_style.padding_right().to_px(containing_block_inline_size));

        let mut context = crate::layout::create_formatting_context(
            measurement.rust_state(),
            table_box,
            crate::layout::FcParents::default(),
            crate::layout::FfiFormattingContextType::Table,
            LayoutMode::IntrinsicSizing,
            false,
            *measurement.callbacks(),
        );
        let table_available = table_used.available_inner_space_or_constraints_from(available_space);
        let FormattingContextInstance { frame, implementation } = &mut *context;
        let FcImpl::Table(table) = implementation else {
            unreachable!("table measurement created a non-table context");
        };
        table.run_until_inline_size_calculation(
            LayoutInput {
                available_space: table_available,
                containing_block_constraints: table_constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
            true,
        );
        frame.automatic_content_inline_size = table.automatic_content_inline_size();

        let table_used_inline_size = table_used.border_box_inline_size(false);
        if table_wrapper_inline_size_mode == TableWrapperInlineSizeMode::UseTableUsedInlineSizeIfNotAuto
            && !table_style.width().is_auto()
        {
            return table_used_inline_size;
        }
        if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
            table_used_inline_size.min(available_inline_size)
        } else {
            table_used_inline_size
        }
    }

    // 17.5.3 Table height algorithms
    // https://www.w3.org/TR/CSS22/tables.html#height-layout
    pub(crate) fn compute_table_box_block_size_inside_wrapper(
        &self,
        wrapper: Node,
        available_space: AvailableSpace,
        table_wrapper_constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // The table wrapper block size should equal the block size of the table box it contains.

        let style = self.style(wrapper);
        let containing_block_inline_size = available_space.inline_size.to_px_or_zero();
        let containing_block_block_size = available_space.block_size.to_px_or_zero();

        // If 'margin-top', or 'margin-bottom' are computed as 'auto', their used value is '0'.
        let margin_top = style.margin_top().to_px(containing_block_inline_size);
        let margin_bottom = style.margin_bottom().to_px(containing_block_inline_size);

        // table-wrapper can't have borders or paddings but it might have margin taken from table-root.
        let available_block_size = containing_block_block_size - margin_top - margin_bottom;
        let table_box = self.table_box_inside_wrapper(wrapper);

        let measurement = MeasurementState::create(self.callbacks, wrapper, table_wrapper_constraints);
        measurement.run_with_layout_mode(
            wrapper,
            LayoutMode::IntrinsicSizing,
            LayoutInput {
                available_space: self
                    .used(wrapper)
                    .available_inner_space_or_constraints_from(available_space),
                containing_block_constraints: table_wrapper_constraints,
                content_box_position_in_bfc_root: None,
                table_grid_min_border_box_block_size: None,
            },
        );

        let table_used = measurement.rust_state().used_values(measurement.callbacks(), table_box);
        let table_used_block_size = table_used.border_box_block_size(table_used.uses_collapsing_borders_model.get());
        if matches!(available_space.block_size, AvailableSize::Definite(_)) {
            table_used_block_size.min(available_block_size)
        } else {
            table_used_block_size
        }
    }

    pub(crate) fn calculate_fit_content_size(
        &self,
        node: Node,
        axis: SizingAxis,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        // https://drafts.csswg.org/css-sizing-3/#fit-content-size
        match axis {
            SizingAxis::Inline => {
                // If the available space in a given axis is definite, equal to clamp(min-content size, stretch-fit size,
                // max-content size) (i.e. max(min-content size, min(max-content size, stretch-fit size))).
                if matches!(available_space.inline_size, AvailableSize::Definite(_)) {
                    let stretch = self.calculate_stretch_fit_inline_size(node, available_space.inline_size);
                    let max_content = self.calculate_max_content_inline_size(node, constraints);
                    if max_content <= stretch {
                        return max_content;
                    }
                    return self.calculate_min_content_inline_size(node, constraints).max(stretch);
                }
                // When sizing under a min-content constraint, equal to the min-content size.
                if available_space.inline_size == AvailableSize::MinContent {
                    return self.calculate_min_content_inline_size(node, constraints);
                }
                // Otherwise, equal to the max-content size in that axis.
                self.calculate_max_content_inline_size(node, constraints)
            }
            SizingAxis::Block => {
                let inline_size = available_space.inline_size.to_px_or_zero();
                // https://drafts.csswg.org/css-sizing-3/#fit-content-size
                // If the available space in a given axis is definite,
                // equal to clamp(min-content size, stretch-fit size, max-content size)
                // (i.e. max(min-content size, min(max-content size, stretch-fit size))).
                if matches!(available_space.block_size, AvailableSize::Definite(_)) {
                    let stretch = self.calculate_stretch_fit_block_size(node, available_space.block_size);
                    let max_content = self.calculate_max_content_block_size(node, inline_size, constraints);
                    if max_content <= stretch {
                        return max_content;
                    }
                    return self
                        .calculate_min_content_block_size(node, inline_size, constraints)
                        .max(stretch);
                }
                // When sizing under a min-content constraint, equal to the min-content size.
                if available_space.block_size == AvailableSize::MinContent {
                    return self.calculate_min_content_block_size(node, inline_size, constraints);
                }
                // Otherwise, equal to the max-content size in that axis.
                self.calculate_max_content_block_size(node, inline_size, constraints)
            }
        }
    }

    pub(crate) fn calculate_inner_inline_size(
        &self,
        node: Node,
        available: AvailableSize,
        preferred_size: FfiSizeValue,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        assert!(!preferred_size.is_auto());
        let basis = if preferred_size.contains_percentage {
            if let Some(basis) = constraints.percentage_basis_inline_size {
                basis
            } else {
                available.to_px_or_zero()
            }
        } else {
            available.to_px_or_zero()
        };
        if preferred_size.is_fit_content() {
            return self.calculate_fit_content_size(
                node,
                SizingAxis::Inline,
                AvailableSpace {
                    inline_size: available,
                    block_size: AvailableSize::Indefinite,
                },
                constraints,
            );
        }
        if preferred_size.is_max_content() {
            return self.calculate_max_content_inline_size(node, constraints);
        }
        if preferred_size.is_min_content() {
            return self.calculate_min_content_inline_size(node, constraints);
        }
        let value = preferred_size.to_px(basis);
        let style = self.style(node);
        if style.box_sizing() == box_sizing::BORDER_BOX {
            let used = self.used(node);
            return subtract_border_box_adjustment(
                value,
                style.border_left_width(),
                used.padding_left.get(),
                style.border_right_width(),
                used.padding_right.get(),
            );
        }
        value
    }

    pub(crate) fn calculate_inner_block_size(
        &self,
        node: Node,
        available_space: AvailableSpace,
        preferred_size: FfiSizeValue,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        if preferred_size.is_auto() && self.facts(node).has_preferred_aspect_ratio() {
            return self.content_block_size_from_aspect_ratio(node, self.used(node).content_inline_size.get());
        }
        assert!(!preferred_size.is_auto());
        if preferred_size.is_fit_content() {
            return self.calculate_fit_content_size(node, SizingAxis::Block, available_space, constraints);
        }
        if preferred_size.is_max_content() {
            return self.calculate_max_content_block_size(
                node,
                available_space.inline_size.to_px_or_zero(),
                constraints,
            );
        }
        if preferred_size.is_min_content() {
            return self.calculate_min_content_block_size(
                node,
                available_space.inline_size.to_px_or_zero(),
                constraints,
            );
        }

        let mut basis = available_space.block_size.to_px_or_zero();
        // NOTE: Percentage heights are resolved against the containing block's used height,
        //       not the available space height. The containing block's height must be definite
        //       for percentage resolution to work (otherwise should_treat_block_size_as_auto
        //       should have returned true and we wouldn't be here).
        // NOTE: We only do this when available space height is indefinite. If it's definite,
        //       we trust that the caller has set it up correctly (e.g., grid/flex items get
        //       their cell/area size as available space).
        if preferred_size.contains_percentage && available_space.block_size == AvailableSize::Indefinite {
            // https://quirks.spec.whatwg.org/#the-percentage-height-calculation-quirk
            // NOTE: Flex/grid items resolve percentage heights against their container, not via quirk.
            let facts = self.facts(node);
            let parent = self.parent(node);
            let parent_is_flex_or_grid = if parent.is_invalid() {
                false
            } else {
                let display = self.facts(parent).display();
                display.is_flex_inside() || display.is_grid_inside()
            };
            if facts.document_in_quirks_mode()
                && !facts.is_anonymous()
                && !parent_is_flex_or_grid
                && !facts.is_in_user_agent_shadow_tree()
            {
                basis = constraints.quirks_mode_percentage_basis_block_size.unwrap_or_default();
            } else {
                basis = constraints.block_basis();
            }
        }
        let value = preferred_size.to_px(basis);
        let style = self.style(node);
        if style.box_sizing() == box_sizing::BORDER_BOX {
            let used = self.used(node);
            return subtract_border_box_adjustment(
                value,
                style.border_top_width(),
                used.padding_top.get(),
                style.border_bottom_width(),
                used.padding_bottom.get(),
            );
        }
        value
    }

    pub(crate) fn calculate_inner_size_for_property(
        &self,
        node: Node,
        axis: SizingAxis,
        property: SizingProperty,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        let style = self.style(node);
        let size = match property {
            SizingProperty::Width => style.width(),
            SizingProperty::Height => style.height(),
            SizingProperty::MinWidth => style.min_width(),
            SizingProperty::MinHeight => style.min_height(),
            SizingProperty::MaxWidth => style.max_width(),
            SizingProperty::MaxHeight => style.max_height(),
            SizingProperty::FlexBasis => style.flex_basis(),
        };
        match axis {
            SizingAxis::Inline => {
                self.calculate_inner_inline_size(node, available_space.inline_size, size, constraints)
            }
            SizingAxis::Block => self.calculate_inner_block_size(node, available_space, size, constraints),
        }
    }

    pub(crate) fn calculate_inner_inline_width(
        &self,
        node: Node,
        available: AvailableSize,
        constraints: ContainingBlockConstraints,
    ) -> CssPixels {
        self.calculate_inner_inline_size(node, available, self.style(node).width(), constraints)
    }

    pub(crate) fn should_treat_size_as_auto(
        &self,
        node: Node,
        axis: SizingAxis,
        available_space: AvailableSpace,
        constraints: ContainingBlockConstraints,
    ) -> bool {
        match axis {
            SizingAxis::Inline => self.should_treat_inline_size_as_auto(node, available_space),
            SizingAxis::Block => self.should_treat_block_size_as_auto(node, available_space, constraints),
        }
    }
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
    loop {
        let containing_block = callbacks.containing_block(target);
        let facts = state.node_facts(callbacks, target);
        if containing_block.is_invalid() || formatting_context_type_created_by_box(facts).is_some() {
            break;
        }
        target = containing_block;
    }
    state.register_contained_abspos_child(callbacks, target, child, static_position_rect);
}

pub(crate) fn box_baseline(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    mut baseline_set: BaselineSet,
) -> CssPixels {
    let facts = state.node_facts(callbacks, box_);
    let style = state.style_facts(callbacks, box_);
    let used_pointer = state.used_values(callbacks, box_);
    let used = used_pointer;
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
        BaselineSet::First if used.has_first_baseline.get() => Some(used.first_baseline.get()),
        BaselineSet::Last if used.has_last_baseline.get() => Some(used.last_baseline.get()),
        _ => None,
    };
    if let Some(content_baseline) = content_baseline
        && (derive_baseline_from_content || input_derives_from_children)
    {
        return used.margin_box_top(collapsed) + content_baseline;
    }

    // If the box has no baseline set, the bottom margin edge of the box is used.
    used.margin_box_block_size(collapsed)
}

pub(crate) fn compute_and_store_baselines(
    state: &LayoutState,
    callbacks: &FfiLayoutFcCallbacks,
    box_: Node,
    inhibits_floating: bool,
) {
    let used_pointer = state.used_values(callbacks, box_);
    // NOTE: This may run more than once for the same UsedValues (e.g. table cells are laid out twice),
    //       so reset both baselines before deriving them anew.
    used_pointer.has_first_baseline.set(false);
    used_pointer.has_last_baseline.set(false);

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
            child_offset_from_margin_edge + box_baseline(state, callbacks, fragment_node, baseline_set)
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
        used_pointer.has_first_baseline.set(true);
        used_pointer.first_baseline.set(first_baseline);
        used_pointer.has_last_baseline.set(true);
        used_pointer.last_baseline.set(last_baseline);
        return;
    }

    if callbacks.first_child(box_).is_invalid() || facts.children_are_inline() {
        return;
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
            if !child_facts.is_box() {
                continue;
            }
            if child_facts.is_absolutely_positioned() || (!inhibits_floating && child_facts.is_floating()) {
                continue;
            }
            let Some(child_state) = state.try_used_values(callbacks, child) else {
                continue;
            };
            match baseline_set {
                BaselineSet::First if child_state.has_first_baseline.get() => {}
                BaselineSet::Last if child_state.has_last_baseline.get() => {}
                _ => continue,
            }
            let child_offset_from_margin_edge = child_state.content_offset.get().y
                - child_state.margin_box_top(child_state.uses_collapsing_borders_model.get());
            return Some(child_offset_from_margin_edge + box_baseline(state, callbacks, child, baseline_set));
        }
        None
    };
    let first_baseline = baseline_from_children(BaselineSet::First);
    let last_baseline = baseline_from_children(BaselineSet::Last);
    if let Some(value) = first_baseline {
        used_pointer.has_first_baseline.set(true);
        used_pointer.first_baseline.set(value);
    }
    if let Some(value) = last_baseline {
        used_pointer.has_last_baseline.set(true);
        used_pointer.last_baseline.set(value);
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
#[repr(C)]
pub struct FfiTableBoxFacts {
    pub raw_column_span: u32,
    pub border_top_color: u32,
    pub border_right_color: u32,
    pub border_bottom_color: u32,
    pub border_left_color: u32,
}

#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub(crate) struct ChildLayoutResult {
    pub automatic_content_inline_size: CssPixels,
    pub automatic_content_block_size: CssPixels,
}

pub(crate) struct PendingChildLayout<'pass> {
    context: Box<FormattingContextInstance<'pass>>,
}

impl PendingChildLayout<'_> {
    pub(crate) fn result(&self) -> ChildLayoutResult {
        ChildLayoutResult {
            automatic_content_inline_size: self.context.automatic_content_inline_size,
            automatic_content_block_size: self.context.automatic_content_block_size,
        }
    }

    pub(crate) fn finish(mut self) {
        complete_formatting_context_after_root_box_has_used_size(&mut self.context);
    }
}

pub(crate) enum ChildLayoutOutcome<'pass> {
    Skipped,
    Created(PendingChildLayout<'pass>),
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

pub type FfiBuildTableBoxFactsCallback = unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiTableBoxFacts;

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
    pub release_calc_handle: crate::layout::FfiReleaseCalcHandleCallback,
    pub release_anchor_name_handle: crate::layout::FfiReleaseAnchorNameHandleCallback,
    pub build_replaced_content_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> crate::layout::FfiReplacedContentFacts,
    pub build_list_item_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> crate::layout::FfiListItemFacts,
    pub text_node_is_empty_editable: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub document_cursor_is_on_node: unsafe extern "C" fn(*mut c_void, *mut c_void) -> bool,
    pub build_table_box_facts: FfiBuildTableBoxFactsCallback,
    pub build_grid_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiGridStyleFacts,
    pub release_grid_facts_snapshot: unsafe extern "C" fn(*mut c_void, *mut c_void),
    pub release_grid_name_handle: unsafe extern "C" fn(usize),
    pub build_svg_facts: unsafe extern "C" fn(*mut c_void, *mut c_void) -> FfiSvgElementFacts,
    pub read_paintable_geometry:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void, *mut crate::layout::FfiPaintableGeometry) -> bool,
    pub read_paintable_svg_transforms:
        unsafe extern "C" fn(*mut c_void, *mut c_void, *mut FfiSvgComputedTransforms) -> bool,
    pub compute_svg_path: unsafe extern "C" fn(*mut c_void, *mut c_void, FfiSvgPathRequest) -> FfiSvgPathResult,
    pub release_svg_path: crate::layout::ReleaseRetainedLayoutHandle,
    pub svg_image_bounding_box: unsafe extern "C" fn(*mut c_void, *mut c_void, CssPixels, CssPixels) -> FfiFloatRect,
    pub anchor_lookup: unsafe extern "C" fn(*mut c_void, *mut c_void, usize, *const *mut c_void, usize) -> NodeSlotId,
    pub build_anchor_function_facts: unsafe extern "C" fn(*mut c_void, *const c_void) -> FfiAnchorFunctionFacts,
    pub anchor_function_fallback: unsafe extern "C" fn(*mut c_void, *const c_void) -> FfiAnchorFallbackFacts,
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

    pub(crate) fn style_reader_if_styled(&self, node: Node) -> Option<StyleReader<'static>> {
        let payloads = self.arena().style_payloads(node)?;
        // SAFETY: The node's ComputedValues keep the style container alive
        // for the pass, and the container is only replaced between passes:
        // set_computed_values verifies no pass is running and no layout node
        // is created mid-pass.
        Some(StyleReader::new(unsafe { &*std::ptr::from_ref(payloads) }))
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

pub(crate) struct FcFrame<'pass> {
    pub(crate) state: &'pass LayoutState,
    pub(crate) box_: Node,
    pub(crate) layout_mode: LayoutMode,
    pub(crate) callbacks: FfiLayoutFcCallbacks,
    pub(crate) should_collect_devtools_layout_data: bool,
    pub(crate) automatic_content_inline_size: CssPixels,
    pub(crate) automatic_content_block_size: CssPixels,
}

impl<'pass> FcFrame<'pass> {
    pub(crate) fn new(
        state: &'pass LayoutState,
        box_: Node,
        layout_mode: LayoutMode,
        callbacks: FfiLayoutFcCallbacks,
        should_collect_devtools_layout_data: bool,
    ) -> Self {
        Self {
            state,
            box_,
            layout_mode,
            callbacks,
            should_collect_devtools_layout_data,
            automatic_content_inline_size: CssPixels::default(),
            automatic_content_block_size: CssPixels::default(),
        }
    }
}

#[derive(Clone, Copy, Default)]
struct FcParents<'parent, 'pass> {
    block: Option<&'parent BlockFormattingContext<'pass>>,
    grid: Option<&'parent GridFormattingContext<'pass>>,
}

enum FcImpl<'pass> {
    Block(Box<BlockFormattingContext<'pass>>),
    Flex(Box<FlexFormattingContext<'pass>>),
    Grid(Box<GridFormattingContext<'pass>>),
    Table(Box<TableFormattingContext<'pass>>),
    Svg(Box<SvgFormattingContext<'pass>>),
    ReplacedWithChildren,
    InternalReplaced,
    InternalDummy,
}

pub(crate) struct FormattingContextInstance<'pass> {
    pub(crate) frame: FcFrame<'pass>,
    implementation: FcImpl<'pass>,
}

impl<'pass> std::ops::Deref for FormattingContextInstance<'pass> {
    type Target = FcFrame<'pass>;

    fn deref(&self) -> &Self::Target {
        &self.frame
    }
}

impl std::ops::DerefMut for FormattingContextInstance<'_> {
    fn deref_mut(&mut self) -> &mut Self::Target {
        &mut self.frame
    }
}

pub(crate) fn formatting_context_type_created_by_node_data(
    data: &NodeData,
    style: Option<StyleReader<'_>>,
    parent_style: Option<StyleReader<'_>>,
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
        facts.style_reader_if_styled(),
        facts.parent_style_reader_if_styled(),
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
        let style = arena.style_payloads(facts.node).map(StyleReader::new);
        let parent_style = (!data.parent.is_invalid())
            .then(|| arena.style_payloads(data.parent))
            .flatten()
            .map(StyleReader::new);
        formatting_context_type_created_by_node_data(data, style, parent_style)
            .map(|type_| type_ as u8)
            .unwrap_or(NO_FORMATTING_CONTEXT)
    })
}

fn create_formatting_context<'pass>(
    state: &'pass LayoutState,
    box_: Node,
    parents: FcParents<'_, 'pass>,
    fc_type: FfiFormattingContextType,
    layout_mode: LayoutMode,
    should_collect_devtools_layout_data: bool,
    callbacks: FfiLayoutFcCallbacks,
) -> Box<FormattingContextInstance<'pass>> {
    assert!(!box_.is_invalid());

    let frame = FcFrame::new(state, box_, layout_mode, callbacks, should_collect_devtools_layout_data);
    let implementation = match fc_type {
        FfiFormattingContextType::Block => FcImpl::Block(Box::new(
            BlockFormattingContext::new(state, box_, layout_mode, callbacks),
        )),
        FfiFormattingContextType::Flex => {
            FcImpl::Flex(Box::new(FlexFormattingContext::new(&frame)))
        }
        FfiFormattingContextType::Grid => FcImpl::Grid(Box::new(GridFormattingContext::new(
            state,
            box_,
            parents.grid,
            layout_mode,
            callbacks,
            should_collect_devtools_layout_data,
        ))),
        FfiFormattingContextType::Table => {
            let pending_table_offset = parents
                .block
                .and_then(|parent| parent.take_pending_table_box_content_offset_in_wrapper());
            FcImpl::Table(Box::new(TableFormattingContext::new(&frame, pending_table_offset)))
        }
        FfiFormattingContextType::Svg => {
            FcImpl::Svg(Box::new(SvgFormattingContext::new(state, box_, layout_mode, callbacks)))
        }
        FfiFormattingContextType::ReplacedWithChildren => FcImpl::ReplacedWithChildren,
        FfiFormattingContextType::InternalReplaced => FcImpl::InternalReplaced,
        FfiFormattingContextType::InternalDummy => FcImpl::InternalDummy,
        FfiFormattingContextType::Inline => panic!("no Rust implementation for inline formatting contexts"),
    };
    Box::new(FormattingContextInstance { frame, implementation })
}

fn register_table_abspos_descendants(frame: &mut FcFrame, parent: Node) {
    let mut child = frame.callbacks.first_child(parent);
    while !child.is_invalid() {
        let next = frame.callbacks.next_sibling(child);
        let facts = frame.state.node_facts(&frame.callbacks, child);
        if facts.is_box() {
            if facts.is_absolutely_positioned() {
                register_contained_abspos_child(
                    frame.state,
                    &frame.callbacks,
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
                register_table_abspos_descendants(frame, child);
            }
        } else {
            register_table_abspos_descendants(frame, child);
        }
        child = next;
    }
}

fn complete_formatting_context_after_root_box_has_used_size(instance: &mut FormattingContextInstance) {
    if let FcImpl::Block(context) = &instance.implementation {
        context.parent_context_did_dimension_child_root_box();
    }
    let registered_abspos_children_could_never_be_laid_out =
        instance.layout_mode != LayoutMode::Normal || instance.frame.state.is_measurement();
    if registered_abspos_children_could_never_be_laid_out {
        return;
    }
    match &instance.implementation {
        FcImpl::Block(_) => {}
        FcImpl::Table(_) => {
            let box_ = instance.frame.box_;
            register_table_abspos_descendants(&mut instance.frame, box_);
        }
        FcImpl::Flex(context) => {
            context.parent_did_dimension();
        }
        FcImpl::Grid(context) => {
            context.parent_did_dimension();
        }
        FcImpl::Svg(_) | FcImpl::ReplacedWithChildren => {}
        FcImpl::InternalReplaced | FcImpl::InternalDummy => return,
    }
    let box_ = instance.box_;
    if instance.frame.state.abspos_layout_pass_is_active() {
        layout_contained_abspos_children(&mut instance.frame);
    } else {
        instance.frame.state.enqueue_for_abspos_layout_pass(box_);
    }
}

impl Drop for FormattingContextInstance<'_> {
    fn drop(&mut self) {
        if let FcImpl::Block(context) = &self.implementation {
            debug_assert!(
                context.was_notified_after_parent_dimensioned_root(),
                "block formatting context dropped without being completed"
            );
        }
    }
}

fn run_formatting_context<'pass>(
    instance: &mut FormattingContextInstance<'pass>,
    input: LayoutInput,
    parent_block: Option<&BlockFormattingContext<'pass>>,
) {
    let FormattingContextInstance { frame, implementation } = instance;
    match implementation {
        FcImpl::Block(context) => {
            context.run(frame, input);
            frame.automatic_content_inline_size = context.automatic_content_inline_size();
            frame.automatic_content_block_size = context.automatic_content_block_size();
        }
        FcImpl::Flex(context) => {
            context.run(frame, input);
            frame.automatic_content_inline_size = context.automatic_content_inline_size();
            frame.automatic_content_block_size = context.automatic_content_block_size();
        }
        FcImpl::Grid(context) => {
            context.run(frame, input);
            frame.automatic_content_inline_size = context.automatic_content_inline_size();
            frame.automatic_content_block_size = context.automatic_content_block_size();
        }
        FcImpl::Table(context) => {
            context.run(frame, parent_block, input);
            frame.automatic_content_inline_size = context.automatic_content_inline_size();
            frame.automatic_content_block_size = context.automatic_content_block_size;
            if context.should_publish_pending_table_offset
                && let Some(parent) = parent_block
            {
                parent.set_pending_table_box_content_offset_in_wrapper(context.pending_table_offset);
            }
        }
        FcImpl::Svg(context) => {
            context.run(frame, input);
        }
        FcImpl::ReplacedWithChildren => {
            run(frame, input);
        }
        FcImpl::InternalReplaced | FcImpl::InternalDummy => {}
    }
}

pub(crate) fn layout_inside_child<'pass>(
    frame: &mut FcFrame<'pass>,
    parent_block: Option<&BlockFormattingContext<'pass>>,
    parent_grid: Option<&GridFormattingContext<'pass>>,
    child: Node,
    layout_mode: LayoutMode,
    input: LayoutInput,
    force_independent_context_run: bool,
) -> ChildLayoutOutcome<'pass> {
    let facts = frame.state.node_facts(&frame.callbacks, child);
    let used = frame.state.try_used_values(&frame.callbacks, child);
    if !force_independent_context_run
        && layout_mode == LayoutMode::IntrinsicSizing
        && !facts.is_inline()
        && used.is_some_and(|used| {
            used.inline_size_constraint.get() == SizeConstraint::None
                && used.block_size_constraint.get() == SizeConstraint::None
                && used.has_definite_inline_size()
                && used.has_definite_block_size()
        })
    {
        return ChildLayoutOutcome::Skipped;
    }
    let creates_replaced_context = matches!(
        formatting_context_type_created_by_box(facts),
        Some(FfiFormattingContextType::InternalReplaced | FfiFormattingContextType::ReplacedWithChildren)
    );
    if !facts.can_have_children() && !creates_replaced_context {
        return ChildLayoutOutcome::Skipped;
    }

    let fc_type = formatting_context_type_created_by_box(facts).or_else(|| {
        force_independent_context_run.then(|| independent_formatting_context_type(frame.state, child, &frame.callbacks))
    });
    let Some(fc_type) = fc_type else {
        if force_independent_context_run {
            return ChildLayoutOutcome::Skipped;
        }
        return ChildLayoutOutcome::ReenterCurrent;
    };
    let mut context = create_formatting_context(
        frame.state,
        child,
        FcParents {
            block: parent_block,
            grid: parent_grid,
        },
        fc_type,
        layout_mode,
        frame.should_collect_devtools_layout_data,
        frame.callbacks,
    );
    run_formatting_context(&mut context, input, parent_block);
    ChildLayoutOutcome::Created(PendingChildLayout { context })
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

/// # Safety
///
/// The callback table and commit sink must remain valid for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn rust_layout_run_root_layout(
    root: NodeSlotId,
    document_element_layout_node: NodeSlotId,
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
        state.record_precreated_used_values(&callbacks, root);
        viewport_used.set_content_inline_size(viewport_inline_size);
        viewport_used.set_content_block_size(viewport_block_size);

        let mut root_for_layout = root;
        let has_initial_containing_block = !document_element_layout_node.is_invalid();
        if has_initial_containing_block {
            let icb_used = state.create_used_values(&callbacks, document_element_layout_node, root_constraints);
            state.record_precreated_used_values(&callbacks, document_element_layout_node);
            icb_used.set_content_inline_size(viewport_inline_size);
        }

        let first_child = callbacks.first_child(root);
        if !first_child.is_invalid() && state.node_facts(&callbacks, first_child).is_svg_svg_box() {
            // Standalone SVG documents use the viewport size for the root
            // SVG container and enter SVG layout directly.
            let svg_root_used = state.used_values(&callbacks, first_child);
            svg_root_used.set_content_block_size(viewport_used.content_block_size.get());
            root_for_layout = first_child;
        }
        let input = LayoutInput {
            available_space: AvailableSpace {
                inline_size: AvailableSize::definite(viewport_inline_size),
                block_size: AvailableSize::definite(viewport_block_size),
            },
            containing_block_constraints: crate::layout::ContainingBlockConstraints::default(),
            content_box_position_in_bfc_root: None,
            table_grid_min_border_box_block_size: None,
        };
        let state_ref = &state;
        let fc_type = independent_formatting_context_type(state_ref, root_for_layout, &callbacks);
        let mut context = create_formatting_context(
            state_ref,
            root_for_layout,
            FcParents::default(),
            fc_type,
            LayoutMode::Normal,
            should_collect_devtools_layout_data,
            callbacks,
        );
        run_formatting_context(&mut context, input, None);
        complete_formatting_context_after_root_box_has_used_size(&mut context);
        drop(context);
        run_abspos_layout_pass(state_ref, callbacks, should_collect_devtools_layout_data);
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
            let _ = state.populate_from_paintable(&callbacks, viewport, std::ptr::null_mut());
        }
        let input = LayoutInput {
            available_space: AvailableSpace {
                inline_size: AvailableSize::definite(root_used.content_inline_size.get()),
                block_size: AvailableSize::definite(root_used.content_block_size.get()),
            },
            // The subtree root has definite sizes in both axes, so boxes
            // below it do not need inherited percentage constraints.
            containing_block_constraints: crate::layout::ContainingBlockConstraints::default(),
            content_box_position_in_bfc_root: None,
            table_grid_min_border_box_block_size: None,
        };

        let state_ref = &state;
        let facts = state.node_facts(&callbacks, root);
        let fc_type = formatting_context_type_created_by_box(facts)
            .expect("partial relayout root must establish an independent formatting context");
        let mut context = create_formatting_context(
            state_ref,
            root,
            FcParents::default(),
            fc_type,
            LayoutMode::Normal,
            false,
            callbacks,
        );
        run_formatting_context(&mut context, input, None);

        complete_formatting_context_after_root_box_has_used_size(&mut context);
        drop(context);
        run_abspos_layout_pass(state_ref, callbacks, false);
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
        let mut frame = crate::layout::FcFrame::new(state_ref, containing_block, LayoutMode::Normal, callbacks, false);
        AbsposEngine::new(state_ref, callbacks).replay(&mut frame, box_);
        run_abspos_layout_pass(state_ref, callbacks, false);
        state.commit_replacing(box_, paintable_to_replace, &callbacks, sink);
    });
}
